#include "common/Win32Helpers.h"

#include "common/ProductInfo.h"

#include <knownfolders.h>
#include <shellapi.h>
#include <shlobj.h>

#include <array>
#include <format>
#include <system_error>

namespace qrec::win32 {
namespace {

std::filesystem::path KnownFolder(REFKNOWNFOLDERID id) {
    PWSTR rawPath = nullptr;
    const HRESULT result = ::SHGetKnownFolderPath(id, KF_FLAG_DEFAULT, nullptr, &rawPath);
    if (FAILED(result) || rawPath == nullptr) {
        return {};
    }
    const std::filesystem::path path(rawPath);
    ::CoTaskMemFree(rawPath);
    return path;
}

}  // namespace

std::wstring FormatError(const HRESULT result) {
    LPWSTR message = nullptr;
    const DWORD length = ::FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
            FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr,
        static_cast<DWORD>(result),
        MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
        reinterpret_cast<LPWSTR>(&message),
        0,
        nullptr);
    std::wstring text = length > 0 && message != nullptr
        ? std::wstring(message, length)
        : std::format(L"HRESULT 0x{:08X}", static_cast<unsigned long>(result));
    if (message != nullptr) {
        ::LocalFree(message);
    }
    while (!text.empty() && (text.back() == L'\r' || text.back() == L'\n')) {
        text.pop_back();
    }
    return text;
}

std::wstring FormatLastError(const DWORD error) {
    return FormatError(HRESULT_FROM_WIN32(error));
}

std::filesystem::path LocalAppDataDirectory() {
    std::filesystem::path base = KnownFolder(FOLDERID_LocalAppData);
    if (base.empty()) {
        std::array<wchar_t, MAX_PATH> buffer{};
        const DWORD length = ::GetEnvironmentVariableW(
            L"LOCALAPPDATA", buffer.data(), static_cast<DWORD>(buffer.size()));
        if (length > 0 && length < buffer.size()) {
            base = std::filesystem::path(buffer.data());
        } else {
            base = std::filesystem::temp_directory_path();
        }
    }
    return base / product::LocalDataDirectoryName;
}

std::filesystem::path LegacyLocalAppDataDirectory() {
    std::filesystem::path base = KnownFolder(FOLDERID_LocalAppData);
    if (base.empty()) {
        std::array<wchar_t, MAX_PATH> buffer{};
        const DWORD length = ::GetEnvironmentVariableW(
            L"LOCALAPPDATA", buffer.data(), static_cast<DWORD>(buffer.size()));
        if (length > 0 && length < buffer.size()) {
            base = std::filesystem::path(buffer.data());
        } else {
            base = std::filesystem::temp_directory_path();
        }
    }
    return base / product::LegacyLocalDataDirectoryName;
}

std::filesystem::path DefaultVideoDirectory() {
    std::filesystem::path base = KnownFolder(FOLDERID_Videos);
    if (base.empty()) {
        base = std::filesystem::temp_directory_path();
    }
    return base / product::Name;
}

std::filesystem::path CurrentExecutablePath(DWORD* error) noexcept {
    if (error != nullptr) {
        *error = ERROR_SUCCESS;
    }
    std::array<wchar_t, 32'768> buffer{};
    const DWORD length = ::GetModuleFileNameW(
        nullptr,
        buffer.data(),
        static_cast<DWORD>(buffer.size()));
    if (length == 0 || length >= buffer.size()) {
        if (error != nullptr) {
            *error = length == 0 ? ::GetLastError() : ERROR_INSUFFICIENT_BUFFER;
        }
        return {};
    }
    try {
        return std::filesystem::path(std::wstring_view(buffer.data(), length));
    } catch (...) {
        if (error != nullptr) {
            *error = ERROR_NOT_ENOUGH_MEMORY;
        }
        return {};
    }
}

std::wstring TimestampForFileName() {
    SYSTEMTIME local{};
    ::GetLocalTime(&local);
    return std::format(
        L"{:04}{:02}{:02}_{:02}{:02}{:02}",
        local.wYear,
        local.wMonth,
        local.wDay,
        local.wHour,
        local.wMinute,
        local.wSecond);
}

std::filesystem::path MakeUniquePath(
    const std::filesystem::path& directory,
    const std::wstring_view stem,
    const std::wstring_view extension,
    std::wstring* error) noexcept {
    try {
        const std::wstring timestamp = TimestampForFileName();
        for (int suffix = 1; suffix <= 10'000; ++suffix) {
            const std::wstring fileName = suffix == 1
                ? std::format(L"{}_{}{}", stem, timestamp, extension)
                : std::format(L"{}_{}_{}{}", stem, timestamp, suffix, extension);
            const std::filesystem::path candidate = directory / fileName;
            std::error_code code;
            const bool exists = std::filesystem::exists(candidate, code);
            if (code) {
                if (error != nullptr) {
                    *error = L"无法检查目标文件名：" + candidate.wstring() +
                        L"\n错误码：" + std::to_wstring(code.value());
                }
                return {};
            }
            if (!exists) {
                return candidate;
            }
        }
        if (error != nullptr) {
            *error = L"同一时间戳下的文件名已用尽，请稍后重试。";
        }
    } catch (const std::exception&) {
        if (error != nullptr) {
            *error = L"生成文件名失败，系统内存或路径状态异常。";
        }
    } catch (...) {
        if (error != nullptr) {
            *error = L"生成文件名失败，系统内存或路径状态异常。";
        }
    }
    return {};
}

bool EnsureDirectory(const std::filesystem::path& directory, std::wstring* error) {
    std::error_code code;
    if (std::filesystem::exists(directory, code)) {
        if (std::filesystem::is_directory(directory, code)) {
            return true;
        }
        if (error != nullptr) {
            *error = L"目标路径不是文件夹：" + directory.wstring();
        }
        return false;
    }
    if (std::filesystem::create_directories(directory, code)) {
        return true;
    }
    if (error != nullptr) {
        *error = L"无法创建文件夹：" + directory.wstring() + L"\n错误码：" +
            std::to_wstring(code.value());
    }
    return false;
}

int ScaleForDpi(const int value, const UINT dpi) noexcept {
    return ::MulDiv(value, static_cast<int>(dpi), USER_DEFAULT_SCREEN_DPI);
}

void ShowError(HWND owner, const std::wstring_view title, const std::wstring_view message) {
    ::MessageBoxW(owner, std::wstring(message).c_str(), std::wstring(title).c_str(),
                  MB_OK | MB_ICONERROR | MB_SETFOREGROUND);
}

void ShowInfo(HWND owner, const std::wstring_view title, const std::wstring_view message) {
    ::MessageBoxW(owner, std::wstring(message).c_str(), std::wstring(title).c_str(),
                  MB_OK | MB_ICONINFORMATION | MB_SETFOREGROUND);
}

bool OpenDirectory(const std::filesystem::path& directory) {
    const HINSTANCE result = ::ShellExecuteW(
        nullptr, L"open", directory.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    return reinterpret_cast<INT_PTR>(result) > 32;
}

ScopedCoInitialize::ScopedCoInitialize(const DWORD flags) noexcept
    : result_(::CoInitializeEx(nullptr, flags)) {}

ScopedCoInitialize::~ScopedCoInitialize() {
    if (SUCCEEDED(result_)) {
        ::CoUninitialize();
    }
}

}  // namespace qrec::win32
