#pragma once

#include "update/DailyUpdatePolicy.h"

#include <windows.h>

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string_view>

namespace qrec {

class Logger;
class TrayIcon;

namespace update {
class UpdateCoordinator;
class DailyUpdateStateStore;
}

class AppUpdateController final {
public:
    enum class MenuCommand : std::uint8_t {
        CheckForUpdates,
        ApplyDownloadedUpdate,
    };

    using ApplyRequestedCallback =
        std::function<bool(std::filesystem::path, bool)>;
    using IsIdleCallback = std::function<bool()>;

    AppUpdateController();
    ~AppUpdateController();

    AppUpdateController(const AppUpdateController&) = delete;
    AppUpdateController& operator=(const AppUpdateController&) = delete;

    [[nodiscard]] bool Initialize(
        HWND messageWindow,
        TrayIcon& trayIcon,
        Logger& logger,
        ApplyRequestedCallback applyRequested,
        IsIdleCallback isIdle) noexcept;

    void HandleStatusChanged() noexcept;
    void Poll() noexcept;
    void ObserveBusy() noexcept;
    void HandleMenuCommand(MenuCommand command) noexcept;
    void AppendTrayMenu(
        HMENU menu,
        UINT checkCommandId,
        UINT applyCommandId) const noexcept;
    void Shutdown() noexcept;

    [[nodiscard]] const std::filesystem::path& ReadyExecutable() const noexcept {
        return readyExecutable_;
    }

private:
#ifdef SUPERRECORDING_UPDATE_TESTS
    friend struct UpdateControllerProbe;
#endif
    enum class TerminalNotification : std::uint8_t {
        None,
        UpToDate,
        Failed,
        Cancelled,
    };

    void CheckForUpdates() noexcept;
    void ApplyDownloadedUpdate() noexcept;
    void StartCheck() noexcept;
    void StartDownload() noexcept;
    void PersistCompletedDate() noexcept;
    [[nodiscard]] bool ApplicationIdle() const noexcept;
    [[nodiscard]] static update::UpdateClock ClockNow() noexcept;
    void Notify(
        std::wstring_view title,
        std::wstring_view text,
        DWORD flags) const noexcept;
    void LogInfo(std::wstring_view message) const noexcept;
    void LogError(std::wstring_view message) const noexcept;

    TrayIcon* trayIcon_{};
    Logger* logger_{};
    ApplyRequestedCallback applyRequested_;
    IsIdleCallback isIdle_;
    std::unique_ptr<update::UpdateCoordinator> coordinator_;
    std::unique_ptr<update::DailyUpdateStateStore> scheduleStore_;
    update::DailyUpdatePolicy schedule_;
    std::filesystem::path readyExecutable_;
    TerminalNotification terminalNotification_{TerminalNotification::None};
    bool downloadRequested_{};
    bool automaticOperation_{true};
    bool activityPaused_{};
    bool shuttingDown_{};
};

}  // namespace qrec
