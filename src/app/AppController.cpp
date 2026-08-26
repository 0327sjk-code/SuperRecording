#include "app/AppController.h"

#include "app/CacheMaintenance.h"
#include "app/FolderPicker.h"
#include "app/HotkeyEditorDialog.h"
#include "app/resource.h"
#include "common/AppMessages.h"
#include "common/ProductInfo.h"
#include "common/Win32Helpers.h"
#include "update/SelfUpdateBootstrap.h"

#include <commctrl.h>
#include <mfapi.h>
#include <shellapi.h>

#include <algorithm>
#include <format>
#include <memory>
#include <string>
#include <system_error>
#include <utility>

namespace qrec {
namespace {

constexpr int kHotkeySlotA = 1;
constexpr int kHotkeySlotB = 2;

constexpr bool ShouldCloseEditorAfterExport(
    const bool exportSucceeded,
    const bool keepEditorOpenAfterExport) noexcept {
    return exportSucceeded && !keepEditorOpenAfterExport;
}

static_assert(!ShouldCloseEditorAfterExport(false, false));
static_assert(!ShouldCloseEditorAfterExport(false, true));
static_assert(ShouldCloseEditorAfterExport(true, false));
static_assert(!ShouldCloseEditorAfterExport(true, true));

bool DeleteFileIfPresent(
    const std::filesystem::path& filePath,
    std::wstring* errorMessage) noexcept {
    if (filePath.empty()) {
        return true;
    }
    if (::DeleteFileW(filePath.c_str()) != FALSE) {
        return true;
    }
    const DWORD error = ::GetLastError();
    if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND) {
        return true;
    }
    if (errorMessage != nullptr) {
        *errorMessage = win32::FormatLastError(error);
    }
    return false;
}

bool ProbeWritableDirectory(
    const std::filesystem::path& directory,
    const bool requireHardLink) noexcept {
    try {
        const std::filesystem::path probeFile =
            win32::MakeUniquePath(directory, L".qrec-write-probe", L".tmp");
        const std::filesystem::path probeLink =
            win32::MakeUniquePath(directory, L".qrec-link-probe", L".tmp");
        if (probeFile.empty() || probeLink.empty()) {
            return false;
        }
        const HANDLE file = ::CreateFileW(
            probeFile.c_str(),
            GENERIC_WRITE,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            nullptr,
            CREATE_NEW,
            FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_TEMPORARY,
            nullptr);
        if (file == INVALID_HANDLE_VALUE) {
            return false;
        }
        ::CloseHandle(file);

        bool supported = true;
        if (requireHardLink) {
            supported =
                ::CreateHardLinkW(probeLink.c_str(), probeFile.c_str(), nullptr) != FALSE;
            static_cast<void>(::DeleteFileW(probeLink.c_str()));
        }
        static_cast<void>(::DeleteFileW(probeFile.c_str()));
        return supported;
    } catch (...) {
        return false;
    }
}

template <typename T>
T* TakePayload(const LPARAM value) {
    return reinterpret_cast<T*>(value);
}

std::wstring EllipsizePath(const std::filesystem::path& path) {
    std::wstring value = path.wstring();
    constexpr std::size_t maximum = 52;
    if (value.size() <= maximum) {
        return value;
    }
    return value.substr(0, 20) + L"…" + value.substr(value.size() - 29);
}

std::wstring_view StartupOperationLabel(
    const startup::RegistrationOperation operation) noexcept {
    switch (operation) {
    case startup::RegistrationOperation::ResolveExecutable:
        return L"读取当前程序路径";
    case startup::RegistrationOperation::ValidateExecutable:
        return L"校验启动命令";
    case startup::RegistrationOperation::QueryRegistry:
        return L"读取当前用户启动项";
    case startup::RegistrationOperation::WriteRegistry:
        return L"写入当前用户启动项";
    case startup::RegistrationOperation::DeleteRegistry:
        return L"删除当前用户启动项";
    case startup::RegistrationOperation::DeleteLegacyRegistry:
        return L"删除旧版当前用户启动项";
    case startup::RegistrationOperation::None:
    default:
        return L"更新当前用户启动项";
    }
}

}  // namespace

AppController::AppController(
    const HINSTANCE instance,
    const bool launchedAtStartup)
    : instance_(instance),
      launchedAtStartup_(launchedAtStartup) {}

AppController::~AppController() {
    Shutdown();
}

bool AppController::Initialize() {
    settings_ = configStore_.Load();
    recordingHotkey_ = configStore_.LoadRecordingHotkey();
    startupEnabled_ = configStore_.LoadStartupEnabled();
    if (configStore_.LegacySettingsMigrated()) {
        logger_.Info(
            L"已从旧版 QingRecorder 安全迁移用户配置到：" +
            configStore_.FilePath().wstring());
    }
    if (!configStore_.SaveRecordingHotkey(recordingHotkey_)) {
        logger_.Error(
            L"录制快捷键配置写入失败：" + configStore_.FilePath().wstring());
    }
    INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_STANDARD_CLASSES | ICC_BAR_CLASSES};
    ::InitCommonControlsEx(&controls);

    const HRESULT mediaResult = ::MFStartup(MF_VERSION, MFSTARTUP_FULL);
    if (FAILED(mediaResult)) {
        win32::ShowError(nullptr, L"SuperRecording 无法启动",
                         L"Media Foundation 初始化失败：\n" + win32::FormatError(mediaResult));
        return false;
    }
    mediaFoundationStarted_ = true;

    WNDCLASSEXW windowClass{};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.hInstance = instance_;
    windowClass.lpfnWndProc = &AppController::WindowProcedure;
    windowClass.lpszClassName = WindowClassName;
    windowClass.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
    windowClass.hIcon = static_cast<HICON>(::LoadImageW(
        instance_, MAKEINTRESOURCEW(IDI_SUPER_RECORDING), IMAGE_ICON, 32, 32, LR_DEFAULTCOLOR));
    windowClass.hIconSm = static_cast<HICON>(::LoadImageW(
        instance_, MAKEINTRESOURCEW(IDI_SUPER_RECORDING), IMAGE_ICON, 16, 16, LR_DEFAULTCOLOR));
    if (::RegisterClassExW(&windowClass) == 0 && ::GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        win32::ShowError(nullptr, L"SuperRecording 无法启动", win32::FormatLastError());
        return false;
    }

    window_ = ::CreateWindowExW(
        WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
        WindowClassName,
        product::Name,
        WS_OVERLAPPED,
        -32000,
        -32000,
        1,
        1,
        nullptr,
        nullptr,
        instance_,
        this);
    if (window_ == nullptr) {
        win32::ShowError(nullptr, L"SuperRecording 无法启动", win32::FormatLastError());
        return false;
    }

    taskbarCreatedMessage_ = ::RegisterWindowMessageW(L"TaskbarCreated");
    UpdateHotkeyPresentation();
    if (!trayIcon_.Add(window_, messages::TrayIcon)) {
        win32::ShowError(nullptr, L"SuperRecording 无法启动", L"无法创建系统托盘图标。");
        return false;
    }
    static_cast<void>(appUpdateController_.Initialize(
        window_,
        trayIcon_,
        logger_,
        [this](std::filesystem::path downloadedExecutable) {
            BeginApplyDownloadedUpdate(std::move(downloadedExecutable));
        }));

    hotkeyRegistered_ = ::RegisterHotKey(
        window_,
        kHotkeySlotA,
        RegistrationModifiers(recordingHotkey_),
        recordingHotkey_.virtualKey) != FALSE;
    activeHotkeyId_ = hotkeyRegistered_ ? kHotkeySlotA : 0;
    const std::wstring shortcutLabel = FormatHotkeyBinding(recordingHotkey_);
    if (!hotkeyRegistered_) {
        const DWORD hotkeyError = ::GetLastError();
        logger_.Error(std::format(
            L"RegisterHotKey({}, modifiers=0x{:X}) 失败，错误码 {}：{}",
            shortcutLabel,
            RegistrationModifiers(recordingHotkey_),
            hotkeyError,
            win32::FormatLastError(hotkeyError)));
        trayIcon_.ShowNotification(
            shortcutLabel + L" 快捷键不可用",
            hotkeyError == ERROR_HOTKEY_ALREADY_REGISTERED
                ? shortcutLabel + L" 已被其他软件占用；可从托盘右键菜单重新设置。"
                : shortcutLabel + L" 全局快捷键注册失败；可从托盘右键菜单重新设置。",
            NIIF_WARNING);
    } else if (!launchedAtStartup_) {
        trayIcon_.ShowNotification(
            L"SuperRecording 已就绪",
            L"单击托盘图标或按 " + shortcutLabel + L" 开始框选录制。",
            NIIF_INFO);
    }
    ReconcileStartupAtLaunch();
    return true;
}

int AppController::Run() {
    // Update health is signalled after Initialize() and before Run(). Keep all
    // potentially unbounded user-path I/O outside that rollback-critical gate.
    RunStartupMaintenance();

    MSG message{};
    while (true) {
        const BOOL result = ::GetMessageW(&message, nullptr, 0, 0);
        if (result == 0) {
            return static_cast<int>(message.wParam);
        }
        if (result < 0) {
            logger_.Error(L"消息循环失败：" + win32::FormatLastError());
            return 1;
        }
        if (editor_ && editor_->IsOpen()) {
            const HWND editorWindow = editor_->WindowHandle();
            if (message.message == WM_KEYDOWN && message.wParam == VK_ESCAPE) {
                ::SendMessageW(editorWindow, message.message, message.wParam, message.lParam);
                continue;
            }
            if (message.message == WM_KEYDOWN && message.wParam == VK_SPACE) {
                wchar_t focusClass[32]{};
                const HWND focused = ::GetFocus();
                const bool buttonFocused =
                    focused != nullptr &&
                    (::IsChild(editorWindow, focused) != FALSE || focused == editorWindow) &&
                    ::GetClassNameW(focused, focusClass, _countof(focusClass)) > 0 &&
                    _wcsicmp(focusClass, L"Button") == 0;
                if (!buttonFocused) {
                    const bool repeated =
                        (static_cast<LPARAM>(message.lParam) & (1LL << 30)) != 0;
                    if (!repeated) {
                        ::SendMessageW(
                            editorWindow, message.message, message.wParam, message.lParam);
                    }
                    continue;
                }
            }
            if (::IsDialogMessageW(editorWindow, &message) != FALSE) {
                continue;
            }
        }
        ::TranslateMessage(&message);
        ::DispatchMessageW(&message);
    }
}

void AppController::RunStartupMaintenance() noexcept {
    try {
        std::wstring directoryError;
        if (!win32::EnsureDirectory(settings_.saveDirectory, &directoryError)) {
            logger_.Error(
                L"无法访问配置的保存位置，将使用默认视频目录：" +
                directoryError);
            settings_.saveDirectory = win32::DefaultVideoDirectory();
            static_cast<void>(
                win32::EnsureDirectory(settings_.saveDirectory, nullptr));
        }

        const CacheCleanupReport cleanup =
            CacheMaintenance::Cleanup(settings_.saveDirectory);
        if (cleanup.removedFiles > 0) {
            logger_.Info(std::format(
                L"已清理 {} 个过期录屏缓存目录项，涉及文件大小 {} 字节。",
                cleanup.removedFiles,
                cleanup.releasedBytes));
        }
        if (cleanup.failures > 0) {
            logger_.Error(std::format(
                L"录屏缓存清理有 {} 个文件无法检查或删除；程序将继续运行。",
                cleanup.failures));
        }
        logger_.Info(
            L"应用启动，保存位置：" + settings_.saveDirectory.wstring());
    } catch (...) {
        try {
            logger_.Error(
                L"启动维护任务执行失败；录屏功能将继续使用当前配置。");
        } catch (...) {
        }
    }
}

LRESULT CALLBACK AppController::WindowProcedure(
    const HWND window,
    const UINT message,
    const WPARAM wParam,
    const LPARAM lParam) {
    AppController* controller = reinterpret_cast<AppController*>(
        ::GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lParam);
        controller = static_cast<AppController*>(create->lpCreateParams);
        controller->window_ = window;
        ::SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(controller));
    }
    if (controller != nullptr) {
        try {
            return controller->HandleMessage(message, wParam, lParam);
        } catch (...) {
            try {
                controller->logger_.Error(L"窗口消息处理发生异常，当前操作已中止。");
            } catch (...) {
            }
            return 0;
        }
    }
    return ::DefWindowProcW(window, message, wParam, lParam);
}

LRESULT AppController::HandleMessage(
    const UINT message,
    const WPARAM wParam,
    const LPARAM lParam) {
    if (taskbarCreatedMessage_ != 0 && message == taskbarCreatedMessage_) {
        static_cast<void>(trayIcon_.RestoreAfterExplorerRestart());
        return 0;
    }

    switch (message) {
    case WM_HOTKEY:
        if (!hotkeyEditorOpen_ && hotkeyRegistered_ &&
            static_cast<int>(wParam) == activeHotkeyId_) {
            StartSelection();
        }
        return 0;

    case messages::TrayIcon: {
        const UINT eventMessage = LOWORD(lParam);
        if (eventMessage == WM_LBUTTONUP || eventMessage == NIN_SELECT ||
            eventMessage == NIN_KEYSELECT) {
            ActivateCurrentSurface();
        } else if (eventMessage == WM_RBUTTONUP || eventMessage == WM_CONTEXTMENU) {
            ShowTrayMenu();
        }
        return 0;
    }

    case WM_COMMAND:
        HandleMenu(static_cast<MenuCommand>(LOWORD(wParam)));
        return 0;

    case messages::CaptureStats: {
        std::unique_ptr<RecordingStats> stats(TakePayload<RecordingStats>(lParam));
        if (stats) {
            HandleCaptureStats(*stats);
        }
        return 0;
    }

    case messages::CaptureStopped: {
        std::unique_ptr<RecordingResult> result(TakePayload<RecordingResult>(lParam));
        if (result) {
            HandleCaptureCompleted(std::move(*result));
        }
        return 0;
    }

    case messages::CaptureFailed: {
        std::unique_ptr<capture::CaptureError> error(TakePayload<capture::CaptureError>(lParam));
        if (error) {
            HandleCaptureError(*error);
        }
        return 0;
    }

    case messages::EditorClosed:
        HandleEditorClosed();
        return 0;

    case messages::CloseEditorAfterExport:
        CloseEditorAfterExport();
        return 0;

    case messages::UpdateStatusChanged:
        appUpdateController_.HandleStatusChanged();
        return 0;

    case messages::ExistingInstance:
        ActivateCurrentSurface();
        return 0;

    case messages::RequestExit:
        RequestExit();
        return 0;

    case WM_QUERYENDSESSION:
        return TRUE;

    case WM_ENDSESSION:
        if (wParam != FALSE) {
            Shutdown();
        }
        return 0;

    case WM_DESTROY:
        if (!shuttingDown_) {
            Shutdown();
        }
        ::PostQuitMessage(0);
        return 0;

    default:
        return ::DefWindowProcW(window_, message, wParam, lParam);
    }
}

void AppController::StartSelection() {
    if (shuttingDown_ || hotkeyEditorOpen_) {
        return;
    }
    if (state_ == RecordingState::Editing) {
        ActivateCurrentSurface();
        return;
    }
    if (state_ != RecordingState::Idle) {
        trayIcon_.ShowNotification(L"SuperRecording 正在工作", L"请先结束当前录制或编辑。", NIIF_INFO);
        return;
    }

    state_ = RecordingState::Selecting;
    const std::optional<IntRect> selected = selector_.Select(
        window_,
        settings_.adjustSelectionBeforeRecording);
    if (shuttingDown_) {
        return;
    }
    if (!selected.has_value()) {
        state_ = RecordingState::Idle;
        return;
    }
    StartCapture(*selected);
}

void AppController::StartCapture(const IntRect& requestedRegion) {
    std::wstring directoryError;
    const std::optional<std::filesystem::path> cacheDirectory =
        PrepareRecordingCacheDirectory(&directoryError);
    if (!cacheDirectory.has_value()) {
        state_ = RecordingState::Idle;
        win32::ShowError(window_, L"无法开始录制", directoryError);
        return;
    }

    capture::CaptureCallbacks callbacks;
    callbacks.onStats = [window = window_](const RecordingStats& stats) {
        auto payload = std::make_unique<RecordingStats>(stats);
        if (::PostMessageW(window, messages::CaptureStats, 0,
                           reinterpret_cast<LPARAM>(payload.get()))) {
            payload.release();
        }
    };
    callbacks.onError = [window = window_](const capture::CaptureError& error) {
        auto payload = std::make_unique<capture::CaptureError>(error);
        if (::PostMessageW(window, messages::CaptureFailed, 0,
                           reinterpret_cast<LPARAM>(payload.get()))) {
            payload.release();
        }
    };

    std::wstring pathError;
    const std::filesystem::path recordingPath =
        NextTemporaryRecordingPath(*cacheDirectory, &pathError);
    if (recordingPath.empty()) {
        state_ = RecordingState::Idle;
        win32::ShowError(
            window_,
            L"无法开始录制",
            pathError.empty() ? L"无法生成安全的录屏缓存文件名。" : pathError);
        return;
    }

    activeRecordingPath_ = recordingPath;
    capture::CaptureError error;
    if (!captureEngine_.Start(
            requestedRegion,
            recordingPath,
            settings_.framesPerSecond,
            settings_.includeCursor,
            std::move(callbacks),
            &error)) {
        state_ = RecordingState::Idle;
        const std::wstring text = CaptureErrorText(error);
        logger_.Error(L"开始录制失败：" + text);
        std::wstring cleanupError;
        if (!DeleteFileIfPresent(recordingPath, &cleanupError)) {
            logger_.Error(L"清理启动失败录屏文件失败：" + cleanupError +
                          L"；路径：" + recordingPath.wstring());
        }
        activeRecordingPath_.clear();
        win32::ShowError(window_, L"无法开始录制", text);
        return;
    }

    const IntRect actualRegion = captureEngine_.ActualRegion();
    if (!recordingRegionFrame_.Create(window_, actualRegion)) {
        AbortCaptureStartup(
            recordingPath,
            L"无法创建录制区域边界，或系统无法保证边界不进入成片。");
        return;
    }

    overlay::RecordingOverlayCallbacks overlayCallbacks;
    overlayCallbacks.pauseChanged = [this](const bool paused) {
        SetCapturePaused(paused);
    };
    overlayCallbacks.stopRequested = [this]() {
        StopCapture();
    };
    if (!recordingOverlay_.Create(window_, actualRegion, std::move(overlayCallbacks))) {
        AbortCaptureStartup(recordingPath, L"录制控制浮条创建失败。");
        return;
    }
    if (!recordingOverlay_.IsCaptureExcluded()) {
        AbortCaptureStartup(
            recordingPath,
            L"系统无法排除录制控制浮条；为避免控件进入成片，本次录制已取消。");
        return;
    }

    state_ = RecordingState::Recording;
    logger_.Info(std::format(
        L"开始录制：{}x{} @ {} FPS",
        actualRegion.Width(),
        actualRegion.Height(),
        settings_.framesPerSecond));
}

void AppController::AbortCaptureStartup(
    const std::filesystem::path& recordingPath,
    const std::wstring& primaryError) {
    recordingOverlay_.Destroy();
    recordingRegionFrame_.Destroy();

    capture::CaptureError stopError;
    std::optional<RecordingResult> stoppedResult;
    const RecordingState captureState = captureEngine_.State();
    if (captureState == RecordingState::Recording ||
        captureState == RecordingState::Paused ||
        captureState == RecordingState::Finalizing) {
        stoppedResult = captureEngine_.Stop(&stopError);
    }
    if (!stopError.message.empty() || stopError.nativeCode != 0) {
        logger_.Error(L"回滚录制启动时停止捕获失败：" + CaptureErrorText(stopError));
    }

    std::wstring cleanupError;
    if (!DeleteFileIfPresent(recordingPath, &cleanupError)) {
        logger_.Error(L"清理启动失败录屏文件失败：" + cleanupError +
                      L"；路径：" + recordingPath.wstring());
    }
    if (stoppedResult.has_value() &&
        !stoppedResult->systemAudio.sourcePath.empty()) {
        cleanupError.clear();
        if (!DeleteFileIfPresent(
                stoppedResult->systemAudio.sourcePath,
                &cleanupError)) {
            logger_.Error(L"清理启动失败系统音频旁路失败：" + cleanupError +
                          L"；路径：" +
                          stoppedResult->systemAudio.sourcePath.wstring());
        }
    }
    activeRecordingPath_.clear();
    state_ = RecordingState::Idle;
    logger_.Error(L"录制启动已回滚：" + primaryError);
    win32::ShowError(window_, L"无法开始录制", primaryError);
}

void AppController::SetCapturePaused(const bool paused) {
    if (state_ != RecordingState::Recording && state_ != RecordingState::Paused) {
        return;
    }
    capture::CaptureError error;
    const bool changed = paused
        ? captureEngine_.Pause(&error)
        : captureEngine_.Resume(&error);
    if (!changed) {
        recordingOverlay_.SetPaused(!paused);
        win32::ShowError(window_, L"录制状态切换失败", CaptureErrorText(error));
        return;
    }
    state_ = paused ? RecordingState::Paused : RecordingState::Recording;
    recordingOverlay_.SetPaused(paused);
    logger_.Info(paused ? L"录制已暂停。" : L"录制已继续。");
}

void AppController::StopCapture() {
    if (state_ != RecordingState::Recording && state_ != RecordingState::Paused) {
        return;
    }
    state_ = RecordingState::Finalizing;
    recordingOverlay_.SetStopping(true);
    if (finalizer_.joinable()) {
        finalizer_.join();
    }
    finalizer_ = std::jthread([this](std::stop_token) {
        capture::CaptureError error;
        std::optional<RecordingResult> result = captureEngine_.Stop(&error);
        if (result.has_value()) {
            auto payload = std::make_unique<RecordingResult>(std::move(*result));
            if (::PostMessageW(window_, messages::CaptureStopped, 0,
                               reinterpret_cast<LPARAM>(payload.get()))) {
                payload.release();
            }
        } else {
            auto payload = std::make_unique<capture::CaptureError>(std::move(error));
            if (::PostMessageW(window_, messages::CaptureFailed, 0,
                               reinterpret_cast<LPARAM>(payload.get()))) {
                payload.release();
            }
        }
    });
}

void AppController::HandleCaptureStats(const RecordingStats& stats) {
    if (state_ == RecordingState::Recording || state_ == RecordingState::Paused ||
        state_ == RecordingState::Finalizing) {
        recordingOverlay_.SetElapsed(stats.activeDuration);
    }
}

void AppController::HandleCaptureCompleted(RecordingResult result) {
    recordingOverlay_.Destroy();
    recordingRegionFrame_.Destroy();
    activeRecordingPath_ = result.sourcePath;
    logger_.Info(std::format(
        L"录制结束：{} 帧，时长 {} ms，临时文件 {}，电脑声音={}，"
        L"音频时长={} ms，音频文件={}，状态={}",
        captureEngine_.Stats().encodedFrames,
        result.duration.count(),
        result.sourcePath.wstring(),
        result.systemAudio.available ? L"可用" : L"不可用",
        result.systemAudio.duration.count(),
        result.systemAudio.sourcePath.empty()
            ? L"无"
            : result.systemAudio.sourcePath.wstring(),
        result.systemAudio.statusMessage.empty()
            ? L"无"
            : result.systemAudio.statusMessage));
    if (exitAfterFinalize_ || shuttingDown_) {
        if (window_ != nullptr) {
            CompleteExit();
        }
        return;
    }
    OpenEditor(std::move(result));
}

void AppController::HandleCaptureError(const capture::CaptureError& error) {
    if (state_ == RecordingState::Idle || state_ == RecordingState::Editing) {
        return;
    }
    recordingOverlay_.Destroy();
    recordingRegionFrame_.Destroy();
    state_ = RecordingState::Idle;
    const std::wstring text = CaptureErrorText(error);
    logger_.Error(L"录制失败：" + text);
    if (!shuttingDown_) {
        const std::wstring recoveryPath = activeRecordingPath_.empty()
            ? L""
            : L"\n\n可恢复的临时录屏：\n" + activeRecordingPath_.wstring();
        win32::ShowError(window_, L"录制失败", text + recoveryPath);
    }
    activeRecordingPath_.clear();
    if (exitAfterFinalize_ && window_ != nullptr) {
        CompleteExit();
    }
}

void AppController::OpenEditor(RecordingResult result) {
    state_ = RecordingState::Editing;
    const std::filesystem::path sourcePath = result.sourcePath;
    editor_ = std::make_unique<EditorWindow>(instance_, window_);
    EditorWindowCallbacks callbacks;
    callbacks.settingsChanged = [this](const AppSettings& updated) {
        // The selector workflow is owned by the tray and can change while an
        // editor holding an older settings snapshot is open. Never let an
        // editor callback roll that persisted choice back.
        const bool adjustSelectionBeforeRecording =
            settings_.adjustSelectionBeforeRecording;
        settings_ = updated;
        settings_.adjustSelectionBeforeRecording =
            adjustSelectionBeforeRecording;
        SaveSettings();
    };
    callbacks.exportCompleted = [this](const EditorExportResult& exportResult) {
        if (exportResult.success) {
            logger_.Info(L"导出完成：" + exportResult.finalPath.wstring());
            trayIcon_.ShowNotification(
                exportResult.copiedToClipboard ? L"已复制到剪贴板" : L"录屏已保存",
                exportResult.finalPath.wstring(),
                NIIF_INFO);
            if (ShouldCloseEditorAfterExport(
                    exportResult.success,
                    settings_.keepEditorOpenAfterExport) &&
                window_ != nullptr &&
                ::PostMessageW(
                    window_, messages::CloseEditorAfterExport, 0, 0) == FALSE) {
                logger_.Error(L"导出成功后无法投递编辑窗口关闭消息；窗口将保持打开。");
            }
        } else {
            logger_.Error(L"导出失败：" + exportResult.errorMessage);
        }
    };
    callbacks.diagnostic = [this](const std::wstring& message) {
        logger_.Info(message);
    };
    callbacks.closed = [window = window_]() {
        ::PostMessageW(window, messages::EditorClosed, 0, 0);
    };

    std::wstring error;
    if (!editor_->Open(std::move(result), settings_, std::move(callbacks), &error)) {
        editor_.reset();
        state_ = RecordingState::Idle;
        logger_.Error(L"打开编辑窗口失败：" + error);
        win32::ShowError(
            window_,
            L"无法打开录屏编辑器",
            error + L"\n\n原始录屏保留在：\n" + sourcePath.wstring());
    }
}

void AppController::HandleEditorClosed() {
    if (state_ != RecordingState::Editing) {
        return;
    }
    editor_.reset();
    activeRecordingPath_.clear();
    state_ = RecordingState::Idle;
    if (exitAfterFinalize_ && window_ != nullptr) {
        CompleteExit();
    }
}

void AppController::CloseEditorAfterExport() {
    if (state_ != RecordingState::Editing ||
        settings_.keepEditorOpenAfterExport ||
        !editor_ ||
        !editor_->IsOpen()) {
        return;
    }

    const HWND editorWindow = editor_->WindowHandle();
    if (editorWindow == nullptr || ::IsWindow(editorWindow) == FALSE) {
        return;
    }

    ::EnableWindow(editorWindow, FALSE);
    if (::PostMessageW(editorWindow, WM_CLOSE, 0, 0) == FALSE) {
        ::EnableWindow(editorWindow, TRUE);
        logger_.Error(L"导出成功后无法关闭编辑窗口；窗口已恢复为可操作状态。");
    }
}

void AppController::ShowTrayMenu() {
    HMENU menu = ::CreatePopupMenu();
    if (menu == nullptr) {
        return;
    }
    const bool idle = state_ == RecordingState::Idle;
    const UINT settingsAvailability = idle ? MF_ENABLED : MF_GRAYED;
    const std::wstring shortcutLabel = FormatHotkeyBinding(recordingHotkey_);
    const std::wstring startLabel = L"开始录制\t" + shortcutLabel;
    ::AppendMenuW(menu, MF_STRING | (idle ? MF_ENABLED : MF_GRAYED),
                  static_cast<UINT>(MenuCommand::Start), startLabel.c_str());
    ::AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);

    const std::wstring hotkeyStatus = L"录制快捷键：" + shortcutLabel;
    ::AppendMenuW(menu, MF_STRING | MF_GRAYED, 0, hotkeyStatus.c_str());
    ::AppendMenuW(
        menu,
        MF_STRING | settingsAvailability,
        static_cast<UINT>(MenuCommand::SetRecordingHotkey),
        L"设置录制快捷键…");
    ::AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);

    HMENU frameRate = ::CreatePopupMenu();
    ::AppendMenuW(frameRate, MF_STRING | settingsAvailability |
                  (settings_.framesPerSecond == 30 ? MF_CHECKED : 0),
                  static_cast<UINT>(MenuCommand::Fps30), L"30 帧/秒");
    ::AppendMenuW(frameRate, MF_STRING | settingsAvailability |
                  (settings_.framesPerSecond == 60 ? MF_CHECKED : 0),
                  static_cast<UINT>(MenuCommand::Fps60), L"60 帧/秒（默认）");
    ::AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(frameRate), L"录制帧率");

    ::AppendMenuW(
        menu,
        MF_STRING | (settings_.adjustSelectionBeforeRecording ? MF_CHECKED : 0),
        static_cast<UINT>(MenuCommand::ToggleAdjustSelectionBeforeRecording),
        L"框选后调整选区");

    ::AppendMenuW(
        menu,
        MF_STRING | (settings_.keepEditorOpenAfterExport ? MF_CHECKED : 0),
        static_cast<UINT>(MenuCommand::ToggleKeepEditorOpenAfterExport),
        L"导出后保留编辑窗口");

    ::AppendMenuW(
        menu,
        MF_STRING | (startupEnabled_ ? MF_CHECKED : 0),
        static_cast<UINT>(MenuCommand::ToggleStartup),
        L"开机自启");

    ::AppendMenuW(menu, MF_STRING | settingsAvailability,
                  static_cast<UINT>(MenuCommand::SetSaveDirectory),
                  L"设置保存位置…");
    ::AppendMenuW(menu, MF_STRING, static_cast<UINT>(MenuCommand::OpenSaveDirectory),
                  L"打开保存文件夹");
    const std::wstring pathLabel = L"当前：" + EllipsizePath(settings_.saveDirectory);
    ::AppendMenuW(menu, MF_STRING | MF_DISABLED, 0, pathLabel.c_str());
    ::AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);

    appUpdateController_.AppendTrayMenu(
        menu,
        static_cast<UINT>(MenuCommand::CheckForUpdates),
        static_cast<UINT>(MenuCommand::ApplyDownloadedUpdate));
    ::AppendMenuW(menu, MF_STRING, static_cast<UINT>(MenuCommand::Exit), L"退出 SuperRecording");

    POINT cursor{};
    ::GetCursorPos(&cursor);
    ::SetForegroundWindow(window_);
    const UINT chosen = ::TrackPopupMenuEx(
        menu,
        TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY,
        cursor.x,
        cursor.y,
        window_,
        nullptr);
    ::DestroyMenu(menu);
    ::PostMessageW(window_, WM_NULL, 0, 0);
    if (chosen != 0) {
        HandleMenu(static_cast<MenuCommand>(chosen));
    }
}

void AppController::HandleMenu(const MenuCommand command) {
    const bool changesRecordingSettings =
        command == MenuCommand::Fps30 || command == MenuCommand::Fps60 ||
        command == MenuCommand::SetSaveDirectory ||
        command == MenuCommand::SetRecordingHotkey;
    if (changesRecordingSettings && state_ != RecordingState::Idle) {
        trayIcon_.ShowNotification(
            L"当前设置暂不可修改",
            L"请结束当前录制或编辑后再修改快捷键、帧率和保存位置。",
            NIIF_INFO);
        return;
    }

    switch (command) {
    case MenuCommand::Start:
        StartSelection();
        break;
    case MenuCommand::Fps30:
        settings_.framesPerSecond = 30;
        SaveSettings();
        break;
    case MenuCommand::Fps60:
        settings_.framesPerSecond = 60;
        SaveSettings();
        break;
    case MenuCommand::ToggleStartup:
        ToggleStartup();
        break;
    case MenuCommand::ToggleKeepEditorOpenAfterExport:
        ToggleKeepEditorOpenAfterExport();
        break;
    case MenuCommand::ToggleAdjustSelectionBeforeRecording:
        ToggleAdjustSelectionBeforeRecording();
        break;
    case MenuCommand::SetSaveDirectory:
        ChooseSaveDirectory();
        break;
    case MenuCommand::OpenSaveDirectory:
        win32::OpenDirectory(settings_.saveDirectory);
        break;
    case MenuCommand::SetRecordingHotkey:
        ConfigureRecordingHotkey();
        break;
    case MenuCommand::CheckForUpdates:
        appUpdateController_.HandleMenuCommand(
            AppUpdateController::MenuCommand::CheckForUpdates);
        break;
    case MenuCommand::ApplyDownloadedUpdate:
        appUpdateController_.HandleMenuCommand(
            AppUpdateController::MenuCommand::ApplyDownloadedUpdate);
        break;
    case MenuCommand::Exit:
        RequestExit();
        break;
    default:
        break;
    }
}

void AppController::ConfigureRecordingHotkey() {
    if (hotkeyEditorOpen_ || state_ != RecordingState::Idle) {
        return;
    }

    hotkeyEditorOpen_ = true;
    HotkeyEditorDialog dialog(instance_, window_);
    std::wstring error;
    bool opened = false;
    try {
        opened = dialog.Open(
            recordingHotkey_,
            [this](const HotkeyBinding& binding) {
                return ApplyRecordingHotkey(binding);
            },
            &error);
    } catch (...) {
        hotkeyEditorOpen_ = false;
        throw;
    }
    hotkeyEditorOpen_ = false;

    if (!opened && !error.empty()) {
        logger_.Error(L"快捷键设置窗口打开失败：" + error);
        trayIcon_.ShowNotification(
            L"无法打开快捷键设置",
            error,
            NIIF_WARNING);
    }
}

std::wstring AppController::ApplyRecordingHotkey(const HotkeyBinding& binding) {
    const std::wstring validationError = HotkeyValidationError(binding);
    if (!validationError.empty()) {
        return validationError;
    }

    if (binding == recordingHotkey_ && hotkeyRegistered_) {
        if (!configStore_.SaveRecordingHotkey(binding)) {
            logger_.Error(
                L"录制快捷键配置保存失败：" + configStore_.FilePath().wstring());
            return L"无法保存快捷键设置，当前快捷键仍然有效。";
        }
        return {};
    }

    const int candidateHotkeyId = hotkeyRegistered_
        ? (activeHotkeyId_ == kHotkeySlotA ? kHotkeySlotB : kHotkeySlotA)
        : kHotkeySlotA;
    if (::RegisterHotKey(
            window_,
            candidateHotkeyId,
            RegistrationModifiers(binding),
            binding.virtualKey) == FALSE) {
        const DWORD nativeError = ::GetLastError();
        const std::wstring shortcutLabel = FormatHotkeyBinding(binding);
        logger_.Error(std::format(
            L"候选快捷键 {} 注册失败，错误码 {}：{}",
            shortcutLabel,
            nativeError,
            win32::FormatLastError(nativeError)));
        return nativeError == ERROR_HOTKEY_ALREADY_REGISTERED
            ? L"该快捷键已被其他软件占用，请换一个组合。原快捷键仍然有效。"
            : std::format(
                L"无法注册该快捷键（错误码 {}）。原快捷键仍然有效。",
                nativeError);
    }

    const HotkeyBinding previousBinding = recordingHotkey_;
    if (!configStore_.SaveRecordingHotkey(binding)) {
        static_cast<void>(::UnregisterHotKey(window_, candidateHotkeyId));
        if (!configStore_.SaveRecordingHotkey(previousBinding)) {
            logger_.Error(
                L"候选快捷键保存失败后，原配置回写也失败：" +
                configStore_.FilePath().wstring());
        }
        logger_.Error(
            L"候选快捷键配置保存失败：" + configStore_.FilePath().wstring());
        return L"无法保存快捷键设置，已保留原快捷键。";
    }

    if (hotkeyRegistered_ &&
        ::UnregisterHotKey(window_, activeHotkeyId_) == FALSE) {
        const DWORD nativeError = ::GetLastError();
        static_cast<void>(::UnregisterHotKey(window_, candidateHotkeyId));
        if (!configStore_.SaveRecordingHotkey(previousBinding)) {
            logger_.Error(
                L"快捷键切换回滚时无法恢复原配置：" +
                configStore_.FilePath().wstring());
        }
        logger_.Error(std::format(
            L"原快捷键注销失败，错误码 {}：{}",
            nativeError,
            win32::FormatLastError(nativeError)));
        return L"无法完成快捷键切换，已保留原快捷键。";
    }

    activeHotkeyId_ = candidateHotkeyId;
    hotkeyRegistered_ = true;
    recordingHotkey_ = binding;
    UpdateHotkeyPresentation();
    const std::wstring shortcutLabel = FormatHotkeyBinding(recordingHotkey_);
    logger_.Info(L"录制快捷键已更新为：" + shortcutLabel);
    trayIcon_.ShowNotification(
        L"录制快捷键已更新",
        shortcutLabel + L" 已立即生效。",
        NIIF_INFO);
    return {};
}

void AppController::UpdateHotkeyPresentation() {
    std::wstring shortcutLabel = FormatHotkeyBinding(recordingHotkey_);
    if (shortcutLabel.empty()) {
        recordingHotkey_ = DefaultHotkeyBinding();
        shortcutLabel = FormatHotkeyBinding(recordingHotkey_);
    }
    trayIcon_.UpdateShortcutLabel(shortcutLabel);
}

void AppController::ChooseSaveDirectory() {
    const std::optional<std::filesystem::path> selected =
        PickSaveDirectory(window_, settings_.saveDirectory);
    if (!selected.has_value()) {
        return;
    }
    std::wstring error;
    if (!win32::EnsureDirectory(*selected, &error)) {
        win32::ShowError(window_, L"无法使用保存位置", error);
        return;
    }
    settings_.saveDirectory = *selected;
    SaveSettings();
    trayIcon_.ShowNotification(L"保存位置已更新", settings_.saveDirectory.wstring(), NIIF_INFO);
}

void AppController::SaveSettings() {
    if (!configStore_.Save(settings_)) {
        logger_.Error(L"配置保存失败：" + configStore_.FilePath().wstring());
        trayIcon_.ShowNotification(L"设置未保存", L"无法写入 SuperRecording 配置文件。", NIIF_WARNING);
    }
}

void AppController::ToggleKeepEditorOpenAfterExport() {
    const bool requestedValue = !settings_.keepEditorOpenAfterExport;
    if (!configStore_.SaveKeepEditorOpenAfterExport(requestedValue)) {
        logger_.Error(
            L"导出后保留编辑窗口设置保存失败，已保持原设置：" +
            configStore_.FilePath().wstring());
        trayIcon_.ShowNotification(
            L"设置未保存",
            L"无法写入 SuperRecording 配置文件，原设置保持不变。",
            NIIF_WARNING);
        return;
    }

    settings_.keepEditorOpenAfterExport = requestedValue;
    logger_.Info(
        requestedValue
            ? L"已启用导出后保留编辑窗口。"
            : L"已关闭导出后保留编辑窗口；后续成功导出将自动关闭编辑窗口。");
}

void AppController::ToggleAdjustSelectionBeforeRecording() {
    const bool requestedValue = !settings_.adjustSelectionBeforeRecording;
    if (!configStore_.SaveAdjustSelectionBeforeRecording(requestedValue)) {
        logger_.Error(
            L"框选后调整选区设置保存失败，已保持原设置：" +
            configStore_.FilePath().wstring());
        trayIcon_.ShowNotification(
            L"设置未保存",
            L"无法写入 SuperRecording 配置文件，原设置保持不变。",
            NIIF_WARNING);
        return;
    }

    settings_.adjustSelectionBeforeRecording = requestedValue;
    logger_.Info(
        requestedValue
            ? L"已启用框选后调整选区；下次框选将在确认后开始录制。"
            : L"已关闭框选后调整选区；下次松开鼠标后立即开始录制。");
}

void AppController::BeginApplyDownloadedUpdate(
    std::filesystem::path downloadedExecutable) {
    if (downloadedExecutable.empty()) {
        return;
    }
    updateExecutableToApply_ = std::move(downloadedExecutable);
    applyUpdateOnExit_ = true;
    RequestExit();
}

void AppController::ReconcileStartupAtLaunch() {
    const startup::RegistrationResult result =
        startup::ReconcileCurrentUser(startupEnabled_);
    if (result.success) {
        if (result.legacyCleanupError != ERROR_SUCCESS) {
            logger_.Error(std::format(
                L"旧版开机自启项 {} 清理失败，错误码 {}：{}",
                startup::LegacyRunRegistryValueName,
                result.legacyCleanupError,
                win32::FormatLastError(result.legacyCleanupError)));
        }
        if (result.changed) {
            logger_.Info(
                startupEnabled_
                    ? L"已更新当前用户开机自启路径：" + result.command
                    : L"已移除当前用户开机自启项。");
        }
        if (!configStore_.SaveStartupEnabled(startupEnabled_)) {
            logger_.Error(
                L"开机自启偏好保存失败：" + configStore_.FilePath().wstring());
            trayIcon_.ShowNotification(
                L"开机自启设置未保存",
                L"启动项已经更新，但无法写入 SuperRecording 配置文件。",
                NIIF_WARNING);
        }
        return;
    }

    LogStartupFailure(L"启动校正", result);
    if (startupEnabled_) {
        trayIcon_.ShowNotification(
            L"开机自启暂未生效",
            std::format(
                L"无法注册当前用户启动项，已保留开启偏好，下次启动将自动重试。错误码 {}。",
                result.nativeError),
            NIIF_WARNING);
        return;
    }

    trayIcon_.ShowNotification(
        L"无法关闭开机自启",
        std::format(
            L"当前用户启动项可能仍然存在。错误码 {}。",
            result.nativeError),
        NIIF_WARNING);
}

void AppController::ToggleStartup() {
    const bool previousValue = startupEnabled_;
    const bool requestedValue = !previousValue;
    const startup::RegistrationResult result =
        startup::ReconcileCurrentUser(requestedValue);
    if (!result.success) {
        LogStartupFailure(requestedValue ? L"启用" : L"关闭", result);
        if (!configStore_.SaveStartupEnabled(previousValue)) {
            logger_.Error(
                L"开机自启变更失败后，配置回滚也失败：" +
                configStore_.FilePath().wstring());
        }
        trayIcon_.ShowNotification(
            L"开机自启设置失败",
            std::format(
                L"{}失败，已保持原设置。错误码 {}。",
                requestedValue ? L"注册当前用户启动项" : L"移除当前用户启动项",
                result.nativeError),
            NIIF_WARNING);
        return;
    }

    if (!configStore_.SaveStartupEnabled(requestedValue)) {
        logger_.Error(
            L"开机自启偏好保存失败，正在回滚启动项：" +
            configStore_.FilePath().wstring());
        const startup::RegistrationResult rollback =
            startup::ReconcileCurrentUser(previousValue);
        if (!rollback.success) {
            LogStartupFailure(L"配置失败后的注册表回滚", rollback);
        }
        if (!configStore_.SaveStartupEnabled(previousValue)) {
            logger_.Error(
                L"开机自启偏好回写原值失败：" +
                configStore_.FilePath().wstring());
        }
        trayIcon_.ShowNotification(
            L"开机自启设置未保存",
            rollback.success
                ? L"无法写入 SuperRecording 配置文件，已保持原设置。"
                : L"配置与启动项均无法回滚，请检查当前用户注册表权限。",
            NIIF_WARNING);
        return;
    }

    startupEnabled_ = requestedValue;
    if (result.legacyCleanupError != ERROR_SUCCESS) {
        logger_.Error(std::format(
            L"旧版开机自启项 {} 清理失败，错误码 {}：{}",
            startup::LegacyRunRegistryValueName,
            result.legacyCleanupError,
            win32::FormatLastError(result.legacyCleanupError)));
    }
    logger_.Info(
        startupEnabled_
            ? L"已启用当前用户开机自启：" + result.command
            : L"已关闭当前用户开机自启。");
    trayIcon_.ShowNotification(
        startupEnabled_ ? L"已启用开机自启" : L"已关闭开机自启",
        startupEnabled_
            ? L"SuperRecording 将在当前用户登录 Windows 后自动启动。"
            : L"SuperRecording 不会再随当前用户登录自动启动。",
        NIIF_INFO);
}

void AppController::LogStartupFailure(
    const std::wstring_view context,
    const startup::RegistrationResult& result) {
    logger_.Error(std::format(
        L"开机自启{}失败（{}），目标 HKCU\\{}\\{}，错误码 {}：{}",
        context,
        StartupOperationLabel(result.operation),
        startup::RunRegistrySubKey,
        startup::RunRegistryValueName,
        result.nativeError,
        win32::FormatLastError(result.nativeError)));
}

void AppController::ActivateCurrentSurface() {
    if (state_ == RecordingState::Idle) {
        StartSelection();
        return;
    }
    if (state_ == RecordingState::Editing && editor_ && editor_->IsOpen()) {
        const HWND editorWindow = editor_->WindowHandle();
        ::ShowWindow(editorWindow, SW_RESTORE);
        ::SetForegroundWindow(editorWindow);
        return;
    }
    if (state_ == RecordingState::Recording || state_ == RecordingState::Paused) {
        const HWND overlayWindow = recordingOverlay_.WindowHandle();
        if (overlayWindow != nullptr) {
            ::SetWindowPos(overlayWindow, HWND_TOPMOST, 0, 0, 0, 0,
                           SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        }
        return;
    }
    trayIcon_.ShowNotification(L"正在处理录屏", L"编码器正在写入视频尾部，请稍候。", NIIF_INFO);
}

void AppController::CompleteExit() {
    if (window_ == nullptr) {
        return;
    }
    if (applyUpdateOnExit_) {
        DWORD pathError = ERROR_SUCCESS;
        const std::filesystem::path targetExecutable =
            win32::CurrentExecutablePath(&pathError);
        if (targetExecutable.empty()) {
            applyUpdateOnExit_ = false;
            exitAfterFinalize_ = false;
            const std::wstring message = L"无法读取当前程序路径：" +
                win32::FormatLastError(pathError);
            logger_.Error(L"无法启动更新：" + message);
            win32::ShowError(window_, L"SuperRecording 更新失败", message);
            return;
        }

        const update::BootstrapResult launchResult = update::LaunchApplyUpdate(
            updateExecutableToApply_,
            targetExecutable,
            ::GetCurrentProcessId());
        if (!launchResult.success) {
            applyUpdateOnExit_ = false;
            exitAfterFinalize_ = false;
            std::wstring message = launchResult.message.empty()
                ? L"无法启动新版更新引导程序。"
                : launchResult.message;
            if (launchResult.nativeError != ERROR_SUCCESS) {
                message.append(L"\n\n系统错误：");
                message.append(win32::FormatLastError(launchResult.nativeError));
            }
            logger_.Error(L"无法启动更新：" + message);
            win32::ShowError(window_, L"SuperRecording 更新失败", message);
            return;
        }
        logger_.Info(L"新版更新引导程序已启动，当前实例开始安全退出。");
    }
    ::DestroyWindow(window_);
}

void AppController::RequestExit() {
    if (shuttingDown_) {
        return;
    }
    if (state_ == RecordingState::Recording || state_ == RecordingState::Paused) {
        const int answer = ::MessageBoxW(
            window_,
            L"当前正在录制。结束录制并退出吗？\n原始 MP4 会保留在临时目录。",
            L"退出 SuperRecording",
            MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2 | MB_SETFOREGROUND);
        if (answer != IDYES) {
            applyUpdateOnExit_ = false;
            return;
        }
        exitAfterFinalize_ = true;
        StopCapture();
        return;
    }
    if (state_ == RecordingState::Finalizing) {
        exitAfterFinalize_ = true;
        return;
    }
    if (editor_) {
        if (!editor_->Close()) {
            applyUpdateOnExit_ = false;
            return;
        }
        editor_.reset();
    }
    CompleteExit();
}

void AppController::Shutdown() {
    if (shuttingDown_) {
        return;
    }
    shuttingDown_ = true;
    if (window_ != nullptr) {
        static_cast<void>(::UnregisterHotKey(window_, kHotkeySlotA));
        static_cast<void>(::UnregisterHotKey(window_, kHotkeySlotB));
        hotkeyRegistered_ = false;
        activeHotkeyId_ = 0;
    }
    appUpdateController_.Shutdown();
    recordingOverlay_.Destroy();
    recordingRegionFrame_.Destroy();
    if (editor_) {
        editor_->CloseForShutdown();
        editor_.reset();
    }
    if (finalizer_.joinable() && finalizer_.get_id() != std::this_thread::get_id()) {
        finalizer_.join();
    } else if (captureEngine_.State() == RecordingState::Recording ||
               captureEngine_.State() == RecordingState::Paused ||
               captureEngine_.State() == RecordingState::Finalizing) {
        capture::CaptureError ignored;
        static_cast<void>(captureEngine_.Stop(&ignored));
    }
    trayIcon_.Remove();
    if (mediaFoundationStarted_) {
        ::MFShutdown();
        mediaFoundationStarted_ = false;
    }
    logger_.Info(L"应用退出。 ");
}

std::optional<std::filesystem::path> AppController::PrepareRecordingCacheDirectory(
    std::wstring* errorMessage) const {
    const std::filesystem::path preferred =
        settings_.saveDirectory / product::RecordingCacheDirectoryName;
    std::wstring preferredError;
    if (win32::EnsureDirectory(preferred, &preferredError)) {
        const DWORD attributes = ::GetFileAttributesW(preferred.c_str());
        if (attributes != INVALID_FILE_ATTRIBUTES &&
            (attributes & FILE_ATTRIBUTE_HIDDEN) == 0) {
            static_cast<void>(::SetFileAttributesW(
                preferred.c_str(), attributes | FILE_ATTRIBUTE_HIDDEN));
        }
        if (ProbeWritableDirectory(preferred, true)) {
            return preferred;
        }
        preferredError = L"目录不可写，或所在文件系统不支持毫秒级硬链接提交。";
    }

    const std::filesystem::path fallback = win32::LocalAppDataDirectory() / L"Temp";
    std::wstring fallbackError;
    if (win32::EnsureDirectory(fallback, &fallbackError) &&
        ProbeWritableDirectory(fallback, false)) {
        return fallback;
    }
    if (fallbackError.empty()) {
        fallbackError = L"系统缓存目录不可写。";
    }

    if (errorMessage != nullptr) {
        *errorMessage = L"无法创建录制缓存目录。\n默认保存位置：" + preferredError +
            L"\n系统缓存位置：" + fallbackError;
    }
    return std::nullopt;
}

std::filesystem::path AppController::NextTemporaryRecordingPath(
    const std::filesystem::path& cacheDirectory,
    std::wstring* errorMessage) {
    return win32::MakeUniquePath(
        cacheDirectory, L"录屏", L".mp4", errorMessage);
}

bool AppController::IsBusy() const noexcept {
    return state_ != RecordingState::Idle;
}

std::wstring AppController::CaptureErrorText(const capture::CaptureError& error) {
    if (!error.message.empty()) {
        return error.message;
    }
    if (error.nativeCode != 0) {
        return win32::FormatError(static_cast<HRESULT>(error.nativeCode));
    }
    return L"未知录制错误。";
}

}  // namespace qrec
