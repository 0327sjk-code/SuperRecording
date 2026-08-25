#pragma once

#include <cstdint>
#include <filesystem>
#include <string>

namespace qrec::app {

namespace command_line {

inline constexpr wchar_t ApplyUpdateArgument[] = L"--apply-update";
inline constexpr wchar_t TargetExecutablePrefix[] = L"--target-exe=";
inline constexpr wchar_t ParentProcessIdPrefix[] = L"--parent-pid=";

// Internal hand-off arguments used by the newly installed process to remove
// the bootstrap executable after the bootstrap process has exited.
inline constexpr wchar_t CleanupUpdatePrefix[] = L"--cleanup-update=";
inline constexpr wchar_t UpdateBootstrapProcessIdPrefix[] =
    L"--update-bootstrap-pid=";
inline constexpr wchar_t UpdateHealthEventPrefix[] =
    L"--update-health-event=";
inline constexpr wchar_t UpdateHealthEventNamePrefix[] =
    L"Local\\SuperRecording.UpdateHealth.";

}  // namespace command_line

struct CommandLineOptions final {
    bool valid{true};
    bool launchedAtStartup{};
    bool applyUpdate{};
    std::filesystem::path targetExecutable;
    std::uint32_t parentProcessId{};
    std::filesystem::path cleanupUpdateExecutable;
    std::uint32_t updateBootstrapProcessId{};
    std::wstring updateHealthEventName;
    std::wstring errorMessage;

    [[nodiscard]] bool HasCleanupRequest() const noexcept {
        return !cleanupUpdateExecutable.empty() &&
            updateBootstrapProcessId != 0 &&
            !updateHealthEventName.empty();
    }
};

[[nodiscard]] CommandLineOptions ParseCommandLine() noexcept;

}  // namespace qrec::app
