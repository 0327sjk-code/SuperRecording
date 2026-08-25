#pragma once

#include <windows.h>
#include <objbase.h>

#include <filesystem>
#include <string>
#include <string_view>

namespace qrec::win32 {

[[nodiscard]] std::wstring FormatError(HRESULT result);
[[nodiscard]] std::wstring FormatLastError(DWORD error = ::GetLastError());
[[nodiscard]] std::filesystem::path LocalAppDataDirectory();
[[nodiscard]] std::filesystem::path LegacyLocalAppDataDirectory();
[[nodiscard]] std::filesystem::path DefaultVideoDirectory();
[[nodiscard]] std::filesystem::path CurrentExecutablePath(
    DWORD* error = nullptr) noexcept;
[[nodiscard]] std::wstring TimestampForFileName();
[[nodiscard]] std::filesystem::path MakeUniquePath(
    const std::filesystem::path& directory,
    std::wstring_view stem,
    std::wstring_view extension,
    std::wstring* error = nullptr) noexcept;
[[nodiscard]] bool EnsureDirectory(const std::filesystem::path& directory, std::wstring* error = nullptr);
[[nodiscard]] int ScaleForDpi(int value, UINT dpi) noexcept;
void ShowError(HWND owner, std::wstring_view title, std::wstring_view message);
void ShowInfo(HWND owner, std::wstring_view title, std::wstring_view message);
bool OpenDirectory(const std::filesystem::path& directory);

class ScopedCoInitialize final {
public:
    explicit ScopedCoInitialize(DWORD flags = COINIT_APARTMENTTHREADED) noexcept;
    ~ScopedCoInitialize();
    ScopedCoInitialize(const ScopedCoInitialize&) = delete;
    ScopedCoInitialize& operator=(const ScopedCoInitialize&) = delete;
    [[nodiscard]] HRESULT Result() const noexcept { return result_; }

private:
    HRESULT result_{};
};

}  // namespace qrec::win32
