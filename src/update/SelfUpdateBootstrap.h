#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

namespace qrec::update {

enum class BootstrapStage : std::uint8_t {
    None,
    ValidateRequest,
    WaitForPreviousProcess,
    PreservePreviousExecutable,
    InstallExecutable,
    LaunchBootstrapExecutable,
    LaunchInstalledExecutable,
    WaitForInstalledExecutableHealth,
    SignalInstalledExecutableReady,
    RestartPreviousExecutable,
    Rollback,
};

struct ApplyUpdateRequest final {
    std::filesystem::path targetExecutable;
    std::uint32_t parentProcessId{};
};

struct BootstrapResult final {
    bool success{};
    BootstrapStage stage{BootstrapStage::None};
    std::uint32_t nativeError{};
    std::uint32_t rollbackError{};
    std::wstring message;
};

// Runs inside the downloaded executable. The caller must invoke this before
// creating the application's single-instance mutex.
[[nodiscard]] BootstrapResult ApplyUpdate(
    const ApplyUpdateRequest& request) noexcept;

// Runs in the normal application after a verified download is ready. It starts
// that executable in bootstrap mode; the caller can then close the old app.
[[nodiscard]] BootstrapResult LaunchApplyUpdate(
    const std::filesystem::path& downloadedExecutable,
    const std::filesystem::path& targetExecutable,
    std::uint32_t parentProcessId) noexcept;

// Signals that the installed process completed application initialization.
// Main calls this after AppController::Initialize succeeds and before Run.
[[nodiscard]] BootstrapResult SignalUpdateReady(
    std::wstring_view healthEventName) noexcept;

// Runs inside the newly installed executable before normal startup. Cleanup is
// deliberately best-effort and never prevents the recorder from launching.
void CleanupUpdateArtifacts(
    const std::filesystem::path& bootstrapExecutable,
    std::uint32_t bootstrapProcessId) noexcept;

}  // namespace qrec::update
