#include "update/detail/HttpsUrl.h"

#include "update/detail/UpdateError.h"

#include <windows.h>
#include <winhttp.h>

#include <limits>
#include <vector>

namespace qrec::update::detail {

bool HasInvalidHeaderCharacters(const std::wstring_view value) noexcept {
    return value.find(L'\0') != std::wstring_view::npos ||
        value.find(L'\r') != std::wstring_view::npos ||
        value.find(L'\n') != std::wstring_view::npos;
}

std::optional<ParsedHttpsUrl> ParseHttpsUrl(
    const std::wstring_view url,
    UpdateFailure* const failure) {
    if (url.empty() ||
        url.size() > std::numeric_limits<DWORD>::max() ||
        HasInvalidHeaderCharacters(url)) {
        if (failure != nullptr) {
            *failure = MakeFailure(
                UpdateErrorCode::InvalidUrl,
                L"The update URL is empty, malformed, or too long.");
        }
        return std::nullopt;
    }

    URL_COMPONENTS components{};
    components.dwStructSize = sizeof(components);
    components.dwHostNameLength = static_cast<DWORD>(-1);
    components.dwUrlPathLength = static_cast<DWORD>(-1);
    components.dwExtraInfoLength = static_cast<DWORD>(-1);
    components.dwUserNameLength = static_cast<DWORD>(-1);
    components.dwPasswordLength = static_cast<DWORD>(-1);
    if (!::WinHttpCrackUrl(
            url.data(),
            static_cast<DWORD>(url.size()),
            0,
            &components)) {
        if (failure != nullptr) {
            *failure = MakeNativeFailure(
                UpdateErrorCode::InvalidUrl,
                L"WinHttpCrackUrl failed",
                ::GetLastError());
        }
        return std::nullopt;
    }
    if (components.nScheme != INTERNET_SCHEME_HTTPS ||
        components.lpszHostName == nullptr ||
        components.dwHostNameLength == 0 ||
        components.dwUserNameLength != 0 ||
        components.dwPasswordLength != 0) {
        if (failure != nullptr) {
            *failure = MakeFailure(
                UpdateErrorCode::InvalidUrl,
                L"Only credential-free HTTPS update URLs are accepted.");
        }
        return std::nullopt;
    }

    ParsedHttpsUrl parsed{};
    parsed.host.assign(
        components.lpszHostName,
        components.dwHostNameLength);
    parsed.port = components.nPort;
    if (components.lpszUrlPath != nullptr &&
        components.dwUrlPathLength != 0) {
        parsed.objectName.assign(
            components.lpszUrlPath,
            components.dwUrlPathLength);
    } else {
        parsed.objectName = L"/";
    }
    if (components.lpszExtraInfo != nullptr &&
        components.dwExtraInfoLength != 0) {
        const std::wstring_view extra(
            components.lpszExtraInfo,
            components.dwExtraInfoLength);
        if (extra.find(L'#') != std::wstring_view::npos) {
            if (failure != nullptr) {
                *failure = MakeFailure(
                    UpdateErrorCode::InvalidUrl,
                    L"URL fragments are not accepted for update downloads.");
            }
            return std::nullopt;
        }
        parsed.objectName.append(extra);
    }
    return parsed;
}

bool ValidateFinalRequestUrl(
    void* const requestHandle,
    UpdateFailure* const failure) {
    DWORD byteCount = 0;
    if (::WinHttpQueryOption(
            requestHandle, WINHTTP_OPTION_URL, nullptr, &byteCount)) {
        if (failure != nullptr) {
            *failure = MakeFailure(
                UpdateErrorCode::Network,
                L"WinHTTP returned an empty final URL.");
        }
        return false;
    }
    const DWORD queryError = ::GetLastError();
    if (queryError != ERROR_INSUFFICIENT_BUFFER ||
        byteCount < sizeof(wchar_t)) {
        if (failure != nullptr) {
            *failure = MakeNativeFailure(
                UpdateErrorCode::Network,
                L"Could not query the final response URL",
                queryError);
        }
        return false;
    }

    std::vector<wchar_t> buffer(
        static_cast<std::size_t>(byteCount / sizeof(wchar_t)) + 1U,
        L'\0');
    if (!::WinHttpQueryOption(
            requestHandle,
            WINHTTP_OPTION_URL,
            buffer.data(),
            &byteCount)) {
        if (failure != nullptr) {
            *failure = MakeNativeFailure(
                UpdateErrorCode::Network,
                L"Could not read the final response URL",
                ::GetLastError());
        }
        return false;
    }

    UpdateFailure parseFailure;
    if (!ParseHttpsUrl(buffer.data(), &parseFailure).has_value()) {
        if (failure != nullptr) {
            *failure = std::move(parseFailure);
        }
        return false;
    }
    return true;
}

}  // namespace qrec::update::detail
