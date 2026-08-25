#pragma once

#include <windows.h>

#include <cstdint>
#include <filesystem>
#include <string_view>

namespace qrec::update::detail {

enum class InstalledHealthOutcome : std::uint8_t {
    Ready,
    ProcessExited,
    Timeout,
    WaitFailed,
};

class InstalledProcess final {
public:
    InstalledProcess() noexcept = default;
    ~InstalledProcess();
    InstalledProcess(const InstalledProcess&) = delete;
    InstalledProcess& operator=(const InstalledProcess&) = delete;
    InstalledProcess(InstalledProcess&& other) noexcept;
    InstalledProcess& operator=(InstalledProcess&& other) noexcept;

    [[nodiscard]] bool IsValid() const noexcept;
    [[nodiscard]] HANDLE ProcessHandle() const noexcept;
    [[nodiscard]] HANDLE HealthEventHandle() const noexcept;
    void Reset(HANDLE processHandle = nullptr, HANDLE healthEventHandle = nullptr) noexcept;

private:
    HANDLE processHandle_{};
    HANDLE healthEventHandle_{};
};

[[nodiscard]] bool WaitForProcessExit(
    std::uint32_t processId,
    DWORD timeoutMilliseconds,
    DWORD* error) noexcept;

[[nodiscard]] bool LaunchBootstrapExecutable(
    const std::filesystem::path& downloadedExecutable,
    const std::filesystem::path& targetExecutable,
    std::uint32_t parentProcessId,
    DWORD* error);

[[nodiscard]] bool LaunchInstalledExecutable(
    const std::filesystem::path& targetExecutable,
    const std::filesystem::path& bootstrapExecutable,
    InstalledProcess* installedProcess,
    DWORD* error);

[[nodiscard]] InstalledHealthOutcome WaitForInstalledHealth(
    const InstalledProcess& installedProcess,
    DWORD timeoutMilliseconds,
    DWORD* error) noexcept;

[[nodiscard]] bool TerminateInstalledProcess(
    const InstalledProcess& installedProcess,
    DWORD timeoutMilliseconds,
    DWORD* error) noexcept;

[[nodiscard]] bool LaunchExecutableNormally(
    const std::filesystem::path& executable,
    DWORD* error);

[[nodiscard]] bool SignalHealthEvent(
    std::wstring_view healthEventName,
    DWORD* error);

}  // namespace qrec::update::detail
