#include "update/GitHubUpdateClient.h"

#include "update/detail/UpdateError.h"
#include "update/detail/UpdateFileStore.h"
#include "update/detail/WinHttpTransport.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <new>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace qrec::update {
namespace {

bool IsAsciiEnvelopeWhitespace(const char character) noexcept {
    return character == ' ' || character == '\t' ||
        character == '\r' || character == '\n';
}

void NotifyCachedProgress(
    const DownloadProgressCallback& callback,
    const std::uint64_t bytes) noexcept {
    if (!callback) {
        return;
    }
    try {
        callback(bytes, bytes);
    } catch (...) {
        // Consumer callback failures do not invalidate an existing cache file.
    }
}

}  // namespace

class GitHubUpdateClient::Impl final {
public:
    explicit Impl(GitHubUpdateClientOptions options)
        : options_(std::move(options)),
          transport_(options_),
          fileStore_(options_.maxExecutableBytes) {}

    [[nodiscard]] VersionFetchResult FetchLatestVersion(
        const std::stop_token stopToken) noexcept {
        try {
            std::scoped_lock operationLock(operationMutex_);
            transport_.BeginOperation();
            if (transport_.CancellationRequested(stopToken)) {
                return {std::nullopt, detail::MakeCancelledFailure()};
            }

            std::vector<std::byte> body;
            const std::uint64_t initialCapacity = std::min<std::uint64_t>(
                options_.maxVersionBytes, 4'096);
            body.reserve(static_cast<std::size_t>(initialCapacity));
            const detail::HttpTransferResult transfer = transport_.Get(
                options_.versionUrl,
                options_.maxVersionBytes,
                [&body](
                    const std::span<const std::byte> chunk,
                    UpdateFailure*) {
                    body.insert(body.end(), chunk.begin(), chunk.end());
                    return true;
                },
                {},
                stopToken);
            if (transfer.failure.HasError()) {
                return {std::nullopt, transfer.failure};
            }

            std::string text;
            text.reserve(body.size());
            for (const std::byte value : body) {
                const unsigned int byteValue =
                    std::to_integer<unsigned int>(value);
                if (byteValue == 0U || byteValue > 0x7FU) {
                    return {
                        std::nullopt,
                        detail::MakeFailure(
                            UpdateErrorCode::InvalidVersion,
                            L"version.txt must contain an ASCII numeric version.")};
                }
                text.push_back(static_cast<char>(byteValue));
            }

            const auto first = std::find_if_not(
                text.begin(), text.end(), IsAsciiEnvelopeWhitespace);
            const auto last = std::find_if_not(
                text.rbegin(), text.rend(), IsAsciiEnvelopeWhitespace).base();
            if (first >= last) {
                return {
                    std::nullopt,
                    detail::MakeFailure(
                        UpdateErrorCode::InvalidVersion,
                        L"version.txt is empty.")};
            }
            const std::string_view trimmed(
                &*first,
                static_cast<std::size_t>(last - first));
            const std::optional<SemanticVersion> version =
                SemanticVersion::Parse(trimmed);
            if (!version.has_value()) {
                return {
                    std::nullopt,
                    detail::MakeFailure(
                        UpdateErrorCode::InvalidVersion,
                        L"version.txt must contain exactly three or four numeric components.")};
            }
            return {version, {}};
        } catch (const std::bad_alloc&) {
            return {
                std::nullopt,
                detail::MakeFailure(
                    UpdateErrorCode::OutOfMemory,
                    L"Not enough memory to check for updates.")};
        } catch (...) {
            return {
                std::nullopt,
                detail::MakeFailure(
                    UpdateErrorCode::Unexpected,
                    L"Unexpected failure while checking for updates.")};
        }
    }

    [[nodiscard]] ExecutableDownloadResult DownloadExecutable(
        const SemanticVersion& version,
        const DownloadProgressCallback& progress,
        const std::stop_token stopToken) noexcept {
        try {
            std::scoped_lock operationLock(operationMutex_);
            transport_.BeginOperation();
            if (transport_.CancellationRequested(stopToken)) {
                return {{}, 0, false, detail::MakeCancelledFailure()};
            }

            ExecutableDownloadResult result = fileStore_.Acquire(
                version,
                [this, &progress, stopToken](
                    const detail::HttpChunkWriter& writer) {
                    return transport_.Get(
                        options_.executableUrl,
                        options_.maxExecutableBytes,
                        writer,
                        progress,
                        stopToken);
                });
            if (result.Succeeded() && result.reusedCachedFile) {
                NotifyCachedProgress(progress, result.downloadedBytes);
            }
            return result;
        } catch (const std::bad_alloc&) {
            return {
                {}, 0, false,
                detail::MakeFailure(
                    UpdateErrorCode::OutOfMemory,
                    L"Not enough memory to download the update.")};
        } catch (...) {
            return {
                {}, 0, false,
                detail::MakeFailure(
                    UpdateErrorCode::Unexpected,
                    L"Unexpected failure while downloading the update.")};
        }
    }

    void Cancel() noexcept {
        transport_.Cancel();
    }

private:
    GitHubUpdateClientOptions options_;
    detail::WinHttpTransport transport_;
    detail::UpdateFileStore fileStore_;
    std::mutex operationMutex_;
};

GitHubUpdateClient::GitHubUpdateClient(GitHubUpdateClientOptions options)
    : impl_(std::make_unique<Impl>(std::move(options))) {}

GitHubUpdateClient::~GitHubUpdateClient() = default;

VersionFetchResult GitHubUpdateClient::FetchLatestVersion(
    const std::stop_token stopToken) noexcept {
    return impl_->FetchLatestVersion(stopToken);
}

ExecutableDownloadResult GitHubUpdateClient::DownloadExecutable(
    const SemanticVersion& version,
    DownloadProgressCallback progress,
    const std::stop_token stopToken) noexcept {
    return impl_->DownloadExecutable(version, progress, stopToken);
}

void GitHubUpdateClient::Cancel() noexcept {
    impl_->Cancel();
}

}  // namespace qrec::update
