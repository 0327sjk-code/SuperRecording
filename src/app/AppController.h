#pragma once

#include "app/AppUpdateController.h"
#include "app/TrayIcon.h"
#include "app/StartupRegistration.h"
#include "capture/CaptureEngine.h"
#include "common/HotkeyBinding.h"
#include "common/Logger.h"
#include "common/Types.h"
#include "config/ConfigStore.h"
#include "editor/EditorWindow.h"
#include "overlay/RecordingOverlay.h"
#include "overlay/RecordingRegionFrame.h"
#include "selection/RegionSelector.h"

#include <windows.h>

#include <atomic>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <thread>

namespace qrec {

class AppController final {
public:
    AppController(HINSTANCE instance, bool launchedAtStartup);
    ~AppController();

    AppController(const AppController&) = delete;
    AppController& operator=(const AppController&) = delete;

    [[nodiscard]] bool Initialize();
    [[nodiscard]] int Run();

    static constexpr wchar_t WindowClassName[] = L"SuperRecording.HiddenWindow";

private:
    enum class MenuCommand : UINT {
        Start = 1001,
        Fps30 = 1002,
        Fps60 = 1003,
        ToggleStartup = 1004,
        SetSaveDirectory = 1005,
        OpenSaveDirectory = 1006,
        Exit = 1007,
        SetRecordingHotkey = 1008,
        ToggleKeepEditorOpenAfterExport = 1009,
        CheckForUpdates = 1010,
        ApplyDownloadedUpdate = 1011,
        ToggleAdjustSelectionBeforeRecording = 1012,
    };

    static LRESULT CALLBACK WindowProcedure(HWND window, UINT message, WPARAM wParam, LPARAM lParam);
    LRESULT HandleMessage(UINT message, WPARAM wParam, LPARAM lParam);

    void StartSelection();
    void StartCapture(const IntRect& requestedRegion);
    void AbortCaptureStartup(
        const std::filesystem::path& recordingPath,
        const std::wstring& primaryError);
    void SetCapturePaused(bool paused);
    void StopCapture();
    void HandleCaptureStats(const RecordingStats& stats);
    void HandleCaptureCompleted(RecordingResult result);
    void HandleCaptureError(const capture::CaptureError& error);
    void OpenEditor(RecordingResult result);
    void HandleEditorClosed();
    void CloseEditorAfterExport();
    void ShowTrayMenu();
    void HandleMenu(MenuCommand command);
    void ConfigureRecordingHotkey();
    [[nodiscard]] std::wstring ApplyRecordingHotkey(const HotkeyBinding& binding);
    void UpdateHotkeyPresentation();
    void ChooseSaveDirectory();
    void SaveSettings();
    void ReconcileStartupAtLaunch();
    void ToggleStartup();
    void ToggleKeepEditorOpenAfterExport();
    void ToggleAdjustSelectionBeforeRecording();
    void BeginApplyDownloadedUpdate(std::filesystem::path downloadedExecutable);
    void CompleteExit();
    void LogStartupFailure(
        std::wstring_view context,
        const startup::RegistrationResult& result);
    void ActivateCurrentSurface();
    void RequestExit();
    void Shutdown();
    void RunStartupMaintenance() noexcept;

    [[nodiscard]] std::optional<std::filesystem::path> PrepareRecordingCacheDirectory(
        std::wstring* errorMessage) const;
    [[nodiscard]] static std::filesystem::path NextTemporaryRecordingPath(
        const std::filesystem::path& cacheDirectory,
        std::wstring* errorMessage);
    [[nodiscard]] bool IsBusy() const noexcept;
    [[nodiscard]] static std::wstring CaptureErrorText(const capture::CaptureError& error);

    HINSTANCE instance_{};
    HWND window_{};
    UINT taskbarCreatedMessage_{};
    ConfigStore configStore_;
    AppSettings settings_;
    Logger logger_;
    TrayIcon trayIcon_;
    selection::RegionSelector selector_;
    overlay::RecordingRegionFrame recordingRegionFrame_;
    overlay::RecordingOverlay recordingOverlay_;
    capture::CaptureEngine captureEngine_;
    std::unique_ptr<EditorWindow> editor_;
    std::jthread finalizer_;
    AppUpdateController appUpdateController_;
    std::filesystem::path activeRecordingPath_;
    std::filesystem::path updateExecutableToApply_;
    RecordingState state_{RecordingState::Idle};
    HotkeyBinding recordingHotkey_{DefaultHotkeyBinding()};
    int activeHotkeyId_{};
    bool hotkeyRegistered_{};
    bool hotkeyEditorOpen_{};
    bool startupEnabled_{true};
    bool mediaFoundationStarted_{};
    bool shuttingDown_{};
    bool exitAfterFinalize_{};
    bool launchedAtStartup_{};
    bool applyUpdateOnExit_{};
};

}  // namespace qrec
