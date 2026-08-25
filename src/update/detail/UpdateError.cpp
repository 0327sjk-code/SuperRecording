#include "update/detail/UpdateError.h"

#include <windows.h>

#include <utility>

namespace qrec::update::detail {
namespace {

std::wstring FormatNativeError(const std::uint32_t code) {
    wchar_t* message = nullptr;
    const DWORD length = ::FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER |
            FORMAT_MESSAGE_FROM_SYSTEM |
            FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr,
        code,
        MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
        reinterpret_cast<wchar_t*>(&message),
        0,
        nullptr);
    if (length == 0 || message == nullptr) {
        return L"Windows error " + std::to_wstring(code);
    }

    std::wstring result(message, length);
    static_cast<void>(::LocalFree(message));
    while (!result.empty() &&
           (result.back() == L'\r' || result.back() == L'\n' ||
            result.back() == L' ' || result.back() == L'\t')) {
        result.pop_back();
    }
    return result;
}

}  // namespace

UpdateFailure MakeFailure(
    const UpdateErrorCode code,
    std::wstring message,
    const std::uint32_t nativeCode,
    const std::uint32_t httpStatus) {
    UpdateFailure failure{};
    failure.code = code;
    failure.nativeCode = nativeCode;
    failure.httpStatus = httpStatus;
    failure.message = std::move(message);
    return failure;
}

UpdateFailure MakeNativeFailure(
    const UpdateErrorCode code,
    const std::wstring_view context,
    const std::uint32_t nativeCode) {
    return MakeFailure(
        code,
        std::wstring(context) + L": " + FormatNativeError(nativeCode),
        nativeCode);
}

UpdateFailure MakeCancelledFailure() {
    return MakeFailure(
        UpdateErrorCode::Cancelled,
        L"Update operation cancelled.");
}

}  // namespace qrec::update::detail
