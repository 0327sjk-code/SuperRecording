#pragma once

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
}

class AppUpdateController final {
public:
    enum class MenuCommand : std::uint8_t {
        CheckForUpdates,
        ApplyDownloadedUpdate,
    };

    using ApplyRequestedCallback =
        std::function<void(std::filesystem::path)>;

    AppUpdateController();
    ~AppUpdateController();

    AppUpdateController(const AppUpdateController&) = delete;
    AppUpdateController& operator=(const AppUpdateController&) = delete;

    [[nodiscard]] bool Initialize(
        HWND messageWindow,
        TrayIcon& trayIcon,
        Logger& logger,
        ApplyRequestedCallback applyRequested) noexcept;

    void HandleStatusChanged() noexcept;
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
    enum class TerminalNotification : std::uint8_t {
        None,
        UpToDate,
        Failed,
    };

    void CheckForUpdates() noexcept;
    void ApplyDownloadedUpdate() noexcept;
    void Notify(
        std::wstring_view title,
        std::wstring_view text,
        DWORD flags) const noexcept;
    void LogInfo(std::wstring_view message) const noexcept;
    void LogError(std::wstring_view message) const noexcept;

    TrayIcon* trayIcon_{};
    Logger* logger_{};
    ApplyRequestedCallback applyRequested_;
    std::unique_ptr<update::UpdateCoordinator> coordinator_;
    std::filesystem::path readyExecutable_;
    TerminalNotification terminalNotification_{TerminalNotification::None};
    bool downloadRequested_{};
    bool shuttingDown_{};
};

}  // namespace qrec
