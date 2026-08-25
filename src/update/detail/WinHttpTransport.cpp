#include "update/detail/WinHttpTransport.h"

#include "update/detail/HttpsUrl.h"
#include "update/detail/UpdateError.h"

#include <windows.h>
#include <winhttp.h>

#include <array>
#include <atomic>
#include <chrono>
#include <limits>
#include <new>
#include <utility>

#pragma comment(lib, "winhttp.lib")

namespace qrec::update::detail {
namespace {

constexpr std::size_t kTransferBufferBytes = 64U * 1024U;
constexpr wchar_t kAcceptHeaders[] =
    L"Accept: */*\r\n"
    L"Accept-Encoding: identity\r\n"
    L"Cache-Control: no-cache\r\n";

class InternetHandle final {
public:
    InternetHandle() noexcept = default;
    explicit InternetHandle(const HINTERNET handle) noexcept : handle_(handle) {}
    ~InternetHandle() { Reset(); }
    InternetHandle(const InternetHandle&) = delete;
    InternetHandle& operator=(const InternetHandle&) = delete;
    [[nodiscard]] HINTERNET Get() const noexcept { return handle_; }
    [[nodiscard]] explicit operator bool() const noexcept {
        return handle_ != nullptr;
    }
    void Reset(const HINTERNET handle = nullptr) noexcept {
        if (handle_ != nullptr) {
            static_cast<void>(::WinHttpCloseHandle(handle_));
        }
        handle_ = handle;
    }

private:
    HINTERNET handle_{};
};

class ActiveRequest final {
public:
    ~ActiveRequest() { Close(); }
    ActiveRequest() = default;
    ActiveRequest(const ActiveRequest&) = delete;
    ActiveRequest& operator=(const ActiveRequest&) = delete;
    void Set(const HINTERNET handle) noexcept {
        const HINTERNET previous = handle_.exchange(
            handle, std::memory_order_acq_rel);
        if (previous != nullptr) {
            static_cast<void>(::WinHttpCloseHandle(previous));
        }
    }
    void Close() noexcept {
        const HINTERNET handle = handle_.exchange(
            nullptr, std::memory_order_acq_rel);
        if (handle != nullptr) {
            static_cast<void>(::WinHttpCloseHandle(handle));
        }
    }

private:
    std::atomic<HINTERNET> handle_{};
};

class ActiveRequestScope final {
public:
    explicit ActiveRequestScope(ActiveRequest& request) noexcept
        : request_(request) {}
    ~ActiveRequestScope() { request_.Close(); }
    ActiveRequestScope(const ActiveRequestScope&) = delete;
    ActiveRequestScope& operator=(const ActiveRequestScope&) = delete;

private:
    ActiveRequest& request_;
};

std::optional<int> ToNativeTimeout(
    const std::chrono::milliseconds timeout) noexcept {
    const auto count = timeout.count();
    if (count <= 0 || count > std::numeric_limits<int>::max()) {
        return std::nullopt;
    }
    return static_cast<int>(count);
}

std::optional<std::uint64_t> QueryContentLength(
    const HINTERNET request) noexcept {
    DWORD length = 0;
    DWORD bufferBytes = sizeof(length);
    if (!::WinHttpQueryHeaders(
            request,
            WINHTTP_QUERY_CONTENT_LENGTH | WINHTTP_QUERY_FLAG_NUMBER,
            WINHTTP_HEADER_NAME_BY_INDEX,
            &length,
            &bufferBytes,
            WINHTTP_NO_HEADER_INDEX)) {
        return std::nullopt;
    }
    return static_cast<std::uint64_t>(length);
}

void NotifyProgress(
    const DownloadProgressCallback& callback,
    const std::uint64_t bytes,
    const std::optional<std::uint64_t> total) noexcept {
    if (!callback) {
        return;
    }
    try {
        callback(bytes, total);
    } catch (...) {
        // Consumer callbacks cannot abort or corrupt a download.
    }
}

}  // namespace

class WinHttpTransport::Impl final {
public:
    explicit Impl(const GitHubUpdateClientOptions& options)
        : userAgent_(options.userAgent),
          timeouts_(options.timeouts),
          maxRedirects_(options.maxRedirects),
          progressInterval_(options.progressInterval) {}

    void BeginOperation() noexcept {
        activeRequest_.Close();
        cancelled_.store(false, std::memory_order_release);
    }

    void Cancel() noexcept {
        cancelled_.store(true, std::memory_order_release);
        activeRequest_.Close();
    }

    [[nodiscard]] bool CancellationRequested(
        const std::stop_token stopToken) const noexcept {
        return stopToken.stop_requested() ||
            cancelled_.load(std::memory_order_acquire);
    }

    [[nodiscard]] HttpTransferResult Get(
        const std::wstring_view url,
        const std::uint64_t maximumBytes,
        const HttpChunkWriter& writer,
        const DownloadProgressCallback& progress,
        const std::stop_token stopToken) noexcept {
        try {
            return GetImpl(
                url, maximumBytes, writer, progress, stopToken);
        } catch (const std::bad_alloc&) {
            HttpTransferResult result{};
            result.failure = MakeFailure(
                UpdateErrorCode::OutOfMemory,
                L"Not enough memory for the update HTTP request.");
            return result;
        } catch (...) {
            HttpTransferResult result{};
            result.failure = MakeFailure(
                UpdateErrorCode::Unexpected,
                L"Unexpected failure in the update HTTP transport.");
            return result;
        }
    }

private:
    [[nodiscard]] bool ValidateConfiguration(
        const std::wstring_view url,
        const std::uint64_t maximumBytes,
        UpdateFailure* const failure) const {
        UpdateFailure urlFailure;
        if (!ParseHttpsUrl(url, &urlFailure).has_value()) {
            if (failure != nullptr) {
                *failure = std::move(urlFailure);
            }
            return false;
        }
        if (userAgent_.empty() ||
            HasInvalidHeaderCharacters(userAgent_) ||
            maximumBytes == 0 ||
            maxRedirects_ == 0 ||
            progressInterval_.count() <= 0 ||
            !ToNativeTimeout(timeouts_.resolve).has_value() ||
            !ToNativeTimeout(timeouts_.connect).has_value() ||
            !ToNativeTimeout(timeouts_.send).has_value() ||
            !ToNativeTimeout(timeouts_.receive).has_value()) {
            if (failure != nullptr) {
                *failure = MakeFailure(
                    UpdateErrorCode::InvalidConfiguration,
                    L"Update limits, redirects, user agent, or timeouts are invalid.");
            }
            return false;
        }
        return true;
    }

    [[nodiscard]] HttpTransferResult GetImpl(
        const std::wstring_view url,
        const std::uint64_t maximumBytes,
        const HttpChunkWriter& writer,
        const DownloadProgressCallback& progress,
        const std::stop_token stopToken) {
        HttpTransferResult result{};
        if (CancellationRequested(stopToken)) {
            result.failure = MakeCancelledFailure();
            return result;
        }
        UpdateFailure configurationFailure;
        if (!ValidateConfiguration(url, maximumBytes, &configurationFailure)) {
            result.failure = std::move(configurationFailure);
            return result;
        }
        const std::optional<ParsedHttpsUrl> parsed =
            ParseHttpsUrl(url, &configurationFailure);

        InternetHandle session(::WinHttpOpen(
            userAgent_.c_str(),
            WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
            WINHTTP_NO_PROXY_NAME,
            WINHTTP_NO_PROXY_BYPASS,
            0));
        if (!session) {
            result.failure = MakeNativeFailure(
                UpdateErrorCode::Network, L"WinHttpOpen failed", ::GetLastError());
            return result;
        }
        DWORD redirectPolicy =
            WINHTTP_OPTION_REDIRECT_POLICY_DISALLOW_HTTPS_TO_HTTP;
        if (!::WinHttpSetOption(
                session.Get(), WINHTTP_OPTION_REDIRECT_POLICY,
                &redirectPolicy, sizeof(redirectPolicy)) ||
            !::WinHttpSetTimeouts(
                session.Get(),
                *ToNativeTimeout(timeouts_.resolve),
                *ToNativeTimeout(timeouts_.connect),
                *ToNativeTimeout(timeouts_.send),
                *ToNativeTimeout(timeouts_.receive))) {
            result.failure = MakeNativeFailure(
                UpdateErrorCode::Network,
                L"Could not configure WinHTTP",
                ::GetLastError());
            return result;
        }

        InternetHandle connection(::WinHttpConnect(
            session.Get(), parsed->host.c_str(), parsed->port, 0));
        if (!connection) {
            result.failure = MakeNativeFailure(
                UpdateErrorCode::Network,
                L"WinHttpConnect failed",
                ::GetLastError());
            return result;
        }
        const HINTERNET request = ::WinHttpOpenRequest(
            connection.Get(), L"GET", parsed->objectName.c_str(), nullptr,
            WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
            WINHTTP_FLAG_SECURE | WINHTTP_FLAG_REFRESH);
        if (request == nullptr) {
            result.failure = MakeNativeFailure(
                UpdateErrorCode::Network,
                L"WinHttpOpenRequest failed",
                ::GetLastError());
            return result;
        }
        activeRequest_.Set(request);
        ActiveRequestScope requestScope(activeRequest_);
        DWORD redirectLimit = maxRedirects_;
        if (!::WinHttpSetOption(
                request, WINHTTP_OPTION_MAX_HTTP_AUTOMATIC_REDIRECTS,
                &redirectLimit, sizeof(redirectLimit))) {
            result.failure = MakeNativeFailure(
                UpdateErrorCode::Network,
                L"Could not configure the redirect limit",
                ::GetLastError());
            return result;
        }
        if (CancellationRequested(stopToken)) {
            result.failure = MakeCancelledFailure();
            return result;
        }
        if (!::WinHttpSendRequest(
                request, kAcceptHeaders, static_cast<DWORD>(-1),
                WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
            !::WinHttpReceiveResponse(request, nullptr)) {
            result.failure = CancellationRequested(stopToken)
                ? MakeCancelledFailure()
                : MakeNativeFailure(
                    UpdateErrorCode::Network,
                    L"The update HTTP request failed",
                    ::GetLastError());
            return result;
        }
        UpdateFailure finalUrlFailure;
        if (!ValidateFinalRequestUrl(request, &finalUrlFailure)) {
            result.failure = std::move(finalUrlFailure);
            return result;
        }

        DWORD status = 0;
        DWORD statusBytes = sizeof(status);
        if (!::WinHttpQueryHeaders(
                request,
                WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                WINHTTP_HEADER_NAME_BY_INDEX,
                &status,
                &statusBytes,
                WINHTTP_NO_HEADER_INDEX)) {
            result.failure = MakeNativeFailure(
                UpdateErrorCode::Network,
                L"Could not query the update HTTP status",
                ::GetLastError());
            return result;
        }
        if (status != HTTP_STATUS_OK) {
            result.failure = MakeFailure(
                UpdateErrorCode::HttpStatus,
                L"The update server returned HTTP " +
                    std::to_wstring(status) + L'.',
                0,
                status);
            return result;
        }

        result.contentLength = QueryContentLength(request);
        if (result.contentLength.has_value() &&
            *result.contentLength > maximumBytes) {
            result.failure = MakeFailure(
                UpdateErrorCode::ResponseTooLarge,
                L"The update response exceeds the configured size limit.");
            return result;
        }

        NotifyProgress(progress, 0, result.contentLength);
        std::array<std::byte, kTransferBufferBytes> buffer{};
        auto lastProgress = std::chrono::steady_clock::now();
        for (;;) {
            if (CancellationRequested(stopToken)) {
                result.failure = MakeCancelledFailure();
                return result;
            }
            DWORD bytesRead = 0;
            if (!::WinHttpReadData(
                    request, buffer.data(),
                    static_cast<DWORD>(buffer.size()), &bytesRead)) {
                result.failure = CancellationRequested(stopToken)
                    ? MakeCancelledFailure()
                    : MakeNativeFailure(
                        UpdateErrorCode::Network,
                        L"Could not read the update response",
                        ::GetLastError());
                return result;
            }
            if (bytesRead == 0) {
                break;
            }
            const std::uint64_t blockBytes =
                static_cast<std::uint64_t>(bytesRead);
            if (blockBytes > maximumBytes ||
                result.bytesTransferred > maximumBytes - blockBytes) {
                result.failure = MakeFailure(
                    UpdateErrorCode::ResponseTooLarge,
                    L"The update response exceeds the configured size limit.");
                return result;
            }
            UpdateFailure writerFailure;
            if (!writer(
                    std::span<const std::byte>(buffer.data(), bytesRead),
                    &writerFailure)) {
                result.failure = writerFailure.HasError()
                    ? std::move(writerFailure)
                    : MakeFailure(
                        UpdateErrorCode::FileSystem,
                        L"The update response could not be stored.");
                return result;
            }
            result.bytesTransferred += bytesRead;
            const auto now = std::chrono::steady_clock::now();
            if (now - lastProgress >= progressInterval_) {
                NotifyProgress(
                    progress, result.bytesTransferred, result.contentLength);
                lastProgress = now;
            }
        }
        if (result.contentLength.has_value() &&
            result.bytesTransferred != *result.contentLength) {
            result.failure = MakeFailure(
                UpdateErrorCode::Network,
                L"The update response ended before Content-Length was satisfied.");
            return result;
        }
        NotifyProgress(progress, result.bytesTransferred, result.contentLength);
        return result;
    }

    std::wstring userAgent_;
    HttpTimeouts timeouts_;
    std::uint32_t maxRedirects_{};
    std::chrono::milliseconds progressInterval_{};
    ActiveRequest activeRequest_;
    std::atomic_bool cancelled_{};
};

WinHttpTransport::WinHttpTransport(const GitHubUpdateClientOptions& options)
    : impl_(std::make_unique<Impl>(options)) {}
WinHttpTransport::~WinHttpTransport() = default;
void WinHttpTransport::BeginOperation() noexcept { impl_->BeginOperation(); }
void WinHttpTransport::Cancel() noexcept { impl_->Cancel(); }
bool WinHttpTransport::CancellationRequested(
    const std::stop_token token) const noexcept {
    return impl_->CancellationRequested(token);
}
HttpTransferResult WinHttpTransport::Get(
    const std::wstring_view url,
    const std::uint64_t maximumBytes,
    const HttpChunkWriter& writer,
    const DownloadProgressCallback& progress,
    const std::stop_token stopToken) noexcept {
    return impl_->Get(url, maximumBytes, writer, progress, stopToken);
}

}  // namespace qrec::update::detail
