#include "editor/EditorWindow.h"

#include "app/resource.h"
#include "common/Win32Helpers.h"
#include "editor/EditorAudioToggle.h"
#include "editor/EditorChrome.h"
#include "editor/EditorSpeedControl.h"
#include "editor/EditorTheme.h"
#include "editor/EditorTimeFormat.h"
#include "editor/MediaPreview.h"
#include "editor/TrimTimeline.h"
#include "editor/WarmCacheCoordinator.h"
#include "media/ExportQuality.h"
#include "media/Mp4BoundaryEncoderPool.h"
#include "media/MediaExporter.h"
#include "ui/Motion.h"

#include <commctrl.h>
#include <dwmapi.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <format>
#include <mutex>
#include <optional>
#include <ranges>
#include <utility>

#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "dwmapi.lib")

namespace qrec {
namespace {

constexpr wchar_t kEditorWindowClassName[] = L"SuperRecording.EditorWindow";
constexpr UINT_PTR kPlaybackTimer = 1;
constexpr UINT_PTR kTimelineSeekTimer = 2;
constexpr UINT_PTR kWarmCacheDebounceTimer = 3;
constexpr UINT_PTR kTimelineInteractionTimer = 4;
constexpr UINT_PTR kPreviewProxyDebounceTimer = 5;
constexpr UINT kPlaybackRefreshMilliseconds = 16;
constexpr UINT kTimelineInteractionFrameMilliseconds = 16;
constexpr auto kPlaybackTextRefreshInterval = std::chrono::milliseconds(33);
constexpr auto kTimelineSeekPreviewInterval = std::chrono::milliseconds(16);
constexpr auto kProgressUiInterval = std::chrono::milliseconds(100);
constexpr UINT kWarmCacheMp4DebounceMilliseconds = 16;
constexpr UINT kWarmCacheGifDebounceMilliseconds = 350;
constexpr UINT kWarmCacheSpeedDebounceMilliseconds = 250;
constexpr UINT kWarmCacheQualityDebounceMilliseconds = 250;
constexpr UINT kPreviewProxyDebounceMilliseconds = 250;
constexpr UINT_PTR kFormatButtonSubclassId = 0x7201;
constexpr UINT kExportProgressMessage = WM_APP + 0x231;
constexpr UINT kExportCompletedMessage = WM_APP + 0x232;
constexpr UINT kWarmCacheProgressMessage = WM_APP + 0x233;
constexpr UINT kWarmCacheCompletedMessage = WM_APP + 0x234;
constexpr UINT kDeferredEditorCommandMessage = WM_APP + 0x23A;
constexpr UINT kRefreshPreviewVideoMessage = WM_APP + 0x23B;
constexpr UINT kPreviewProxyCompletedMessage = WM_APP + 0x23C;

constexpr int kPreviewId = 1001;
constexpr int kTimelineId = 1002;
constexpr int kPlayButtonId = 1003;
constexpr int kTimeTextId = 1004;
constexpr int kRangeTextId = 1005;
constexpr int kMp4RadioId = 1006;
constexpr int kGifRadioId = 1007;
constexpr int kSaveButtonId = 1008;
constexpr int kCopyButtonId = 1009;
constexpr int kStatusTextId = 1010;
constexpr int kHeaderTitleId = 1011;
constexpr int kHeaderSubtitleId = 1012;
constexpr int kFormatLabelId = 1013;
constexpr int kAudioToggleId = 1014;
constexpr int kSpeedControlId = 1015;
constexpr int kTrimStartButtonId = 1016;
constexpr int kTrimEndButtonId = 1017;
constexpr int kQualityControlId = 1018;
constexpr int kOutputSizeTextId = 1019;

constexpr wchar_t kTrimStartAccessibleName[] = L"设为起点";
constexpr wchar_t kTrimEndAccessibleName[] = L"设为终点";

enum class ExportAction : unsigned char {
    Save,
    Copy,
};

std::wstring_view ExportActionName(const ExportAction action) noexcept {
    return action == ExportAction::Copy ? L"复制" : L"保存";
}

std::wstring_view OutputFormatName(const OutputFormat format) noexcept {
    return format == OutputFormat::Gif ? L"GIF" : L"MP4";
}

std::wstring_view ExportDispositionName(
    const MediaExportDisposition disposition) noexcept {
    switch (disposition) {
    case MediaExportDisposition::AudioMuxed:
        return L"音视频快速重封装";
    case MediaExportDisposition::BoundaryTrimmedHybrid:
        return L"边界编码 + 原片直通";
    case MediaExportDisposition::SmartTrimmedPassthrough:
        return L"智能无损裁切";
    case MediaExportDisposition::HardLinkedPassthrough:
        return L"原片硬链接";
    case MediaExportDisposition::CopiedPassthrough:
        return L"原片文件复制";
    case MediaExportDisposition::OriginalPassthrough:
        return L"原片直通";
    case MediaExportDisposition::CachedArtifact:
        return L"缓存成片";
    case MediaExportDisposition::Transcoded:
    default:
        return L"重新编码";
    }
}

struct WarmCacheProgressState final {
    std::uint64_t generation{};
    ExportProgress progress;
};

struct WarmCacheResultState final {
    std::uint64_t generation{};
    MediaExportResult result;
};

struct PreviewReloadState final {
    std::chrono::milliseconds position{};
    bool resumePlayback{};
};

int Scale(const HWND window, const int value) noexcept {
    const UINT dpi = window != nullptr ? ::GetDpiForWindow(window) : USER_DEFAULT_SCREEN_DPI;
    return ::MulDiv(value, static_cast<int>(dpi), USER_DEFAULT_SCREEN_DPI);
}

std::wstring ExtensionFor(const OutputFormat format) {
    return format == OutputFormat::Gif ? L".gif" : L".mp4";
}

void SetControlFont(const HWND control, const HFONT font) {
    if (control != nullptr) {
        ::SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
    }
}

void ApplyEditorDarkTitleBar(const HWND window) noexcept {
    if (window == nullptr) {
        return;
    }

    constexpr auto useImmersiveDarkMode = static_cast<DWMWINDOWATTRIBUTE>(20);
    constexpr auto useImmersiveDarkModeLegacy = static_cast<DWMWINDOWATTRIBUTE>(19);
    constexpr auto borderColorAttribute = static_cast<DWMWINDOWATTRIBUTE>(34);
    constexpr auto captionColorAttribute = static_cast<DWMWINDOWATTRIBUTE>(35);
    constexpr auto textColorAttribute = static_cast<DWMWINDOWATTRIBUTE>(36);

    const BOOL enabled = TRUE;
    const HRESULT immersiveResult = ::DwmSetWindowAttribute(
        window,
        useImmersiveDarkMode,
        &enabled,
        sizeof(enabled));
    if (FAILED(immersiveResult)) {
        static_cast<void>(::DwmSetWindowAttribute(
            window,
            useImmersiveDarkModeLegacy,
            &enabled,
            sizeof(enabled)));
    }

    const COLORREF borderColor = editor_theme::TitleBarBorder;
    const COLORREF captionColor = editor_theme::TitleBar;
    const COLORREF textColor = editor_theme::TitleBarText;
    static_cast<void>(::DwmSetWindowAttribute(
        window,
        borderColorAttribute,
        &borderColor,
        sizeof(borderColor)));
    static_cast<void>(::DwmSetWindowAttribute(
        window,
        captionColorAttribute,
        &captionColor,
        sizeof(captionColor)));
    static_cast<void>(::DwmSetWindowAttribute(
        window,
        textColorAttribute,
        &textColor,
        sizeof(textColor)));
    static_cast<void>(::RedrawWindow(
        window,
        nullptr,
        nullptr,
        RDW_FRAME | RDW_INVALIDATE));
}

}  // namespace

class EditorWindow::Impl final {
public:
    Impl(const HINSTANCE instance, const HWND owner)
        : instance_(instance), owner_(owner) {}

    ~Impl() {
        CloseForShutdown();
        DiscardBoundaryEncoderPreparation();
        StopWarmCacheAndWait();
        StopPreviewProxyAndWait();
        exporter_.CancelAndWait();
    }

    [[nodiscard]] bool Open(
        RecordingResult recording,
        AppSettings settings,
        EditorWindowCallbacks callbacks,
        std::wstring* errorMessage) {
        if (window_ != nullptr && ::IsWindow(window_) != FALSE) {
            ::ShowWindow(window_, SW_RESTORE);
            ::SetForegroundWindow(window_);
            if (errorMessage != nullptr) {
                *errorMessage = L"已有录屏编辑窗口正在打开。";
            }
            return false;
        }
        std::error_code sourceStatusError;
        const bool sourceIsFile = !recording.sourcePath.empty() &&
            std::filesystem::is_regular_file(recording.sourcePath, sourceStatusError);
        if (!sourceIsFile || sourceStatusError) {
            if (errorMessage != nullptr) {
                *errorMessage = L"源录屏不存在或无法读取：" + recording.sourcePath.wstring();
                if (sourceStatusError) {
                    *errorMessage += L"\n错误码：" +
                        std::to_wstring(sourceStatusError.value());
                }
            }
            return false;
        }
        if (!RegisterWindowClass()) {
            if (errorMessage != nullptr) {
                *errorMessage = L"无法注册录屏编辑窗口：" + win32::FormatLastError();
            }
            return false;
        }

        recording_ = std::move(recording);
        boundaryEncoderGeneration_ =
            detail::Mp4BoundaryEncoderPool::Shared().Prepare(
                recording_.sourcePath);
        settings_ = std::move(settings);
        callbacks_ = std::move(callbacks);
        // 输出格式只属于当前编辑会话；每次打开编辑器都从 MP4 开始。
        selectedFormat_ = OutputFormat::Mp4;
        audioRequestedForMp4_ = false;
        if (settings_.saveDirectory.empty()) {
            settings_.saveDirectory = win32::DefaultVideoDirectory();
        }
        duration_ = std::max(recording_.duration, std::chrono::milliseconds(1));
        trimStart_ = std::chrono::milliseconds(0);
        trimEnd_ = duration_;
        trimRangeEdited_ = false;
        playbackSpeedTenths_ = EditorSpeedControl::DefaultSpeedTenths;
        qualityPercent_ = media::ExportQuality::Normalize(
            settings_.outputQualityPercent);
        settings_.outputQualityPercent = qualityPercent_;
        estimatedOutputBytes_ = 0;
        exactOutputBytes_ = 0;
        outputSizeExact_ = false;
        displayedOutputSizeText_.clear();
        mediaItemReady_ = false;
        speedInteractionActive_ = false;
        qualityInteractionActive_ = false;
        activePreviewPath_.clear();
        previewReloadState_.reset();
        WriteDiagnostic(std::format(
            L"编辑器打开：format=MP4，trimStart={} ms，trimEnd={} ms，duration={} ms，"
            L"recordingDuration={} ms，trimRangeEdited=否",
            trimStart_.count(),
            trimEnd_.count(),
            duration_.count(),
            recording_.duration.count()));
        creationError_.clear();
        displayedPlayButtonText_.clear();
        displayedTimeText_.clear();
        displayedRangeText_.clear();
        displayedStatusText_.clear();
        pendingTimelineSeek_.reset();
        timelineSeekTimerArmed_ = false;
        timelineInteractionTimerArmed_ = false;
        pendingTimelinePause_ = false;
        pendingTimelineSeekFinal_ = false;
        previewSeekInFlight_ = false;
        inFlightTimelineSeek_.reset();
        playAfterTimelineSeek_ = false;
        timelineRangeInteractionActive_ = false;
        lastPreviewSeekIssuedAt_ = {};
        lastTimelineLabelRefresh_ = {};
        lastTimeLabelPositionBucket_ = std::chrono::milliseconds(-1);
        lastTimeLabelDuration_ = std::chrono::milliseconds(-1);
        lastRangeLabelStart_ = std::chrono::milliseconds(-1);
        lastRangeLabelEnd_ = std::chrono::milliseconds(-1);
        lastRangeLabelSpeedTenths_ = -1;
        timelinePreviewNotificationCount_ = 0;
        timelineCommittedNotificationCount_ = 0;
        previewSeekRequestCount_ = 0;
        previewSeekIssuedCount_ = 0;
        previewSeekCoalescedCount_ = 0;
        previewSeekDurationSampleCount_ = 0;
        previewSeekDurationSamples_.fill(0);
        warmCacheProgressMessagePending_.store(false, std::memory_order_release);
        warmCacheCompletionMessagePending_.store(false, std::memory_order_release);
        previewProxyCompletionMessagePending_.store(false, std::memory_order_release);
        exportProgressMessagePending_.store(false, std::memory_order_release);
        lastExportProgressSignalMilliseconds_.store(0, std::memory_order_release);

        const UINT dpi = owner_ != nullptr ? ::GetDpiForWindow(owner_) : USER_DEFAULT_SCREEN_DPI;
        RECT rectangle{0, 0, ScaleForDpi(1040, dpi), ScaleForDpi(720, dpi)};
        ::AdjustWindowRectExForDpi(
            &rectangle,
            WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
            FALSE,
            WS_EX_APPWINDOW,
            dpi);

        window_ = ::CreateWindowExW(
            WS_EX_APPWINDOW,
            kEditorWindowClassName,
            L"SuperRecording - 编辑录屏",
            WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
            CW_USEDEFAULT,
            CW_USEDEFAULT,
            rectangle.right - rectangle.left,
            rectangle.bottom - rectangle.top,
            owner_,
            nullptr,
            instance_,
            this);
        if (window_ == nullptr) {
            DiscardBoundaryEncoderPreparation();
            if (errorMessage != nullptr) {
                *errorMessage = creationError_.empty()
                    ? L"无法创建录屏编辑窗口：" + win32::FormatLastError()
                    : creationError_;
            }
            return false;
        }
        ApplyEditorDarkTitleBar(window_);
        notificationTarget_.store(window_, std::memory_order_release);
        if (!InitializeWarmCacheCoordinator()) {
            WriteDiagnostic(L"后台预生成协调器启动失败；导出仍可按需执行。");
        }

        std::wstring previewError;
        if (!preview_.Open(recording_.sourcePath, previewHost_, window_, &previewError)) {
            SetStatus(L"预览不可用；仍可导出。" + previewError, EditorStatusTone::Error);
        } else {
            activePreviewPath_ = recording_.sourcePath;
        }
        CenterOnRecordingMonitor();
        ::ShowWindow(window_, SW_SHOW);
        ::UpdateWindow(window_);
        ::SetForegroundWindow(window_);
        if (qualityPercent_ < media::ExportQuality::DefaultPercent) {
            ScheduleWarmCache(kWarmCacheQualityDebounceMilliseconds);
            SchedulePreviewProxy(kPreviewProxyDebounceMilliseconds);
            UpdateReadyStatus();
        }
        return true;
    }

    [[nodiscard]] bool Close() {
        if (window_ == nullptr || ::IsWindow(window_) == FALSE) {
            return true;
        }
        ::SendMessageW(window_, WM_CLOSE, 0, 0);
        return window_ == nullptr || ::IsWindow(window_) == FALSE;
    }

    void CloseForShutdown() noexcept {
        if (window_ == nullptr || ::IsWindow(window_) == FALSE) {
            return;
        }
        forceClose_ = true;
        ::SendMessageW(window_, WM_CLOSE, 0, 0);
    }

    [[nodiscard]] HWND WindowHandle() const noexcept { return window_; }
    [[nodiscard]] bool IsOpen() const noexcept {
        return window_ != nullptr && ::IsWindow(window_) != FALSE;
    }

private:
    void DiscardBoundaryEncoderPreparation() noexcept {
        if (boundaryEncoderGeneration_ == 0) {
            return;
        }
        detail::Mp4BoundaryEncoderPool::Shared().Discard(
            recording_.sourcePath,
            boundaryEncoderGeneration_);
        boundaryEncoderGeneration_ = 0;
    }

    void CenterOnRecordingMonitor() const noexcept {
        if (window_ == nullptr) {
            return;
        }
        const POINT recordingCenter{
            recording_.region.left + recording_.region.Width() / 2,
            recording_.region.top + recording_.region.Height() / 2};
        const HMONITOR monitor = ::MonitorFromPoint(
            recordingCenter,
            MONITOR_DEFAULTTONEAREST);
        MONITORINFO information{};
        information.cbSize = sizeof(information);
        if (monitor == nullptr ||
            ::GetMonitorInfoW(monitor, &information) == FALSE) {
            return;
        }
        const int workWidth = information.rcWork.right - information.rcWork.left;
        const int workHeight = information.rcWork.bottom - information.rcWork.top;
        if (workWidth <= 0 || workHeight <= 0) {
            return;
        }

        const auto placeInsideWorkArea = [this, &information, workWidth, workHeight]() noexcept {
            RECT windowBounds{};
            if (::GetWindowRect(window_, &windowBounds) == FALSE) {
                return;
            }
            const int width = std::min(
                workWidth,
                std::max(1, static_cast<int>(windowBounds.right - windowBounds.left)));
            const int height = std::min(
                workHeight,
                std::max(1, static_cast<int>(windowBounds.bottom - windowBounds.top)));
            const int x = information.rcWork.left + (workWidth - width) / 2;
            const int y = information.rcWork.top + (workHeight - height) / 2;
            ::SetWindowPos(
                window_,
                nullptr,
                x,
                y,
                width,
                height,
                SWP_NOZORDER | SWP_NOACTIVATE);
        };

        // Move without resizing first so WM_DPICHANGED can establish the target monitor DPI.
        // Only then clamp and center the resulting physical bounds inside the work area.
        ::SetWindowPos(
            window_,
            nullptr,
            information.rcWork.left,
            information.rcWork.top,
            0,
            0,
            SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
        placeInsideWorkArea();
    }

    static int ScaleForDpi(const int value, const UINT dpi) noexcept {
        return ::MulDiv(value, static_cast<int>(dpi), USER_DEFAULT_SCREEN_DPI);
    }

    bool RegisterWindowClass() const {
        WNDCLASSEXW existing{};
        existing.cbSize = sizeof(existing);
        if (::GetClassInfoExW(instance_, kEditorWindowClassName, &existing) != FALSE) {
            return true;
        }
        WNDCLASSEXW windowClass{};
        windowClass.cbSize = sizeof(windowClass);
        windowClass.style = 0;
        windowClass.lpfnWndProc = &Impl::WindowProc;
        windowClass.hInstance = instance_;
        windowClass.hIcon = static_cast<HICON>(::LoadImageW(
            instance_, MAKEINTRESOURCEW(IDI_SUPER_RECORDING), IMAGE_ICON,
            0, 0, LR_DEFAULTSIZE | LR_SHARED));
        windowClass.hIconSm = windowClass.hIcon;
        windowClass.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
        windowClass.lpszClassName = kEditorWindowClassName;
        return ::RegisterClassExW(&windowClass) != 0 ||
            ::GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
    }

    static LRESULT CALLBACK WindowProc(
        const HWND window,
        const UINT message,
        const WPARAM wParam,
        const LPARAM lParam) {
        Impl* self = reinterpret_cast<Impl*>(
            ::GetWindowLongPtrW(window, GWLP_USERDATA));
        if (message == WM_NCCREATE) {
            const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lParam);
            self = static_cast<Impl*>(create->lpCreateParams);
            self->window_ = window;
            ::SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        }
        if (self != nullptr) {
            try {
                return self->HandleMessage(message, wParam, lParam);
            } catch (...) {
                return 0;
            }
        }
        return ::DefWindowProcW(window, message, wParam, lParam);
    }

    LRESULT HandleMessage(const UINT message, const WPARAM wParam, const LPARAM lParam) {
        switch (message) {
        case WM_CREATE:
            if (!CreateControls()) {
                creationError_ = L"无法创建录屏编辑控件。";
                return -1;
            }
            return 0;
        case WM_SIZE:
            LayoutControls(LOWORD(lParam), HIWORD(lParam));
            return 0;
        case WM_EXITSIZEMOVE:
            previewVideoRefreshPosted_ = false;
            warmCacheProgressMessagePending_.store(false, std::memory_order_release);
            warmCacheCompletionMessagePending_.store(false, std::memory_order_release);
            exportProgressMessagePending_.store(false, std::memory_order_release);
            preview_.UpdateVideo();
            return 0;
        case WM_PAINT:
            chrome_.PaintWindow(
                window_, editorPanelTop_, previewStage_, statusDotCenter_, statusTone_);
            return 0;
        case WM_DRAWITEM:
            return DrawButton(reinterpret_cast<const DRAWITEMSTRUCT*>(lParam)) ? TRUE : FALSE;
        case WM_GETMINMAXINFO: {
            auto* information = reinterpret_cast<MINMAXINFO*>(lParam);
            const UINT dpi = ::GetDpiForWindow(window_);
            RECT minimumWindow{
                0,
                0,
                ScaleForDpi(760, dpi),
                ScaleForDpi(590, dpi)};
            const DWORD style = static_cast<DWORD>(::GetWindowLongPtrW(window_, GWL_STYLE));
            const DWORD extendedStyle =
                static_cast<DWORD>(::GetWindowLongPtrW(window_, GWL_EXSTYLE));
            ::AdjustWindowRectExForDpi(
                &minimumWindow,
                style,
                ::GetMenu(window_) != nullptr,
                extendedStyle,
                dpi);
            information->ptMinTrackSize.x = minimumWindow.right - minimumWindow.left;
            information->ptMinTrackSize.y = minimumWindow.bottom - minimumWindow.top;
            return 0;
        }
        case WM_DPICHANGED: {
            const auto* suggested = reinterpret_cast<const RECT*>(lParam);
            ::SetWindowPos(
                window_, nullptr,
                suggested->left, suggested->top,
                suggested->right - suggested->left,
                suggested->bottom - suggested->top,
                SWP_NOZORDER | SWP_NOACTIVATE);
            RecreateFonts();
            LayoutFromClient();
            return 0;
        }
        case WM_THEMECHANGED:
        case WM_SETTINGCHANGE:
            if (message == WM_SETTINGCHANGE) {
                static_cast<void>(ui::RefreshClientAreaAnimationsEnabled());
                if (audioToggle_.WindowHandle() != nullptr) {
                    ::SendMessageW(
                        audioToggle_.WindowHandle(),
                        WM_SETTINGCHANGE,
                        wParam,
                        lParam);
                }
                if (speedControl_.WindowHandle() != nullptr) {
                    ::SendMessageW(
                        speedControl_.WindowHandle(),
                        WM_SETTINGCHANGE,
                        wParam,
                        lParam);
                }
                if (qualityControl_.WindowHandle() != nullptr) {
                    ::SendMessageW(
                        qualityControl_.WindowHandle(),
                        WM_SETTINGCHANGE,
                        wParam,
                        lParam);
                }
            }
            ApplyEditorDarkTitleBar(window_);
            ::InvalidateRect(window_, nullptr, FALSE);
            return ::DefWindowProcW(window_, message, wParam, lParam);
        case WM_COMMAND:
            HandleCommand(LOWORD(wParam), HIWORD(wParam));
            return 0;
        case WM_NOTIFY:
            HandleNotification(reinterpret_cast<const NMHDR*>(lParam));
            return 0;
        case TimelineInteractionMessage:
            ScheduleTimelineInteractionDrain();
            return 0;
        case WM_TIMER:
            if (wParam == kPlaybackTimer) {
                UpdatePlaybackPosition();
                return 0;
            }
            if (wParam == kTimelineSeekTimer) {
                PumpPendingTimelineSeek();
                return 0;
            }
            if (wParam == kWarmCacheDebounceTimer) {
                ::KillTimer(window_, kWarmCacheDebounceTimer);
                warmCacheDebounceArmed_ = false;
                StartWarmCache();
                return 0;
            }
            if (wParam == kTimelineInteractionTimer) {
                ::KillTimer(window_, kTimelineInteractionTimer);
                timelineInteractionTimerArmed_ = false;
                HandlePendingTimelineInteraction();
                return 0;
            }
            if (wParam == kPreviewProxyDebounceTimer) {
                ::KillTimer(window_, kPreviewProxyDebounceTimer);
                previewProxyDebounceArmed_ = false;
                StartPreviewProxy();
                return 0;
            }
            break;
        case PreviewEventMessage:
            HandlePreviewEvent(
                static_cast<MFP_EVENT_TYPE>(wParam), static_cast<HRESULT>(lParam));
            return 0;
        case kDeferredEditorCommandMessage:
            ExecuteCommand(static_cast<int>(wParam));
            return 0;
        case kRefreshPreviewVideoMessage:
            previewVideoRefreshPosted_ = false;
            preview_.UpdateVideo();
            return 0;
        case kExportProgressMessage:
            HandleExportProgress();
            return 0;
        case kExportCompletedMessage:
            HandleExportCompleted();
            return 0;
        case kWarmCacheProgressMessage:
            HandleWarmCacheProgress();
            return 0;
        case kWarmCacheCompletedMessage:
            HandleWarmCacheCompleted();
            return 0;
        case kPreviewProxyCompletedMessage:
            HandlePreviewProxyCompleted();
            return 0;
        case WM_KEYDOWN:
            if (wParam == VK_SPACE &&
                (static_cast<LPARAM>(lParam) & (1LL << 30)) == 0 &&
                !exporter_.IsRunning()) {
                TogglePlayback();
                return 0;
            }
            if (wParam == VK_ESCAPE) {
                ::SendMessageW(window_, WM_CLOSE, 0, 0);
                return 0;
            }
            break;
        case WM_CTLCOLORSTATIC: {
            const HDC dc = reinterpret_cast<HDC>(wParam);
            const HWND control = reinterpret_cast<HWND>(lParam);
            COLORREF textColor = editor_theme::TextPrimary;
            if (control == statusText_) {
                textColor = chrome_.StatusColor(statusTone_);
            } else if (control == outputSizeText_) {
                textColor = outputSizeExact_
                    ? editor_theme::TextPrimary
                    : editor_theme::TextSecondary;
            } else if (control == headerSubtitle_ || control == formatLabel_) {
                textColor = editor_theme::TextSecondary;
            }
            ::SetTextColor(dc, textColor);
            if (control == previewHost_) {
                ::SetBkColor(dc, editor_theme::VideoStage);
                return reinterpret_cast<LRESULT>(chrome_.VideoBrush());
            }
            ::SetBkMode(dc, TRANSPARENT);
            return reinterpret_cast<LRESULT>(chrome_.PanelBrush());
        }
        case WM_CTLCOLORBTN: {
            const HDC dc = reinterpret_cast<HDC>(wParam);
            ::SetBkColor(dc, editor_theme::Panel);
            ::SetTextColor(dc, editor_theme::TextPrimary);
            return reinterpret_cast<LRESULT>(chrome_.PanelBrush());
        }
        case WM_ERASEBKGND:
            return 1;
        case WM_CLOSE:
            if (exporter_.IsRunning() && !forceClose_) {
                const int answer = ::MessageBoxW(
                    window_,
                    L"正在导出。关闭窗口将取消本次导出，源录屏会保留。",
                    L"关闭编辑窗口",
                    MB_OKCANCEL | MB_ICONQUESTION | MB_DEFBUTTON2);
                if (answer != IDOK) {
                    return 0;
                }
                exporter_.CancelAndWait();
            }
            StopWarmCacheAndWait();
            StopPreviewProxyAndWait();
            ::DestroyWindow(window_);
            return 0;
        case WM_DESTROY:
            WriteInteractionMetrics();
            DiscardBoundaryEncoderPreparation();
            notificationTarget_.store(nullptr, std::memory_order_release);
            StopPlaybackUiTimer();
            ::KillTimer(window_, kTimelineSeekTimer);
            ::KillTimer(window_, kWarmCacheDebounceTimer);
            ::KillTimer(window_, kTimelineInteractionTimer);
            ::KillTimer(window_, kPreviewProxyDebounceTimer);
            timelineSeekTimerArmed_ = false;
            timelineInteractionTimerArmed_ = false;
            pendingTimelineSeek_.reset();
            pendingTimelineSeekFinal_ = false;
            previewSeekInFlight_ = false;
            inFlightTimelineSeek_.reset();
            playAfterTimelineSeek_ = false;
            previewVideoRefreshPosted_ = false;
            mediaItemReady_ = false;
            StopWarmCacheAndWait();
            StopPreviewProxyAndWait();
            exporter_.CancelAndWait();
            preview_.Close();
            return 0;
        case WM_NCDESTROY: {
            ::SetWindowLongPtrW(window_, GWLP_USERDATA, 0);
            window_ = nullptr;
            DeleteUiResources();
            auto closed = std::move(callbacks_.closed);
            if (closed) {
                try {
                    closed();
                } catch (...) {
                }
            }
            return 0;
        }
        default:
            break;
        }
        return ::DefWindowProcW(window_, message, wParam, lParam);
    }

    bool CreateControls() {
        if (!chrome_.Initialize(window_)) {
            return false;
        }

        previewHost_ = ::CreateWindowExW(
            0, WC_STATICW, L"", WS_CHILD | WS_VISIBLE | SS_BLACKRECT,
            0, 0, 1, 1, window_,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(kPreviewId)), instance_, nullptr);
        headerTitle_ = CreateStatic(L"裁剪与导出", kHeaderTitleId, SS_LEFT | SS_CENTERIMAGE);
        headerSubtitle_ = CreateStatic(L"正在读取录屏信息…", kHeaderSubtitleId, SS_LEFT | SS_CENTERIMAGE);
        rangeText_ = CreateStatic(
            L"保留  00:00.00 — 00:00.00  ·  输出 00:00.00",
            kRangeTextId,
            SS_LEFT | SS_CENTERIMAGE | SS_ENDELLIPSIS);
        if (!timeline_.Create(window_, kTimelineId, instance_)) {
            return false;
        }
        if (!speedControl_.Create(
                window_,
                kSpeedControlId,
                instance_,
                [this](
                    const int speedTenths,
                    const EditorSliderInteractionPhase phase) {
                    HandleSpeedChanged(speedTenths, phase);
                })) {
            return false;
        }
        if (!qualityControl_.Create(
                window_,
                kQualityControlId,
                instance_,
                EditorSliderPresentation::QualityPercent,
                [this](
                    const int qualityPercent,
                    const EditorSliderInteractionPhase phase) {
                    HandleQualityChanged(qualityPercent, phase);
                })) {
            return false;
        }
        outputSizeText_ = CreateStatic(
            L"计算中",
            kOutputSizeTextId,
            SS_CENTER | SS_CENTERIMAGE);
        trimStartButton_ = CreateButton(kTrimStartAccessibleName, kTrimStartButtonId);
        trimEndButton_ = CreateButton(kTrimEndAccessibleName, kTrimEndButtonId);
        playButton_ = CreateButton(L"播放", kPlayButtonId);
        timeText_ = CreateStatic(L"00:00.00 / 00:00.00", kTimeTextId, SS_LEFT | SS_CENTERIMAGE);
        if (!audioToggle_.Create(
                window_,
                kAudioToggleId,
                instance_,
                [this](const bool checked) {
                    HandleAudioToggleChanged(checked);
                })) {
            return false;
        }
        formatLabel_ = CreateStatic(L"输出格式", kFormatLabelId, SS_LEFT | SS_CENTERIMAGE);
        mp4Radio_ = CreateButton(L"MP4", kMp4RadioId, WS_GROUP);
        gifRadio_ = CreateButton(L"GIF", kGifRadioId);
        copyButton_ = CreateButton(L"复制到剪贴板", kCopyButtonId);
        saveButton_ = CreateButton(L"保存到本地", kSaveButtonId);
        statusText_ = CreateStatic(L"准备就绪", kStatusTextId, SS_LEFT | SS_CENTERIMAGE);

        const std::array controls{
            previewHost_, headerTitle_, headerSubtitle_, rangeText_, timeline_.WindowHandle(),
            qualityControl_.WindowHandle(), outputSizeText_, speedControl_.WindowHandle(),
            trimStartButton_, trimEndButton_,
            playButton_, timeText_, audioToggle_.WindowHandle(), formatLabel_,
            mp4Radio_, gifRadio_, saveButton_,
            copyButton_, statusText_};
        if (std::ranges::any_of(controls, [](const HWND control) { return control == nullptr; })) {
            return false;
        }

        InitializeTooltips();

        if (mp4Radio_ != nullptr) {
            ::SetWindowSubclass(
                mp4Radio_,
                &Impl::FormatButtonSubclassProc,
                kFormatButtonSubclassId,
                reinterpret_cast<DWORD_PTR>(this));
        }
        if (gifRadio_ != nullptr) {
            ::SetWindowSubclass(
                gifRadio_,
                &Impl::FormatButtonSubclassProc,
                kFormatButtonSubclassId,
                reinterpret_cast<DWORD_PTR>(this));
        }

        SetControlFonts();
        timeline_.SetRange(duration_, trimStart_, trimEnd_);
        timeline_.SetPlayhead(std::chrono::milliseconds::zero());
        speedControl_.SetValueTenths(playbackSpeedTenths_);
        qualityControl_.SetValue(qualityPercent_);
        UpdateTimeLabels(std::chrono::milliseconds(0));
        UpdateOutputSizeEstimate();
        UpdateHeaderSubtitle();
        RefreshAudioToggleState();
        UpdateTrimBoundaryButtonStates();
        return true;
    }

    HWND CreateStatic(const wchar_t* text, const int id, const DWORD style) const {
        return ::CreateWindowExW(
            0, WC_STATICW, text, WS_CHILD | WS_VISIBLE | style,
            0, 0, 1, 1, window_,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), instance_, nullptr);
    }

    HWND CreateButton(const wchar_t* text, const int id, const DWORD extraStyle = 0) {
        const HWND button = ::CreateWindowExW(
            0, WC_BUTTONW, text,
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW | extraStyle,
            0, 0, 1, 1, window_,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), instance_, nullptr);
        if (button != nullptr) {
            chrome_.AttachButton(button);
        }
        return button;
    }

    HWND CreateTooltipWindow() const {
        const HWND tooltip = ::CreateWindowExW(
            WS_EX_TOPMOST,
            TOOLTIPS_CLASSW,
            nullptr,
            WS_POPUP | TTS_ALWAYSTIP | TTS_NOPREFIX,
            CW_USEDEFAULT,
            CW_USEDEFAULT,
            CW_USEDEFAULT,
            CW_USEDEFAULT,
            window_,
            nullptr,
            instance_,
            nullptr);
        if (tooltip != nullptr) {
            ::SetWindowPos(
                tooltip,
                HWND_TOPMOST,
                0,
                0,
                0,
                0,
                SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
            ::SendMessageW(tooltip, TTM_SETMAXTIPWIDTH, 0, Scale(window_, 260));
        }
        return tooltip;
    }

    void InitializeTooltips() noexcept {
        try {
            InitializeTooltipsUnchecked();
        } catch (...) {
            if (tooltipWindow_ != nullptr && ::IsWindow(tooltipWindow_) != FALSE) {
                ::DestroyWindow(tooltipWindow_);
            }
            tooltipWindow_ = nullptr;
            try {
                WriteDiagnostic(
                    L"编辑器 Tooltip 已降级：stage=Exception；"
                    L"【/】按钮与快捷定界功能继续可用。");
            } catch (...) {
                // Tooltip 诊断本身也不能中断编辑器控件初始化。
            }
        }
    }

    void InitializeTooltipsUnchecked() {
        ::SetLastError(ERROR_SUCCESS);
        tooltipWindow_ = CreateTooltipWindow();
        const DWORD creationError = ::GetLastError();
        if (tooltipWindow_ == nullptr) {
            const std::wstring creationDetail = creationError == ERROR_SUCCESS
                ? L"Tooltip 控件未提供 Win32 错误码"
                : win32::FormatLastError(creationError);
            WriteDiagnostic(std::format(
                L"编辑器 Tooltip 已降级：stage=CreateWindowExW，class={}，error={}：{}；"
                L"【/】按钮与快捷定界功能继续可用。",
                TOOLTIPS_CLASSW,
                creationError,
                creationDetail));
            return;
        }

        const bool startTooltipAdded = AddTooltip(trimStartButton_, L"设为起点");
        const bool endTooltipAdded = AddTooltip(trimEndButton_, L"设为终点");
        if (startTooltipAdded && endTooltipAdded) {
            return;
        }

        if (::IsWindow(tooltipWindow_) != FALSE) {
            ::DestroyWindow(tooltipWindow_);
        }
        tooltipWindow_ = nullptr;
        WriteDiagnostic(std::format(
            L"编辑器 Tooltip 已整体降级：startRegistered={}，endRegistered={}；"
            L"【/】按钮与快捷定界功能继续可用。",
            startTooltipAdded ? L"是" : L"否",
            endTooltipAdded ? L"是" : L"否"));
    }

    bool AddTooltip(const HWND control, const wchar_t* text) const {
        if (tooltipWindow_ == nullptr || control == nullptr || text == nullptr) {
            WriteDiagnostic(std::format(
                L"编辑器 Tooltip 注册已跳过：tooltipAvailable={}，controlAvailable={}，"
                L"textAvailable={}。",
                tooltipWindow_ != nullptr ? L"是" : L"否",
                control != nullptr ? L"是" : L"否",
                text != nullptr ? L"是" : L"否"));
            return false;
        }
        TOOLINFOW information{};
        information.uFlags = TTF_IDISHWND | TTF_SUBCLASS;
        information.hwnd = window_;
        information.uId = reinterpret_cast<UINT_PTR>(control);
        information.lpszText = const_cast<wchar_t*>(text);

        const auto registerTool = [this, &information](
                                      const UINT structureSize,
                                      DWORD* const lastError) {
            information.cbSize = structureSize;
            ::SetLastError(ERROR_SUCCESS);
            const LRESULT result = ::SendMessageW(
                tooltipWindow_,
                TTM_ADDTOOLW,
                0,
                reinterpret_cast<LPARAM>(&information));
            if (lastError != nullptr) {
                *lastError = ::GetLastError();
            }
            return result;
        };

        constexpr UINT primarySize = static_cast<UINT>(sizeof(TOOLINFOW));
        DWORD primaryError = ERROR_SUCCESS;
        const LRESULT primaryResult = registerTool(primarySize, &primaryError);
        if (primaryResult != FALSE) {
            return true;
        }

        constexpr UINT compatibilitySize =
            static_cast<UINT>(TTTOOLINFOW_V1_SIZE);
        DWORD compatibilityError = ERROR_SUCCESS;
        LRESULT compatibilityResult = FALSE;
        if constexpr (compatibilitySize != primarySize) {
            compatibilityResult = registerTool(compatibilitySize, &compatibilityError);
        }
        if (compatibilityResult != FALSE) {
            WriteDiagnostic(std::format(
                L"编辑器 Tooltip 使用兼容结构注册：control={}，primaryCbSize={}，"
                L"primaryResult={}，primaryError={}，compatibilityCbSize={}，"
                L"compatibilityResult={}，compatibilityError={}。",
                text,
                primarySize,
                static_cast<long long>(primaryResult),
                primaryError,
                compatibilitySize,
                static_cast<long long>(compatibilityResult),
                compatibilityError));
            return true;
        }

        const std::wstring primaryDetail = primaryError == ERROR_SUCCESS
            ? L"未提供 Win32 错误码"
            : win32::FormatLastError(primaryError);
        const std::wstring compatibilityDetail = compatibilityError == ERROR_SUCCESS
            ? L"未提供 Win32 错误码"
            : win32::FormatLastError(compatibilityError);
        WriteDiagnostic(std::format(
            L"编辑器 Tooltip 注册失败：control={}，primaryCbSize={}，primaryResult={}，"
            L"primaryError={}：{}，compatibilityCbSize={}，compatibilityResult={}，"
            L"compatibilityError={}：{}。",
            text,
            primarySize,
            static_cast<long long>(primaryResult),
            primaryError,
            primaryDetail,
            compatibilitySize,
            static_cast<long long>(compatibilityResult),
            compatibilityError,
            compatibilityDetail));
        return false;
    }

    static LRESULT CALLBACK FormatButtonSubclassProc(
        const HWND control,
        const UINT message,
        const WPARAM wParam,
        const LPARAM lParam,
        const UINT_PTR subclassId,
        const DWORD_PTR referenceData) {
        auto* self = reinterpret_cast<Impl*>(referenceData);
        if (self == nullptr) {
            return ::DefSubclassProc(control, message, wParam, lParam);
        }
        switch (message) {
        case WM_GETDLGCODE:
            return ::DefSubclassProc(control, message, wParam, lParam) | DLGC_WANTARROWS;
        case WM_KEYDOWN:
            if (wParam == VK_LEFT || wParam == VK_UP ||
                wParam == VK_RIGHT || wParam == VK_DOWN) {
                const bool selectMp4 = wParam == VK_LEFT || wParam == VK_UP;
                const HWND target = selectMp4 ? self->mp4Radio_ : self->gifRadio_;
                ::SetFocus(target);
                self->ChangeFormat(selectMp4 ? OutputFormat::Mp4 : OutputFormat::Gif);
                return 0;
            }
            if ((wParam == VK_SPACE || wParam == VK_RETURN) &&
                (static_cast<LPARAM>(lParam) & (1LL << 30)) == 0) {
                self->ChangeFormat(
                    control == self->gifRadio_ ? OutputFormat::Gif : OutputFormat::Mp4);
                return 0;
            }
            break;
        case WM_NCDESTROY:
            ::RemoveWindowSubclass(
                control, &Impl::FormatButtonSubclassProc, subclassId);
            break;
        default:
            break;
        }
        return ::DefSubclassProc(control, message, wParam, lParam);
    }

    void RecreateFonts() {
        chrome_.RecreateFonts(window_);
        SetControlFonts();
    }

    void SetControlFonts() const {
        SetControlFont(headerTitle_, chrome_.TitleFont());
        SetControlFont(headerSubtitle_, chrome_.CaptionFont());
        SetControlFont(rangeText_, chrome_.RegularFont());
        SetControlFont(outputSizeText_, chrome_.TimeFont());
        SetControlFont(timeline_.WindowHandle(), chrome_.CaptionFont());
        SetControlFont(trimStartButton_, chrome_.RegularFont());
        SetControlFont(trimEndButton_, chrome_.RegularFont());
        SetControlFont(playButton_, chrome_.RegularFont());
        SetControlFont(timeText_, chrome_.TimeFont());
        SetControlFont(formatLabel_, chrome_.RegularFont());
        SetControlFont(mp4Radio_, chrome_.RegularFont());
        SetControlFont(gifRadio_, chrome_.RegularFont());
        SetControlFont(saveButton_, chrome_.StrongFont());
        SetControlFont(copyButton_, chrome_.RegularFont());
        SetControlFont(statusText_, chrome_.CaptionFont());
    }

    void DeleteUiResources() noexcept {
        if (tooltipWindow_ != nullptr && ::IsWindow(tooltipWindow_) != FALSE) {
            ::DestroyWindow(tooltipWindow_);
        }
        tooltipWindow_ = nullptr;
        chrome_.Destroy();
    }

    void LayoutFromClient() {
        RECT client{};
        ::GetClientRect(window_, &client);
        LayoutControls(client.right, client.bottom);
    }

    void LayoutControls(const int width, const int height) {
        if (previewHost_ == nullptr || width <= 0 || height <= 0) {
            return;
        }
        const EditorChromeLayout layout =
            chrome_.CalculateLayout(window_, width, height, recording_);
        struct Placement final {
            HWND control{};
            const RECT* rectangle{};
        };
        const std::array placements{
            Placement{headerTitle_, &layout.headerTitle},
            Placement{headerSubtitle_, &layout.headerSubtitle},
            Placement{previewHost_, &layout.preview},
            Placement{rangeText_, &layout.rangeLabel},
            Placement{qualityControl_.WindowHandle(), &layout.qualityControl},
            Placement{outputSizeText_, &layout.outputSizeLabel},
            Placement{speedControl_.WindowHandle(), &layout.speedControl},
            Placement{trimStartButton_, &layout.trimStartButton},
            Placement{trimEndButton_, &layout.trimEndButton},
            Placement{timeline_.WindowHandle(), &layout.timeline},
            Placement{playButton_, &layout.playButton},
            Placement{timeText_, &layout.timeLabel},
            Placement{audioToggle_.WindowHandle(), &layout.audioToggle},
            Placement{formatLabel_, &layout.formatLabel},
            Placement{mp4Radio_, &layout.mp4Button},
            Placement{gifRadio_, &layout.gifButton},
            Placement{copyButton_, &layout.copyButton},
            Placement{saveButton_, &layout.saveButton},
            Placement{statusText_, &layout.statusLabel},
        };
        constexpr UINT placementFlags = SWP_NOACTIVATE | SWP_NOZORDER |
            SWP_NOOWNERZORDER | SWP_NOCOPYBITS | SWP_NOREDRAW;
        bool positioned = false;
        HDWP batch = ::BeginDeferWindowPos(static_cast<int>(placements.size()));
        if (batch != nullptr) {
            for (const Placement& placement : placements) {
                const RECT& rectangle = *placement.rectangle;
                batch = ::DeferWindowPos(
                    batch,
                    placement.control,
                    nullptr,
                    rectangle.left,
                    rectangle.top,
                    rectangle.right - rectangle.left,
                    rectangle.bottom - rectangle.top,
                    placementFlags);
                if (batch == nullptr) {
                    break;
                }
            }
            if (batch != nullptr) {
                positioned = ::EndDeferWindowPos(batch) != FALSE;
            }
        }
        if (!positioned) {
            for (const Placement& placement : placements) {
                const RECT& rectangle = *placement.rectangle;
                ::SetWindowPos(
                    placement.control,
                    nullptr,
                    rectangle.left,
                    rectangle.top,
                    rectangle.right - rectangle.left,
                    rectangle.bottom - rectangle.top,
                    placementFlags);
            }
        }
        editorPanelTop_ = layout.editorPanelTop;
        previewStage_ = layout.previewStage;
        statusDotCenter_ = layout.statusDotCenter;
        chrome_.ApplyPreviewRegion(previewHost_, layout);
        ::RedrawWindow(
            window_,
            nullptr,
            nullptr,
            RDW_INVALIDATE | RDW_ALLCHILDREN | RDW_NOERASE);
        // Coalesce resize bursts. EVR composition is refreshed after the current
        // input/layout batch instead of synchronously blocking every WM_SIZE.
        if (!previewVideoRefreshPosted_) {
            previewVideoRefreshPosted_ = ::PostMessageW(
                window_, kRefreshPreviewVideoMessage, 0, 0) != FALSE;
        }
    }

    void SetStatus(const std::wstring& text, const EditorStatusTone tone) {
        const bool textChanged = displayedStatusText_ != text;
        const bool toneChanged = statusTone_ != tone;
        if (!textChanged && !toneChanged) {
            return;
        }
        statusTone_ = tone;
        if (statusText_ != nullptr && textChanged) {
            displayedStatusText_ = text;
            ::SetWindowTextW(statusText_, text.c_str());
            ::InvalidateRect(statusText_, nullptr, FALSE);
        }
        if (toneChanged && window_ != nullptr && statusDotCenter_.y > 0) {
            const int radius = Scale(window_, 7);
            const RECT dotBounds{
                statusDotCenter_.x - radius,
                statusDotCenter_.y - radius,
                statusDotCenter_.x + radius + 1,
                statusDotCenter_.y + radius + 1};
            ::InvalidateRect(window_, &dotBounds, FALSE);
        }
    }

    void UpdateHeaderSubtitle() const {
        if (headerSubtitle_ == nullptr) {
            return;
        }
        const std::wstring subtitle = std::format(
            L"{} × {}  ·  {} FPS  ·  总时长 {}",
            recording_.width,
            recording_.height,
            recording_.framesPerSecond,
            FormatEditorTime(duration_));
        ::SetWindowTextW(headerSubtitle_, subtitle.c_str());
    }

    [[nodiscard]] bool DrawButton(const DRAWITEMSTRUCT* item) const {
        if (item == nullptr) {
            return false;
        }
        const int controlId = static_cast<int>(item->CtlID);
        EditorButtonPaintState state{};
        switch (controlId) {
        case kPlayButtonId:
            state.role = EditorButtonRole::Play;
            break;
        case kTrimStartButtonId:
            state.role = EditorButtonRole::TrimStart;
            break;
        case kTrimEndButtonId:
            state.role = EditorButtonRole::TrimEnd;
            break;
        case kMp4RadioId:
            state.role = EditorButtonRole::SegmentLeft;
            state.selected = selectedFormat_ == OutputFormat::Mp4;
            break;
        case kGifRadioId:
            state.role = EditorButtonRole::SegmentRight;
            state.selected = selectedFormat_ == OutputFormat::Gif;
            break;
        case kSaveButtonId:
            state.role = EditorButtonRole::Primary;
            break;
        case kCopyButtonId:
            state.role = EditorButtonRole::Secondary;
            break;
        default:
            return false;
        }
        state.playing = playing_;
        state.busy = busy_ &&
            ((controlId == kSaveButtonId && currentExportAction_ == ExportAction::Save) ||
             (controlId == kCopyButtonId && currentExportAction_ == ExportAction::Copy));
        return chrome_.DrawButton(window_, item, state);
    }

    void HandleCommand(const int id, const int notificationCode) {
        if (notificationCode != BN_CLICKED) {
            return;
        }
        const HWND control = ::GetDlgItem(window_, id);
        if (control != nullptr) {
            // Commands are deferred below; let the normal 16 ms motion cadence
            // coalesce press/release painting instead of blocking input on GDI.
            ::InvalidateRect(control, nullptr, FALSE);
        }
        if (::PostMessageW(
                window_,
                kDeferredEditorCommandMessage,
                static_cast<WPARAM>(id),
                0) == FALSE) {
            ExecuteCommand(id);
        }
    }

    void ExecuteCommand(const int id) {
        switch (id) {
        case kPlayButtonId:
            TogglePlayback();
            break;
        case kTrimStartButtonId:
            SetTrimBoundaryFromPlayhead(true);
            break;
        case kTrimEndButtonId:
            SetTrimBoundaryFromPlayhead(false);
            break;
        case kMp4RadioId:
            ChangeFormat(OutputFormat::Mp4);
            break;
        case kGifRadioId:
            ChangeFormat(OutputFormat::Gif);
            break;
        case kSaveButtonId:
            BeginSaveExport();
            break;
        case kCopyButtonId:
            BeginClipboardExport();
            break;
        default:
            break;
        }
    }

    void HandleNotification(const NMHDR* header) {
        if (header == nullptr || header->idFrom != kTimelineId) {
            return;
        }
        HandleTimelineNotification(
            *reinterpret_cast<const TimelineNotification*>(header));
    }

    void ScheduleTimelineInteractionDrain() {
        if (timelineInteractionTimerArmed_ || window_ == nullptr) {
            return;
        }
        timelineInteractionTimerArmed_ = ::SetTimer(
            window_,
            kTimelineInteractionTimer,
            kTimelineInteractionFrameMilliseconds,
            nullptr) != 0;
        if (!timelineInteractionTimerArmed_) {
            HandlePendingTimelineInteraction();
        }
    }

    void HandlePendingTimelineInteraction() {
        TimelineNotification notification{};
        if (timeline_.ConsumePendingNotification(&notification)) {
            HandleTimelineNotification(notification);
        }
    }

    [[nodiscard]] std::chrono::milliseconds MinimumTrimSpan() const noexcept {
        return std::min(duration_, std::chrono::milliseconds(100));
    }

    [[nodiscard]] std::chrono::milliseconds QuantizedPlayhead() const noexcept {
        const int framesPerSecond = std::max(1, recording_.framesPerSecond);
        const auto playhead = std::clamp(
            timeline_.Playhead(),
            std::chrono::milliseconds::zero(),
            duration_);
        const long double frame = std::round(
            static_cast<long double>(playhead.count()) * framesPerSecond / 1000.0L);
        const auto quantizedMilliseconds = static_cast<std::int64_t>(std::llround(
            frame * 1000.0L / static_cast<long double>(framesPerSecond)));
        return std::clamp(
            std::chrono::milliseconds(quantizedMilliseconds),
            std::chrono::milliseconds::zero(),
            duration_);
    }

    [[nodiscard]] bool CanSetTrimStartFromPlayhead() const noexcept {
        return QuantizedPlayhead() <= trimEnd_ - MinimumTrimSpan();
    }

    [[nodiscard]] bool CanSetTrimEndFromPlayhead() const noexcept {
        return QuantizedPlayhead() >= trimStart_ + MinimumTrimSpan();
    }

    void UpdateTrimBoundaryButtonStates() {
        const bool startEnabled = !busy_ && CanSetTrimStartFromPlayhead();
        const bool endEnabled = !busy_ && CanSetTrimEndFromPlayhead();
        if (trimStartButton_ != nullptr &&
            (::IsWindowEnabled(trimStartButton_) != FALSE) != startEnabled) {
            ::EnableWindow(trimStartButton_, startEnabled ? TRUE : FALSE);
        }
        if (trimEndButton_ != nullptr &&
            (::IsWindowEnabled(trimEndButton_) != FALSE) != endEnabled) {
            ::EnableWindow(trimEndButton_, endEnabled ? TRUE : FALSE);
        }
    }

    void SetTrimBoundaryFromPlayhead(const bool setStart) {
        if (busy_) {
            return;
        }
        const auto position = QuantizedPlayhead();
        const bool valid = setStart
            ? CanSetTrimStartFromPlayhead()
            : CanSetTrimEndFromPlayhead();
        if (!valid) {
            UpdateTrimBoundaryButtonStates();
            return;
        }
        const bool changed = setStart
            ? timeline_.CommitTrimStart(position)
            : timeline_.CommitTrimEnd(position);
        UpdateTrimBoundaryButtonStates();
        if (changed) {
            WriteDiagnostic(std::format(
                L"编辑器快速定界：boundary={}，playhead={} ms，fps={}，"
                L"quantized={} ms",
                setStart ? L"起点" : L"终点",
                timeline_.Playhead().count(),
                std::max(1, recording_.framesPerSecond),
                position.count()));
        }
    }

    void ApplyPreviewSpeed() {
        std::wstring error;
        if (!mediaItemReady_) {
            static_cast<void>(preview_.SetPlaybackSpeedTenths(
                playbackSpeedTenths_,
                nullptr));
            return;
        }
        if (!preview_.SetPlaybackSpeedTenths(playbackSpeedTenths_, &error)) {
            WriteDiagnostic(std::format(
                L"编辑器预览倍速设置失败：speed={:.1f}x，error={}",
                playbackSpeedTenths_ / 10.0,
                error));
            if (!busy_) {
                SetStatus(
                    error.empty() ? L"当前视频无法按所选倍速预览。" : error,
                    EditorStatusTone::Error);
            }
        }
    }

    void HandleSpeedChanged(
        const int speedTenths,
        const EditorSliderInteractionPhase phase) {
        if (busy_) {
            speedControl_.SetValueTenths(playbackSpeedTenths_);
            return;
        }
        const int clamped = std::clamp(
            speedTenths,
            EditorSpeedControl::MinimumSpeedTenths,
            EditorSpeedControl::MaximumSpeedTenths);
        const bool valueChanged = playbackSpeedTenths_ != clamped;
        playbackSpeedTenths_ = clamped;
        speedControl_.SetValueTenths(playbackSpeedTenths_);
        if (valueChanged) {
            lastRangeLabelSpeedTenths_ = -1;
            UpdateTimeLabels(timeline_.Playhead());
            UpdateOutputSizeEstimate();
            ApplyPreviewSpeed();
            if (playing_) {
                // The playback-end guard is polled only while playing. Re-arm
                // it immediately so a faster rate cannot keep the previous
                // 16 ms cadence until the next play/pause transition.
                StopPlaybackUiTimer();
                StartPlaybackUiTimer();
            }
        }

        if (phase == EditorSliderInteractionPhase::Preview) {
            if (!speedInteractionActive_) {
                speedInteractionActive_ = true;
                InvalidateWarmCacheWork();
            }
            UpdateReadyStatus();
            return;
        }

        speedInteractionActive_ = false;
        WriteDiagnostic(std::format(
            L"编辑器倍速提交：speed={:.1f}x，trimStart={} ms，trimEnd={} ms，"
            L"format={}，includeSystemAudio={}",
            playbackSpeedTenths_ / 10.0,
            trimStart_.count(),
            trimEnd_.count(),
            OutputFormatName(selectedFormat_),
            ShouldIncludeSystemAudio() ? L"是" : L"否"));
        ScheduleWarmCache(kWarmCacheSpeedDebounceMilliseconds);
    }

    static std::wstring FormatOutputSize(
        const std::uint64_t bytes,
        const bool exact) {
        if (bytes == 0) {
            return L"计算中";
        }
        constexpr long double kBytesPerMegabyte = 1024.0L * 1024.0L;
        const long double megabytes = static_cast<long double>(bytes) /
            kBytesPerMegabyte;
        if (megabytes < 0.01L) {
            return exact ? L"<0.01 MB" : L"~0.01 MB";
        }
        if (megabytes < 1.0L) {
            return exact
                ? std::format(L"{:.2f} MB", static_cast<double>(megabytes))
                : std::format(L"~{:.2f} MB", static_cast<double>(megabytes));
        }
        return exact
            ? std::format(L"{:.1f} MB", static_cast<double>(megabytes))
            : std::format(L"~{:.1f} MB", static_cast<double>(megabytes));
    }

    void SetDisplayedOutputSize(
        const std::uint64_t bytes,
        const bool exact) {
        const std::wstring text = FormatOutputSize(bytes, exact);
        const bool toneChanged = outputSizeExact_ != exact;
        outputSizeExact_ = exact;
        if (exact) {
            exactOutputBytes_ = bytes;
        } else {
            exactOutputBytes_ = 0;
        }
        if (displayedOutputSizeText_ == text && !toneChanged) {
            return;
        }
        displayedOutputSizeText_ = text;
        if (outputSizeText_ != nullptr) {
            ::SetWindowTextW(outputSizeText_, text.c_str());
            ::InvalidateRect(outputSizeText_, nullptr, FALSE);
        }
    }

    void UpdateOutputSizeEstimate() {
        const MediaExportEstimate estimate = MediaExporter::EstimateOutput(
            BuildExportRequest({}));
        estimatedOutputBytes_ = estimate.outputBytes;
        SetDisplayedOutputSize(estimate.outputBytes, estimate.exact);
    }

    void HandleQualityChanged(
        const int qualityPercent,
        const EditorSliderInteractionPhase phase) {
        if (busy_) {
            qualityControl_.SetValue(qualityPercent_);
            return;
        }
        const int normalized = media::ExportQuality::Normalize(qualityPercent);
        const bool valueChanged = qualityPercent_ != normalized;
        qualityPercent_ = normalized;
        qualityControl_.SetValue(qualityPercent_);
        if (valueChanged) {
            UpdateOutputSizeEstimate();
        }

        if (phase == EditorSliderInteractionPhase::Preview) {
            if (!qualityInteractionActive_) {
                qualityInteractionActive_ = true;
                InvalidateWarmCacheWork();
                InvalidatePreviewProxyWork();
            }
            UpdateReadyStatus();
            return;
        }

        qualityInteractionActive_ = false;
        settings_.outputQualityPercent = qualityPercent_;
        if (callbacks_.settingsChanged) {
            callbacks_.settingsChanged(settings_);
        }
        const MediaExportEstimate estimate = MediaExporter::EstimateOutput(
            BuildExportRequest({}));
        WriteDiagnostic(std::format(
            L"编辑器画质提交：quality={}%，target={}×{}，estimatedBytes={}，"
            L"format={}，trimStart={} ms，trimEnd={} ms，speed={:.1f}x",
            qualityPercent_,
            estimate.outputWidth,
            estimate.outputHeight,
            estimate.outputBytes,
            OutputFormatName(selectedFormat_),
            trimStart_.count(),
            trimEnd_.count(),
            playbackSpeedTenths_ / 10.0));
        ScheduleWarmCache(kWarmCacheQualityDebounceMilliseconds);
        SchedulePreviewProxy(kPreviewProxyDebounceMilliseconds);
    }

    void HandleTimelineNotification(const TimelineNotification& notification) {
        const bool committed =
            notification.phase == TimelineInteractionPhase::Committed;
        if (notification.header.code == TimelineRangeChanged) {
            trimStart_ = notification.trimStart;
            trimEnd_ = notification.trimEnd;
            UpdateTrimBoundaryButtonStates();
            bool invalidatedPreparedArtifact = false;
            if (!committed && !timelineRangeInteractionActive_) {
                invalidatedPreparedArtifact = warmCacheReady_ || warmCacheInProgress_;
                timelineRangeInteractionActive_ = true;
                InvalidateWarmCacheWork();
            }
            const bool wasEdited = trimRangeEdited_;
            RefreshTrimRangeEditedState();
            UpdateOutputSizeEstimate();
            QueueTimelineSeek(notification.seekPosition, committed);
            UpdateTimelineLabels(notification.seekPosition, committed);
            if (wasEdited != trimRangeEdited_) {
                WriteDiagnostic(std::format(
                    L"编辑器裁切状态变化：trimStart={} ms，trimEnd={} ms，duration={} ms，"
                    L"trimRangeEdited={}，format={}",
                    trimStart_.count(),
                    trimEnd_.count(),
                    duration_.count(),
                    trimRangeEdited_ ? L"是" : L"否",
                    OutputFormatName(selectedFormat_)));
                UpdateReadyStatus();
            } else if (invalidatedPreparedArtifact) {
                UpdateReadyStatus();
            }
            if (committed) {
                timelineRangeInteractionActive_ = false;
                ScheduleWarmCache();
                ++timelineCommittedNotificationCount_;
            } else {
                ++timelinePreviewNotificationCount_;
            }
        } else if (notification.header.code == TimelineSeekRequested) {
            const auto position = std::clamp(
                notification.seekPosition,
                std::chrono::milliseconds::zero(),
                duration_);
            timeline_.SetPlayhead(position);
            UpdateTrimBoundaryButtonStates();
            UpdateTimelineLabels(position, committed);
            QueueTimelineSeek(position, committed);
            if (committed) {
                ++timelineCommittedNotificationCount_;
            } else {
                ++timelinePreviewNotificationCount_;
            }
        }
    }

    void QueueTimelineSeek(
        const std::chrono::milliseconds position,
        const bool finalCommit = false) {
        const auto clamped = std::clamp(
            position,
            std::chrono::milliseconds::zero(),
            duration_);
        awaitingNaturalPlaybackEnd_ = false;
        ++previewSeekRequestCount_;
        if (pendingTimelineSeek_.has_value() || previewSeekInFlight_) {
            ++previewSeekCoalescedCount_;
        }
        pendingTimelineSeek_ = clamped;
        pendingTimelineSeekFinal_ = pendingTimelineSeekFinal_ || finalCommit;
        if (playing_) {
            playing_ = false;
            pendingTimelinePause_ = true;
            StopPlaybackUiTimer();
            UpdatePlayButton();
        }
        if (previewSeekInFlight_) {
            return;
        }
        ArmTimelineSeekTimer(finalCommit ? 1U : TimelineSeekDelayMilliseconds());
    }

    [[nodiscard]] UINT TimelineSeekDelayMilliseconds() const noexcept {
        if (lastPreviewSeekIssuedAt_ == std::chrono::steady_clock::time_point{}) {
            return 1;
        }
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - lastPreviewSeekIssuedAt_);
        if (elapsed >= kTimelineSeekPreviewInterval) {
            return 1;
        }
        return static_cast<UINT>(std::max<std::int64_t>(
            1,
            (kTimelineSeekPreviewInterval - elapsed).count()));
    }

    void ArmTimelineSeekTimer(const UINT delayMilliseconds) {
        if (timelineSeekTimerArmed_ || window_ == nullptr) {
            return;
        }
        timelineSeekTimerArmed_ = ::SetTimer(
            window_,
            kTimelineSeekTimer,
            std::max<UINT>(1, delayMilliseconds),
            nullptr) != 0;
        if (!timelineSeekTimerArmed_) {
            PumpPendingTimelineSeek();
        }
    }

    void PumpPendingTimelineSeek() {
        if (timelineSeekTimerArmed_) {
            ::KillTimer(window_, kTimelineSeekTimer);
            timelineSeekTimerArmed_ = false;
        }
        if (previewSeekInFlight_ || !pendingTimelineSeek_.has_value()) {
            return;
        }
        if (!pendingTimelineSeekFinal_) {
            const UINT remaining = TimelineSeekDelayMilliseconds();
            if (remaining > 1) {
                ArmTimelineSeekTimer(remaining);
                return;
            }
        }
        const auto position = *pendingTimelineSeek_;
        pendingTimelineSeek_.reset();
        pendingTimelineSeekFinal_ = false;
        if (pendingTimelinePause_) {
            static_cast<void>(preview_.Pause());
            pendingTimelinePause_ = false;
        }
        const auto startedAt = std::chrono::steady_clock::now();
        const bool submitted = preview_.Seek(position);
        const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - startedAt);
        RecordPreviewSeekDuration(elapsed);
        lastPreviewSeekIssuedAt_ = std::chrono::steady_clock::now();
        if (submitted) {
            previewSeekInFlight_ = true;
            inFlightTimelineSeek_ = position;
            ++previewSeekIssuedCount_;
        }
    }

    void TogglePlayback() {
        if (exporter_.IsRunning()) {
            return;
        }
        if (playing_) {
            playAfterTimelineSeek_ = false;
            awaitingNaturalPlaybackEnd_ = false;
            std::wstring error;
            if (!preview_.Pause(&error)) {
                SynchronizePlaybackUiWithPreview();
                awaitingNaturalPlaybackEnd_ = playing_;
                SetStatus(error, EditorStatusTone::Error);
                return;
            }
            playing_ = false;
            StopPlaybackUiTimer();
        } else {
            if (pendingTimelineSeek_.has_value() || previewSeekInFlight_) {
                const auto pendingPosition = pendingTimelineSeek_.value_or(
                    inFlightTimelineSeek_.value_or(preview_.Position()));
                if (pendingPosition < trimStart_ || pendingPosition >= trimEnd_) {
                    QueueTimelineSeek(trimStart_, true);
                }
                pendingTimelineSeekFinal_ = true;
                playAfterTimelineSeek_ = true;
                playing_ = true;
                if (!previewSeekInFlight_) {
                    ArmTimelineSeekTimer(1);
                }
                UpdatePlayButton();
                return;
            }
            auto position = preview_.Position();
            if (position < trimStart_ || position >= trimEnd_) {
                QueueTimelineSeek(trimStart_, true);
                playAfterTimelineSeek_ = true;
                playing_ = true;
                UpdatePlayButton();
                return;
            }
            std::wstring error;
            if (!preview_.Play(&error)) {
                awaitingNaturalPlaybackEnd_ = false;
                SetStatus(error, EditorStatusTone::Error);
                return;
            }
            playing_ = true;
            awaitingNaturalPlaybackEnd_ = true;
            StartPlaybackUiTimer();
        }
        UpdatePlayButton();
    }

    void StartPlaybackUiTimer() {
        lastPlaybackTextRefresh_ = {};
        if (playbackTimerArmed_ || window_ == nullptr) {
            return;
        }
        playbackTimerArmed_ = ::SetTimer(
            window_,
            kPlaybackTimer,
            PlaybackRefreshIntervalMilliseconds(),
            nullptr) != 0;
    }

    [[nodiscard]] UINT PlaybackRefreshIntervalMilliseconds() const noexcept {
        const auto framesPerSecond = static_cast<std::uint64_t>(
            std::max(1, recording_.framesPerSecond));
        const auto speedTenths = static_cast<std::uint64_t>(
            std::clamp(
                playbackSpeedTenths_,
                EditorSpeedControl::MinimumSpeedTenths,
                EditorSpeedControl::MaximumSpeedTenths));
        // One UI poll should advance by no more than roughly one source frame.
        // Slower rates retain the existing 16 ms ceiling for smooth UI updates.
        const auto frameBoundedInterval = std::max<std::uint64_t>(
            1,
            10'000ULL / (framesPerSecond * speedTenths));
        return static_cast<UINT>(std::min<std::uint64_t>(
            kPlaybackRefreshMilliseconds,
            frameBoundedInterval));
    }

    [[nodiscard]] std::chrono::milliseconds PlaybackEndGuard() const noexcept {
        if (playbackSpeedTenths_ <= EditorSpeedControl::DefaultSpeedTenths) {
            return std::chrono::milliseconds::zero();
        }
        const long double framesPerSecond = static_cast<long double>(
            std::max(1, recording_.framesPerSecond));
        const long double sourceFrameMilliseconds = 1000.0L / framesPerSecond;
        // SetTimer clamps sub-USER_TIMER_MINIMUM requests. Compensate only for
        // the source-time advance beyond one frame, avoiding a full-frame early
        // stop at rates whose requested cadence is already sufficient.
        const UINT effectiveInterval = std::max<UINT>(
            PlaybackRefreshIntervalMilliseconds(),
            USER_TIMER_MINIMUM);
        const long double sourceAdvanceMilliseconds =
            static_cast<long double>(effectiveInterval) *
            static_cast<long double>(playbackSpeedTenths_) / 10.0L;
        const long double guardMilliseconds = std::max<long double>(
            0.0L,
            sourceAdvanceMilliseconds - sourceFrameMilliseconds);
        return std::chrono::milliseconds(static_cast<std::int64_t>(
            std::ceil(guardMilliseconds)));
    }

    void StopPlaybackUiTimer() noexcept {
        if (playbackTimerArmed_ && window_ != nullptr) {
            ::KillTimer(window_, kPlaybackTimer);
        }
        playbackTimerArmed_ = false;
        lastPlaybackTextRefresh_ = {};
    }

    void UpdatePlayButton() {
        if (playButton_ != nullptr) {
            const std::wstring label = playing_ ? L"暂停" : L"播放";
            if (displayedPlayButtonText_ != label) {
                displayedPlayButtonText_ = label;
                ::SetWindowTextW(playButton_, label.c_str());
                ::InvalidateRect(playButton_, nullptr, FALSE);
            }
        }
    }

    void SynchronizePlaybackUiWithPreview() {
        playing_ = preview_.IsPlaying();
        if (playing_) {
            StartPlaybackUiTimer();
        } else {
            StopPlaybackUiTimer();
        }
        UpdatePlayButton();
    }

    void UpdatePlaybackPosition() {
        if (pendingTimelineSeek_.has_value() || timelineSeekTimerArmed_ ||
            previewSeekInFlight_) {
            return;
        }
        if (!playing_) {
            StopPlaybackUiTimer();
            return;
        }
        const auto position = preview_.Position();
        const auto endGuard = PlaybackEndGuard();
        const auto guardedEnd = std::max(trimStart_, trimEnd_ - endGuard);
        if (position >= guardedEnd) {
            awaitingNaturalPlaybackEnd_ = false;
            static_cast<void>(preview_.Pause());
            playing_ = false;
            StopPlaybackUiTimer();
            timeline_.SetPlayhead(trimStart_);
            UpdateTrimBoundaryButtonStates();
            UpdateTimeLabels(trimStart_);
            UpdatePlayButton();
            QueueTimelineSeek(trimStart_, true);
            return;
        }
        timeline_.SetPlayhead(position);
        UpdateTrimBoundaryButtonStates();
        const auto now = std::chrono::steady_clock::now();
        if (lastPlaybackTextRefresh_ == std::chrono::steady_clock::time_point{} ||
            now - lastPlaybackTextRefresh_ >= kPlaybackTextRefreshInterval) {
            lastPlaybackTextRefresh_ = now;
            UpdateTimeLabels(position);
        }
    }

    void UpdateTimeLabels(const std::chrono::milliseconds position) {
        const auto positionBucket = std::chrono::milliseconds(
            std::max<std::int64_t>(0, position.count()) / 10 * 10);
        if (positionBucket != lastTimeLabelPositionBucket_ ||
            duration_ != lastTimeLabelDuration_) {
            lastTimeLabelPositionBucket_ = positionBucket;
            lastTimeLabelDuration_ = duration_;
            const std::wstring timeText =
                FormatEditorTime(positionBucket) + L" / " + FormatEditorTime(duration_);
            if (displayedTimeText_ != timeText) {
                displayedTimeText_ = timeText;
                ::SetWindowTextW(timeText_, timeText.c_str());
            }
        }
        if (trimStart_ != lastRangeLabelStart_ || trimEnd_ != lastRangeLabelEnd_ ||
            playbackSpeedTenths_ != lastRangeLabelSpeedTenths_) {
            lastRangeLabelStart_ = trimStart_;
            lastRangeLabelEnd_ = trimEnd_;
            lastRangeLabelSpeedTenths_ = playbackSpeedTenths_;
            const auto retainedDuration = trimEnd_ - trimStart_;
            const auto outputDuration = std::chrono::milliseconds(
                static_cast<std::int64_t>(std::llround(
                    static_cast<long double>(retainedDuration.count()) * 10.0L /
                    static_cast<long double>(std::max(1, playbackSpeedTenths_)))));
            const std::wstring rangeText = std::format(
                L"保留  {} — {}  ·  输出 {}",
                FormatEditorTime(trimStart_),
                FormatEditorTime(trimEnd_),
                FormatEditorTime(outputDuration));
            if (displayedRangeText_ != rangeText) {
                displayedRangeText_ = rangeText;
                ::SetWindowTextW(rangeText_, rangeText.c_str());
            }
        }
    }

    void UpdateTimelineLabels(
        const std::chrono::milliseconds position,
        const bool force) {
        const auto now = std::chrono::steady_clock::now();
        if (!force &&
            lastTimelineLabelRefresh_ != std::chrono::steady_clock::time_point{} &&
            now - lastTimelineLabelRefresh_ < kPlaybackTextRefreshInterval) {
            return;
        }
        lastTimelineLabelRefresh_ = now;
        UpdateTimeLabels(position);
    }

    void RecordPreviewSeekDuration(const std::chrono::microseconds elapsed) noexcept {
        const std::size_t slot = previewSeekDurationSampleCount_ %
            previewSeekDurationSamples_.size();
        previewSeekDurationSamples_[slot] = elapsed.count();
        ++previewSeekDurationSampleCount_;
    }

    void WriteInteractionMetrics() const {
        const std::size_t sampleCount = std::min(
            previewSeekDurationSampleCount_,
            previewSeekDurationSamples_.size());
        std::array<std::int64_t, 256> sortedSamples{};
        std::copy_n(
            previewSeekDurationSamples_.begin(),
            sampleCount,
            sortedSamples.begin());
        std::sort(sortedSamples.begin(), sortedSamples.begin() + sampleCount);
        const std::int64_t p95Microseconds = sampleCount == 0
            ? 0
            : sortedSamples[std::min(
                  sampleCount - 1,
                  (sampleCount * 95 + 99) / 100 - 1)];
        const std::int64_t maximumMicroseconds = sampleCount == 0
            ? 0
            : sortedSamples[sampleCount - 1];
        WriteDiagnostic(std::format(
            L"编辑器交互性能：timelinePreview={}，timelineCommit={}，"
            L"seekRequested={}，seekIssued={}，seekCoalesced={}，"
            L"setPositionSamples={}，setPositionP95={} us，setPositionMax={} us",
            timelinePreviewNotificationCount_,
            timelineCommittedNotificationCount_,
            previewSeekRequestCount_,
            previewSeekIssuedCount_,
            previewSeekCoalescedCount_,
            sampleCount,
            p95Microseconds,
            maximumMicroseconds));
    }

    void RefreshTrimRangeEditedState() noexcept {
        trimRangeEdited_ =
            trimStart_ != std::chrono::milliseconds::zero() || trimEnd_ != duration_;
    }

    void UpdateReadyStatus() {
        if (busy_) {
            return;
        }
        if (NeedsWarmCache() && warmCacheReady_) {
            SetStatus(
                ShouldIncludeSystemAudio()
                    ? L"有声 MP4 已准备完成，复制或保存将立即完成"
                    : L"已准备完成，复制或保存将立即完成",
                EditorStatusTone::Success);
            return;
        }
        if (NeedsWarmCache() && warmCacheInProgress_) {
            SetStatus(
                qualityPercent_ < media::ExportQuality::DefaultPercent
                    ? std::format(
                        L"正在后台准备 {}% 画质成片，可继续预览",
                        qualityPercent_)
                : playbackSpeedTenths_ != EditorSpeedControl::DefaultSpeedTenths
                    ? L"正在后台准备倍速成片，可继续预览"
                    : (ShouldIncludeSystemAudio()
                    ? L"正在后台快速合并电脑声音，可继续编辑"
                    : L"正在后台准备，可继续预览"),
                EditorStatusTone::Progress);
            return;
        }
        if (selectedFormat_ == OutputFormat::Gif) {
            SetStatus(
                L"GIF 需要重新生成；耗时取决于片段长度",
                EditorStatusTone::Neutral);
        } else if (qualityPercent_ < media::ExportQuality::DefaultPercent) {
            SetStatus(
                std::format(
                    L"{}% 画质成片将在后台准备；可继续预览和裁剪",
                    qualityPercent_),
                EditorStatusTone::Neutral);
        } else if (playbackSpeedTenths_ != EditorSpeedControl::DefaultSpeedTenths) {
            SetStatus(
                L"倍速成片将在后台准备；可继续预览和裁剪",
                EditorStatusTone::Neutral);
        } else if (ShouldIncludeSystemAudio()) {
            SetStatus(
                L"将使用已编码音视频快速生成有声 MP4",
                EditorStatusTone::Neutral);
        } else if (trimRangeEdited_) {
            SetStatus(
                L"精确裁切 MP4 正在后台准备；原始录屏不会被覆盖",
                EditorStatusTone::Neutral);
        } else {
            SetStatus(
                L"原始 MP4 已就绪；复制或保存无需重新编码",
                EditorStatusTone::Success);
        }
    }

    void HandlePreviewEvent(const MFP_EVENT_TYPE type, const HRESULT status) {
        if (FAILED(status)) {
            awaitingNaturalPlaybackEnd_ = false;
            if (type == MFP_EVENT_TYPE_POSITION_SET) {
                CompleteTimelineSeek(status);
            }
            SynchronizePlaybackUiWithPreview();
            SetStatus(L"预览失败：" + win32::FormatError(status), EditorStatusTone::Error);
            return;
        }
        switch (type) {
        case MFP_EVENT_TYPE_MEDIAITEM_SET: {
            mediaItemReady_ = true;
            if (previewReloadState_.has_value()) {
                const PreviewReloadState reload = *previewReloadState_;
                previewReloadState_.reset();
                ApplyPreviewSpeed();
                preview_.UpdateVideo();
                timeline_.SetPlayhead(reload.position);
                UpdateTrimBoundaryButtonStates();
                UpdateTimeLabels(reload.position);
                QueueTimelineSeek(reload.position, true);
                playAfterTimelineSeek_ = reload.resumePlayback;
                UpdatePlayButton();
                UpdateReadyStatus();
                break;
            }
            const auto detectedDuration = preview_.Duration();
            if (detectedDuration.count() > 0) {
                duration_ = detectedDuration;
                trimStart_ = std::chrono::milliseconds(0);
                trimEnd_ = duration_;
                trimRangeEdited_ = false;
                timeline_.SetRange(duration_, trimStart_, trimEnd_);
                timeline_.SetPlayhead(std::chrono::milliseconds::zero());
                UpdateTrimBoundaryButtonStates();
                UpdateTimeLabels(std::chrono::milliseconds(0));
                UpdateHeaderSubtitle();
                WriteDiagnostic(std::format(
                    L"编辑器媒体时长已探测：trimStart=0 ms，trimEnd={} ms，duration={} ms，"
                    L"recordingDuration={} ms，trimRangeEdited=否，format={}",
                    trimEnd_.count(),
                    duration_.count(),
                    recording_.duration.count(),
                    OutputFormatName(selectedFormat_)));
                ScheduleWarmCache();
            }
            ApplyPreviewSpeed();
            preview_.UpdateVideo();
            std::wstring playError;
            if (preview_.Play(&playError)) {
                playing_ = true;
                awaitingNaturalPlaybackEnd_ = true;
                StartPlaybackUiTimer();
                UpdatePlayButton();
                UpdateReadyStatus();
            } else {
                awaitingNaturalPlaybackEnd_ = false;
                SetStatus(L"首帧已就绪；" + playError, EditorStatusTone::Error);
            }
            break;
        }
        case MFP_EVENT_TYPE_PLAY:
        case MFP_EVENT_TYPE_PAUSE:
        case MFP_EVENT_TYPE_STOP:
            // MFPlay callbacks are free-threaded and may arrive after a newer command.
            // The current player state is authoritative; the event type can be stale.
            SynchronizePlaybackUiWithPreview();
            break;
        case MFP_EVENT_TYPE_PLAYBACK_ENDED:
            if (!awaitingNaturalPlaybackEnd_ || preview_.IsPlaying()) {
                // A delayed ENDED from the previous playback must not rewind a new play.
                SynchronizePlaybackUiWithPreview();
                break;
            }
            awaitingNaturalPlaybackEnd_ = false;
            playing_ = false;
            StopPlaybackUiTimer();
            timeline_.SetPlayhead(trimStart_);
            UpdateTrimBoundaryButtonStates();
            UpdateTimeLabels(trimStart_);
            UpdatePlayButton();
            QueueTimelineSeek(trimStart_, true);
            break;
        case MFP_EVENT_TYPE_POSITION_SET:
            CompleteTimelineSeek(status);
            break;
        default:
            break;
        }
    }

    void CompleteTimelineSeek(const HRESULT status) {
        if (!previewSeekInFlight_) {
            return;
        }
        previewSeekInFlight_ = false;
        inFlightTimelineSeek_.reset();
        if (FAILED(status)) {
            playAfterTimelineSeek_ = false;
            pendingTimelineSeek_.reset();
            pendingTimelineSeekFinal_ = false;
            return;
        }
        if (pendingTimelineSeek_.has_value()) {
            ArmTimelineSeekTimer(
                pendingTimelineSeekFinal_ ? 1U : TimelineSeekDelayMilliseconds());
            return;
        }
        if (!playAfterTimelineSeek_) {
            return;
        }
        playAfterTimelineSeek_ = false;
        std::wstring error;
        if (!preview_.Play(&error)) {
            playing_ = false;
            awaitingNaturalPlaybackEnd_ = false;
            StopPlaybackUiTimer();
            UpdatePlayButton();
            SetStatus(error, EditorStatusTone::Error);
            return;
        }
        playing_ = true;
        awaitingNaturalPlaybackEnd_ = true;
        StartPlaybackUiTimer();
        UpdatePlayButton();
    }

    [[nodiscard]] bool ShouldIncludeSystemAudio() const noexcept {
        return selectedFormat_ == OutputFormat::Mp4 &&
            audioRequestedForMp4_ &&
            recording_.systemAudio.available &&
            !recording_.systemAudio.sourcePath.empty();
    }

    void RefreshAudioToggleState() {
        if (audioToggle_.WindowHandle() == nullptr) {
            return;
        }
        if (!recording_.systemAudio.available ||
            recording_.systemAudio.sourcePath.empty()) {
            audioRequestedForMp4_ = false;
            audioToggle_.SetChecked(false);
            audioToggle_.SetEnabled(false);
            audioToggle_.SetStatusText(L"未捕获到电脑声音");
            return;
        }
        if (selectedFormat_ == OutputFormat::Gif) {
            audioToggle_.SetChecked(false);
            audioToggle_.SetEnabled(false);
            audioToggle_.SetStatusText(L"GIF 不支持声音");
            return;
        }

        audioToggle_.SetChecked(audioRequestedForMp4_);
        audioToggle_.SetEnabled(!busy_);
        audioToggle_.SetStatusText(
            audioRequestedForMp4_
                ? L"开启 · 输出将包含电脑声音"
                : L"关闭 · 输出不包含电脑声音");
    }

    void HandleAudioToggleChanged(const bool checked) {
        if (busy_ || selectedFormat_ != OutputFormat::Mp4 ||
            !recording_.systemAudio.available) {
            RefreshAudioToggleState();
            return;
        }
        audioRequestedForMp4_ = checked;
        WriteDiagnostic(std::format(
            L"编辑器电脑声音切换：includeSystemAudio={}，trimStart={} ms，"
            L"trimEnd={} ms，format=MP4",
            audioRequestedForMp4_ ? L"是" : L"否",
            trimStart_.count(),
            trimEnd_.count()));
        RefreshAudioToggleState();
        UpdateOutputSizeEstimate();
        UpdateReadyStatus();
        ScheduleWarmCache();
    }

    void ChangeFormat(const OutputFormat format) {
        if (exporter_.IsRunning() || selectedFormat_ == format) {
            return;
        }
        selectedFormat_ = format;
        RefreshAudioToggleState();
        WriteDiagnostic(std::format(
            L"编辑器格式切换：format={}，trimStart={} ms，trimEnd={} ms，duration={} ms，"
            L"trimRangeEdited={}，includeSystemAudio={}（仅当前窗口）",
            OutputFormatName(selectedFormat_),
            trimStart_.count(),
            trimEnd_.count(),
            duration_.count(),
            trimRangeEdited_ ? L"是" : L"否",
            ShouldIncludeSystemAudio() ? L"是" : L"否"));
        ::InvalidateRect(mp4Radio_, nullptr, FALSE);
        ::InvalidateRect(gifRadio_, nullptr, FALSE);
        UpdateOutputSizeEstimate();
        UpdateReadyStatus();
        ScheduleWarmCache();
    }

    [[nodiscard]] ExportRequest BuildExportRequest(
        const std::filesystem::path& destination) const {
        ExportRequest request{};
        request.recording = recording_;
        request.trimStart = trimRangeEdited_
            ? trimStart_
            : std::chrono::milliseconds::zero();
        request.trimEnd = trimRangeEdited_ ? trimEnd_ : recording_.duration;
        request.format = selectedFormat_;
        request.includeSystemAudio = ShouldIncludeSystemAudio();
        request.playbackSpeedTenths = playbackSpeedTenths_;
        request.qualityPercent = qualityPercent_;
        request.destinationPath = destination;
        return request;
    }

    [[nodiscard]] bool NeedsWarmCache() const noexcept {
        return selectedFormat_ == OutputFormat::Gif || trimRangeEdited_ ||
            ShouldIncludeSystemAudio() ||
            playbackSpeedTenths_ != EditorSpeedControl::DefaultSpeedTenths ||
            qualityPercent_ < media::ExportQuality::DefaultPercent;
    }

    void ClearPendingWarmCacheMessages() {
        std::scoped_lock lock(warmCachePendingMutex_);
        pendingWarmCacheProgress_.reset();
        pendingWarmCacheResult_.reset();
    }

    [[nodiscard]] bool InitializeWarmCacheCoordinator() noexcept {
        if (warmCacheCoordinatorStarted_) {
            return true;
        }
        warmCacheCoordinatorStarted_ = warmCacheCoordinator_.Start(
            [this](
                const std::uint64_t generation,
                const ExportProgress& progress) {
                {
                    std::scoped_lock lock(warmCachePendingMutex_);
                    pendingWarmCacheProgress_ =
                        WarmCacheProgressState{generation, progress};
                }
                PostWarmCacheProgressNotification();
            },
            [this](
                const std::uint64_t generation,
                MediaExportResult result) {
                {
                    std::scoped_lock lock(warmCachePendingMutex_);
                    pendingWarmCacheResult_ =
                        WarmCacheResultState{generation, std::move(result)};
                }
                PostWarmCacheCompletionNotification();
            });
        return warmCacheCoordinatorStarted_;
    }

    void PostWarmCacheProgressNotification() noexcept {
        if (warmCacheProgressMessagePending_.exchange(
                true, std::memory_order_acq_rel)) {
            return;
        }
        const HWND target = notificationTarget_.load(std::memory_order_acquire);
        if (target == nullptr ||
            ::PostMessageW(target, kWarmCacheProgressMessage, 0, 0) == FALSE) {
            warmCacheProgressMessagePending_.store(false, std::memory_order_release);
        }
    }

    void PostWarmCacheCompletionNotification() noexcept {
        if (warmCacheCompletionMessagePending_.exchange(
                true, std::memory_order_acq_rel)) {
            return;
        }
        const HWND target = notificationTarget_.load(std::memory_order_acquire);
        if (target == nullptr ||
            ::PostMessageW(target, kWarmCacheCompletedMessage, 0, 0) == FALSE) {
            warmCacheCompletionMessagePending_.store(false, std::memory_order_release);
        }
    }

    void InvalidateWarmCacheWork() {
        if (window_ != nullptr) {
            ::KillTimer(window_, kWarmCacheDebounceTimer);
        }
        warmCacheDebounceArmed_ = false;
        ++warmCacheGeneration_;
        warmCacheReady_ = false;
        warmCacheInProgress_ = false;
        if (warmCacheCoordinatorStarted_) {
            warmCacheCoordinator_.Cancel();
        }
        ClearPendingWarmCacheMessages();
    }

    void StopWarmCacheAndWait() noexcept {
        try {
            InvalidateWarmCacheWork();
            warmCacheCoordinator_.StopAndWait();
            warmCacheCoordinatorStarted_ = false;
        } catch (...) {
            // 关闭窗口时预热取消必须静默且不能阻断窗口销毁。
        }
    }

    [[nodiscard]] bool InitializePreviewProxyCoordinator() noexcept {
        if (previewProxyCoordinatorStarted_) {
            return true;
        }
        previewProxyCoordinatorStarted_ = previewProxyCoordinator_.Start(
            {},
            [this](
                const std::uint64_t generation,
                MediaExportResult result) {
                {
                    std::scoped_lock lock(previewProxyPendingMutex_);
                    pendingPreviewProxyResult_ =
                        WarmCacheResultState{generation, std::move(result)};
                }
                if (previewProxyCompletionMessagePending_.exchange(
                        true,
                        std::memory_order_acq_rel)) {
                    return;
                }
                const HWND target = notificationTarget_.load(
                    std::memory_order_acquire);
                if (target == nullptr ||
                    ::PostMessageW(
                        target,
                        kPreviewProxyCompletedMessage,
                        0,
                        0) == FALSE) {
                    previewProxyCompletionMessagePending_.store(
                        false,
                        std::memory_order_release);
                }
            });
        return previewProxyCoordinatorStarted_;
    }

    void InvalidatePreviewProxyWork() {
        if (window_ != nullptr) {
            ::KillTimer(window_, kPreviewProxyDebounceTimer);
        }
        previewProxyDebounceArmed_ = false;
        ++previewProxyGeneration_;
        previewProxyInProgress_ = false;
        if (previewProxyCoordinatorStarted_) {
            previewProxyCoordinator_.Cancel();
        }
        std::scoped_lock lock(previewProxyPendingMutex_);
        pendingPreviewProxyResult_.reset();
    }

    void StopPreviewProxyAndWait() noexcept {
        try {
            InvalidatePreviewProxyWork();
            previewProxyCoordinator_.StopAndWait();
            previewProxyCoordinatorStarted_ = false;
        } catch (...) {
            // 预览代理属于可降级功能，关闭时不能阻断窗口销毁。
        }
    }

    void SchedulePreviewProxy(const UINT debounceMilliseconds) {
        InvalidatePreviewProxyWork();
        if (qualityPercent_ >= media::ExportQuality::DefaultPercent) {
            SwitchPreviewMedia(recording_.sourcePath);
            return;
        }
        previewProxyDebounceArmed_ = ::SetTimer(
            window_,
            kPreviewProxyDebounceTimer,
            std::max<UINT>(1, debounceMilliseconds),
            nullptr) != 0;
        if (!previewProxyDebounceArmed_) {
            StartPreviewProxy();
        }
    }

    void StartPreviewProxy() {
        if (qualityPercent_ >= media::ExportQuality::DefaultPercent ||
            window_ == nullptr || ::IsWindow(window_) == FALSE) {
            return;
        }
        if (!InitializePreviewProxyCoordinator()) {
            WriteDiagnostic(L"画质预览代理协调器不可用；保留当前预览。");
            return;
        }

        ExportRequest request{};
        request.recording = recording_;
        request.trimStart = std::chrono::milliseconds::zero();
        request.trimEnd = recording_.duration;
        request.format = OutputFormat::Mp4;
        request.includeSystemAudio = false;
        request.playbackSpeedTenths = EditorSpeedControl::DefaultSpeedTenths;
        request.qualityPercent = qualityPercent_;
        request.destinationPath.clear();
        const std::uint64_t generation = previewProxyGeneration_;
        previewProxyInProgress_ = true;
        if (!previewProxyCoordinator_.Submit(generation, std::move(request))) {
            previewProxyInProgress_ = false;
            WriteDiagnostic(L"画质预览代理请求提交失败；保留当前预览。");
        }
    }

    void SwitchPreviewMedia(const std::filesystem::path& mediaPath) {
        if (mediaPath.empty()) {
            return;
        }
        if (!activePreviewPath_.empty() &&
            _wcsicmp(activePreviewPath_.c_str(), mediaPath.c_str()) == 0) {
            return;
        }

        const PreviewReloadState reload{
            std::clamp(
                timeline_.Playhead(),
                std::chrono::milliseconds::zero(),
                duration_),
            playing_};
        StopPlaybackUiTimer();
        if (timelineSeekTimerArmed_) {
            ::KillTimer(window_, kTimelineSeekTimer);
        }
        timelineSeekTimerArmed_ = false;
        pendingTimelineSeek_.reset();
        inFlightTimelineSeek_.reset();
        previewSeekInFlight_ = false;
        pendingTimelineSeekFinal_ = false;
        playAfterTimelineSeek_ = false;
        awaitingNaturalPlaybackEnd_ = false;
        playing_ = false;
        UpdatePlayButton();

        preview_.Close();
        mediaItemReady_ = false;
        activePreviewPath_.clear();
        previewReloadState_ = reload;
        std::wstring previewError;
        if (preview_.Open(mediaPath, previewHost_, window_, &previewError)) {
            activePreviewPath_ = mediaPath;
            return;
        }

        WriteDiagnostic(std::format(
            L"画质预览切换失败：path={}，error={}",
            mediaPath.wstring(),
            previewError));
        if (_wcsicmp(mediaPath.c_str(), recording_.sourcePath.c_str()) != 0 &&
            preview_.Open(recording_.sourcePath, previewHost_, window_, &previewError)) {
            activePreviewPath_ = recording_.sourcePath;
            return;
        }
        previewReloadState_.reset();
        SetStatus(
            L"画质预览不可用；最终导出仍可继续。",
            EditorStatusTone::Error);
    }

    void HandlePreviewProxyCompleted() {
        std::optional<WarmCacheResultState> state;
        {
            std::scoped_lock lock(previewProxyPendingMutex_);
            state = std::move(pendingPreviewProxyResult_);
            pendingPreviewProxyResult_.reset();
        }
        previewProxyCompletionMessagePending_.store(
            false,
            std::memory_order_release);
        if (!state.has_value() ||
            state->generation != previewProxyGeneration_) {
            return;
        }
        previewProxyInProgress_ = false;
        WriteDiagnostic(std::format(
            L"画质预览代理结果：generation={}，quality={}%，success={}，"
            L"cacheHit={}，outputBytes={}，output={}，error={}",
            state->generation,
            qualityPercent_,
            state->result.success ? L"是" : L"否",
            state->result.cacheHit ? L"是" : L"否",
            state->result.outputBytes,
            state->result.outputPath.wstring(),
            state->result.errorMessage));
        if (state->result.success && !state->result.outputPath.empty()) {
            SwitchPreviewMedia(state->result.outputPath);
        }
    }

    void ScheduleWarmCache(
        const std::optional<UINT> debounceMilliseconds = std::nullopt) {
        InvalidateWarmCacheWork();
        if (!NeedsWarmCache()) {
            if (!busy_) {
                UpdateReadyStatus();
            }
            return;
        }

        if (!busy_) {
            SetStatus(L"正在后台准备，可继续预览", EditorStatusTone::Progress);
        }

        warmCacheDebounceArmed_ = ::SetTimer(
            window_,
            kWarmCacheDebounceTimer,
            debounceMilliseconds.value_or(
                selectedFormat_ == OutputFormat::Mp4
                    ? kWarmCacheMp4DebounceMilliseconds
                    : kWarmCacheGifDebounceMilliseconds),
            nullptr) != 0;
        if (!warmCacheDebounceArmed_) {
            StartWarmCache();
        }
    }

    void StartWarmCache() {
        if (!NeedsWarmCache() || window_ == nullptr ||
            ::IsWindow(window_) == FALSE) {
            return;
        }

        if (!InitializeWarmCacheCoordinator()) {
            warmCacheInProgress_ = false;
            warmCacheReady_ = false;
            WriteDiagnostic(L"后台预生成：协调器不可用，本次预生成已跳过。");
            if (!busy_) {
                UpdateReadyStatus();
            }
            return;
        }

        ClearPendingWarmCacheMessages();
        const std::uint64_t generation = warmCacheGeneration_;
        ExportRequest request = BuildExportRequest({});
        warmCacheInProgress_ = true;
        warmCacheReady_ = false;
        if (!busy_) {
            SetStatus(L"正在后台准备，可继续预览", EditorStatusTone::Progress);
        }
        WriteDiagnostic(std::format(
            L"后台预生成请求：generation={}，format={}，trimStart={} ms，trimEnd={} ms，"
            L"duration={} ms，trimRangeEdited={}，includeSystemAudio={}，speed={:.1f}x，"
            L"quality={}%，requestStart={} ms，requestEnd={} ms",
            generation,
            OutputFormatName(request.format),
            trimStart_.count(),
            trimEnd_.count(),
            duration_.count(),
            trimRangeEdited_ ? L"是" : L"否",
            request.includeSystemAudio ? L"是" : L"否",
            request.playbackSpeedTenths / 10.0,
            request.qualityPercent,
            request.trimStart.count(),
            request.trimEnd.count()));
        if (!warmCacheCoordinator_.Submit(generation, std::move(request))) {
            warmCacheInProgress_ = false;
            warmCacheReady_ = false;
            WriteDiagnostic(L"后台预生成：请求提交失败，导出时将自动重试。");
            if (!busy_) {
                UpdateReadyStatus();
            }
        }
    }

    void HandleWarmCacheProgress() {
        std::optional<WarmCacheProgressState> state;
        {
            std::scoped_lock lock(warmCachePendingMutex_);
            state = std::move(pendingWarmCacheProgress_);
            pendingWarmCacheProgress_.reset();
        }
        warmCacheProgressMessagePending_.store(false, std::memory_order_release);
        bool progressArrivedDuringConsume = false;
        {
            std::scoped_lock lock(warmCachePendingMutex_);
            progressArrivedDuringConsume = pendingWarmCacheProgress_.has_value();
        }
        if (progressArrivedDuringConsume) {
            PostWarmCacheProgressNotification();
        }
        if (!state.has_value() || state->generation != warmCacheGeneration_) {
            return;
        }

        warmCacheInProgress_ = true;
        if (!busy_) {
            const int percentage = static_cast<int>(std::lround(
                std::clamp(state->progress.fraction, 0.0, 1.0) * 100.0));
            const std::wstring progressText =
                qualityPercent_ < media::ExportQuality::DefaultPercent
                ? std::format(
                    L"正在后台准备 {}% 画质成片，可继续预览  {}%",
                    qualityPercent_,
                    percentage)
                : playbackSpeedTenths_ != EditorSpeedControl::DefaultSpeedTenths
                ? std::format(
                    L"正在后台准备倍速成片，可继续预览  {}%",
                    percentage)
                : std::format(
                    L"正在后台准备，可继续预览  {}%",
                    percentage);
            SetStatus(
                progressText,
                EditorStatusTone::Progress);
        }
    }

    void HandleWarmCacheCompleted() {
        std::optional<WarmCacheResultState> state;
        {
            std::scoped_lock lock(warmCachePendingMutex_);
            state = std::move(pendingWarmCacheResult_);
            pendingWarmCacheResult_.reset();
        }
        warmCacheCompletionMessagePending_.store(false, std::memory_order_release);
        bool completionArrivedDuringConsume = false;
        {
            std::scoped_lock lock(warmCachePendingMutex_);
            completionArrivedDuringConsume = pendingWarmCacheResult_.has_value();
        }
        if (completionArrivedDuringConsume) {
            PostWarmCacheCompletionNotification();
        }
        if (!state.has_value() || state->generation != warmCacheGeneration_) {
            return;
        }

        warmCacheInProgress_ = false;
        warmCacheReady_ = state->result.success;
        if (state->result.success && state->result.outputBytes != 0) {
            SetDisplayedOutputSize(state->result.outputBytes, true);
        }
        WriteDiagnostic(std::format(
            L"后台预生成结果：generation={}，success={}，cancelled={}，"
            L"disposition={}，delivery={}，elapsed={} ms，cacheHit={}，"
            L"waitedForBuilder={}，builderWait={} ms，generationElapsed={} ms，"
            L"sourceBytes={}，outputBytes={}，nativeError=0x{:08X}，"
            L"output={}，cacheKey={}，diagnostic={}",
            state->generation,
            state->result.success ? L"是" : L"否",
            state->result.cancelled ? L"是" : L"否",
            ExportDispositionName(state->result.disposition),
            MediaExporter::DeliveryName(state->result.delivery),
            state->result.elapsed.count(),
            state->result.cacheHit ? L"是" : L"否",
            state->result.waitedForCacheBuilder ? L"是" : L"否",
            state->result.cacheBuilderWait.count(),
            state->result.cacheGeneration.count(),
            state->result.sourceBytes,
            state->result.outputBytes,
            static_cast<unsigned long>(state->result.nativeError),
            state->result.outputPath.wstring(),
            state->result.cacheKey,
            state->result.diagnosticSummary));

        if (state->result.cancelled) {
            return;
        }
        if (!busy_) {
            if (state->result.success) {
                SetStatus(
                    ShouldIncludeSystemAudio()
                        ? L"有声 MP4 已准备完成，复制或保存将立即完成"
                        : L"已准备完成，复制或保存将立即完成",
                    EditorStatusTone::Success);
            } else {
                SetStatus(
                    L"后台准备未完成；导出时将自动重试",
                    EditorStatusTone::Neutral);
            }
        }
    }

    void WriteDiagnostic(const std::wstring& message) const {
        if (!callbacks_.diagnostic) {
            return;
        }
        try {
            callbacks_.diagnostic(message);
        } catch (...) {
            // 诊断日志失败不能影响导出或编辑器生命周期。
        }
    }

    void LogExportRequest(
        const ExportRequest& request,
        const ExportAction action,
        const bool passthrough) {
        WriteDiagnostic(std::format(
            L"编辑器导出请求：action={}，format={}，trimStart={} ms，trimEnd={} ms，"
            L"duration={} ms，trimRangeEdited={}，requestStart={} ms，requestEnd={} ms，"
            L"recordingDuration={} ms，includeSystemAudio={}，speed={:.1f}x，quality={}%，"
            L"mode={}，source={}，"
            L"audioSource={}，destination={}",
            ExportActionName(action),
            OutputFormatName(request.format),
            trimStart_.count(),
            trimEnd_.count(),
            duration_.count(),
            trimRangeEdited_ ? L"是" : L"否",
            request.trimStart.count(),
            request.trimEnd.count(),
            request.recording.duration.count(),
            request.includeSystemAudio ? L"是" : L"否",
            request.playbackSpeedTenths / 10.0,
            request.qualityPercent,
            passthrough ? L"原片直通" : L"重新生成媒体",
            request.recording.sourcePath.wstring(),
            request.recording.systemAudio.sourcePath.wstring(),
            request.destinationPath.wstring()));
    }

    void SetInitialExportStatus() {
        if (currentExportWarmReady_) {
            SetStatus(
                currentExportAction_ == ExportAction::Copy
                    ? L"正在提交已准备成片，完成后将复制到剪贴板…"
                    : L"正在提交已准备成片，完成后将保存到本地…",
                EditorStatusTone::Progress);
        } else if (currentExportPassthrough_) {
            SetStatus(
                currentExportAction_ == ExportAction::Copy
                    ? L"正在复制原始 MP4（不重新编码）…"
                    : L"正在保存原始 MP4（不重新编码）…",
                EditorStatusTone::Progress);
        } else if (currentExportFormat_ == OutputFormat::Gif) {
            SetStatus(
                currentExportAction_ == ExportAction::Copy
                    ? L"正在生成 GIF，完成后将复制到剪贴板…"
                    : L"正在生成 GIF，完成后将保存到本地…",
                EditorStatusTone::Progress);
        } else if (qualityPercent_ < media::ExportQuality::DefaultPercent) {
            SetStatus(
                currentExportAction_ == ExportAction::Copy
                    ? L"正在提交画质缓存，完成后将复制到剪贴板…"
                    : L"正在提交画质缓存，完成后将保存到本地…",
                EditorStatusTone::Progress);
        } else if (playbackSpeedTenths_ != EditorSpeedControl::DefaultSpeedTenths) {
            SetStatus(
                currentExportAction_ == ExportAction::Copy
                    ? L"正在生成倍速 MP4，完成后将复制到剪贴板…"
                    : L"正在生成倍速 MP4，完成后将保存到本地…",
                EditorStatusTone::Progress);
        } else if (currentExportSystemAudio_) {
            SetStatus(
                currentExportAction_ == ExportAction::Copy
                    ? L"正在快速合并电脑声音，完成后将复制到剪贴板…"
                    : L"正在快速合并电脑声音，完成后将保存到本地…",
                EditorStatusTone::Progress);
        } else {
            SetStatus(
                currentExportAction_ == ExportAction::Copy
                    ? L"正在编码精确裁切 MP4，完成后将复制到剪贴板…"
                    : L"正在编码精确裁切 MP4，完成后将保存到本地…",
                EditorStatusTone::Progress);
        }
    }

    void BeginSaveExport() {
        if (exporter_.IsRunning()) {
            return;
        }
        std::wstring directoryError;
        if (!win32::EnsureDirectory(settings_.saveDirectory, &directoryError)) {
            win32::ShowError(window_, L"无法保存", directoryError);
            return;
        }

        std::wstring pathError;
        const std::filesystem::path destination = win32::MakeUniquePath(
            settings_.saveDirectory,
            L"录屏",
            ExtensionFor(selectedFormat_),
            &pathError);
        if (destination.empty()) {
            win32::ShowError(
                window_,
                L"无法保存",
                pathError.empty() ? L"无法生成安全的保存文件名。" : pathError);
            return;
        }
        BeginExport(destination, ExportAction::Save);
    }

    void BeginClipboardExport() {
        if (exporter_.IsRunning()) {
            return;
        }

        const ExportRequest eligibility = BuildExportRequest(
            recording_.sourcePath.parent_path() / L"clipboard.mp4");
        if (MediaExporter::CanUsePassthrough(eligibility)) {
            const auto startedAt = std::chrono::steady_clock::now();
            LogExportRequest(eligibility, ExportAction::Copy, true);
            EditorExportResult result{};
            result.format = selectedFormat_;
            result.finalPath = recording_.sourcePath;
            result.copiedToClipboard = MediaExporter::CopyFileToClipboard(
                window_, recording_.sourcePath, &result.errorMessage);
            result.success = result.copiedToClipboard;
            const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - startedAt);
            if (result.success) {
                SetStatus(
                    L"原始 MP4 已复制，可直接粘贴（未重新编码）",
                    EditorStatusTone::Success);
                WriteDiagnostic(std::format(
                    L"编辑器导出结果：action=复制，format=MP4，success=是，"
                    L"disposition=ClipboardDirect，elapsed={} ms，output={}",
                    elapsed.count(),
                    result.finalPath.wstring()));
            } else {
                SetStatus(L"复制失败，源录屏已保留。", EditorStatusTone::Error);
                WriteDiagnostic(std::format(
                    L"编辑器导出结果：action=复制，format=MP4，success=否，"
                    L"disposition=ClipboardDirect，elapsed={} ms，error={}",
                    elapsed.count(),
                    result.errorMessage));
                win32::ShowError(window_, L"复制失败", result.errorMessage);
            }
            if (callbacks_.exportCompleted) {
                try {
                    callbacks_.exportCompleted(result);
                } catch (...) {
                }
            }
            return;
        }

        // Keep clipboard materialization on the recording/cache volume so a
        // trimmed artifact can be submitted through an NTFS hard link rather
        // than copied to the system drive.
        std::filesystem::path clipboardDirectory =
            recording_.sourcePath.parent_path();
        std::wstring error;
        if (clipboardDirectory.empty() ||
            !win32::EnsureDirectory(clipboardDirectory, &error)) {
            clipboardDirectory = win32::LocalAppDataDirectory() / L"Clipboard";
            if (!win32::EnsureDirectory(clipboardDirectory, &error)) {
                win32::ShowError(window_, L"无法复制", error);
                return;
            }
        }
        std::wstring pathError;
        const std::filesystem::path destination = win32::MakeUniquePath(
            clipboardDirectory,
            L"录屏",
            ExtensionFor(selectedFormat_),
            &pathError);
        if (destination.empty()) {
            win32::ShowError(
                window_,
                L"无法复制",
                pathError.empty() ? L"无法生成安全的剪贴板文件名。" : pathError);
            return;
        }
        BeginExport(destination, ExportAction::Copy);
    }

    void BeginExport(const std::filesystem::path& destination, const ExportAction action) {
        if (timelineSeekTimerArmed_) {
            ::KillTimer(window_, kTimelineSeekTimer);
            timelineSeekTimerArmed_ = false;
        }
        pendingTimelineSeek_.reset();
        pendingTimelineSeekFinal_ = false;
        playAfterTimelineSeek_ = false;
        awaitingNaturalPlaybackEnd_ = false;
        static_cast<void>(preview_.Pause());
        playing_ = false;
        StopPlaybackUiTimer();
        UpdatePlayButton();

        ExportRequest request = BuildExportRequest(destination);
        currentExportAction_ = action;
        currentExportFormat_ = selectedFormat_;
        currentExportSystemAudio_ = request.includeSystemAudio;
        currentExportPassthrough_ = MediaExporter::CanUsePassthrough(request);
        currentExportWarmReady_ = warmCacheReady_ && NeedsWarmCache();
        currentExportStartedAt_ = std::chrono::steady_clock::now();
        currentExportTimingActive_ = true;
        lastExportProgressSignalMilliseconds_.store(0, std::memory_order_release);
        LogExportRequest(request, action, currentExportPassthrough_);
        SetBusy(true);
        SetInitialExportStatus();

        const bool started = exporter_.StartExport(
            std::move(request),
            [this](const ExportProgress& progress) {
                {
                    std::scoped_lock lock(pendingMutex_);
                    pendingProgress_ = progress;
                }
                if (!ShouldSignalExportProgress(progress)) {
                    return;
                }
                const HWND target = notificationTarget_.load(std::memory_order_acquire);
                if (!exportProgressMessagePending_.exchange(
                        true, std::memory_order_acq_rel) &&
                    (target == nullptr ||
                     ::PostMessageW(target, kExportProgressMessage, 0, 0) == FALSE)) {
                    exportProgressMessagePending_.store(
                        false, std::memory_order_release);
                }
            },
            [this](const MediaExportResult& result) {
                {
                    std::scoped_lock lock(pendingMutex_);
                    pendingResult_ = result;
                }
                const HWND target = notificationTarget_.load(std::memory_order_acquire);
                if (target != nullptr) {
                    ::PostMessageW(target, kExportCompletedMessage, 0, 0);
                }
            });
        if (!started) {
            currentExportTimingActive_ = false;
            SetBusy(false);
            WriteDiagnostic(L"编辑器导出未启动：已有导出任务正在运行。");
            win32::ShowError(window_, L"无法导出", L"已有导出任务正在运行。" );
        }
    }

    [[nodiscard]] bool ShouldSignalExportProgress(
        const ExportProgress& progress) noexcept {
        const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
        auto previous = lastExportProgressSignalMilliseconds_.load(
            std::memory_order_acquire);
        const bool finalProgress = progress.fraction >= 1.0;
        while (finalProgress || now - previous >= kProgressUiInterval.count()) {
            if (lastExportProgressSignalMilliseconds_.compare_exchange_weak(
                    previous,
                    now,
                    std::memory_order_acq_rel,
                    std::memory_order_acquire)) {
                return true;
            }
        }
        return false;
    }

    void SetBusy(const bool busy) {
        busy_ = busy;
        timeline_.SetEnabled(!busy);
        speedControl_.SetEnabled(!busy);
        qualityControl_.SetEnabled(!busy);
        const std::array controls{
            playButton_, mp4Radio_, gifRadio_, saveButton_, copyButton_};
        for (const HWND control : controls) {
            ::EnableWindow(control, busy ? FALSE : TRUE);
        }
        UpdateTrimBoundaryButtonStates();
        RefreshAudioToggleState();
        const wchar_t* saveLabel = L"保存到本地";
        const wchar_t* copyLabel = L"复制到剪贴板";
        if (busy && currentExportAction_ == ExportAction::Save) {
            saveLabel = currentExportWarmReady_
                ? L"正在保存…"
                : (currentExportPassthrough_
                ? L"正在保存…"
                : (currentExportFormat_ == OutputFormat::Gif
                    ? L"正在生成…"
                    : (currentExportSystemAudio_ ? L"正在合并…" : L"正在编码…")));
        } else if (busy && currentExportAction_ == ExportAction::Copy) {
            copyLabel = currentExportWarmReady_
                ? L"正在复制…"
                : (currentExportPassthrough_
                ? L"正在复制…"
                : (currentExportFormat_ == OutputFormat::Gif
                    ? L"正在生成…"
                    : (currentExportSystemAudio_ ? L"正在合并…" : L"正在编码…")));
        }
        ::SetWindowTextW(saveButton_, saveLabel);
        ::SetWindowTextW(copyButton_, copyLabel);
        ::InvalidateRect(saveButton_, nullptr, FALSE);
        ::InvalidateRect(copyButton_, nullptr, FALSE);
    }

    void HandleExportProgress() {
        std::optional<ExportProgress> progress;
        {
            std::scoped_lock lock(pendingMutex_);
            progress = std::move(pendingProgress_);
            pendingProgress_.reset();
        }
        exportProgressMessagePending_.store(false, std::memory_order_release);
        bool progressArrivedDuringConsume = false;
        {
            std::scoped_lock lock(pendingMutex_);
            progressArrivedDuringConsume = pendingProgress_.has_value();
        }
        if (progressArrivedDuringConsume &&
            !exportProgressMessagePending_.exchange(
                true, std::memory_order_acq_rel)) {
            const HWND target = notificationTarget_.load(std::memory_order_acquire);
            if (target == nullptr ||
                ::PostMessageW(target, kExportProgressMessage, 0, 0) == FALSE) {
                exportProgressMessagePending_.store(
                    false, std::memory_order_release);
            }
        }
        if (!progress.has_value()) {
            return;
        }
        const int percentage = static_cast<int>(std::lround(progress->fraction * 100.0));
        const std::wstring status = std::format(L"{}  {}%", progress->phase, percentage);
        SetStatus(status, EditorStatusTone::Progress);
    }

    void HandleExportCompleted() {
        std::optional<MediaExportResult> mediaResult;
        {
            std::scoped_lock lock(pendingMutex_);
            mediaResult = pendingResult_;
            pendingResult_.reset();
        }
        if (!mediaResult.has_value()) {
            return;
        }
        const auto wallElapsed = currentExportTimingActive_
            ? std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - currentExportStartedAt_)
            : std::chrono::milliseconds::zero();
        currentExportTimingActive_ = false;
        SetBusy(false);

        EditorExportResult editorResult{};
        editorResult.success = mediaResult->success;
        editorResult.format = currentExportFormat_;
        editorResult.finalPath = mediaResult->outputPath;
        editorResult.errorMessage = mediaResult->errorMessage;

        if (mediaResult->success && currentExportAction_ == ExportAction::Copy) {
            std::wstring clipboardError;
            editorResult.copiedToClipboard = MediaExporter::CopyFileToClipboard(
                window_, mediaResult->outputPath, &clipboardError);
            if (!editorResult.copiedToClipboard) {
                editorResult.success = false;
                editorResult.errorMessage = std::move(clipboardError);
            }
        }

        if (editorResult.success) {
            if (mediaResult->outputBytes != 0) {
                estimatedOutputBytes_ = mediaResult->outputBytes;
                SetDisplayedOutputSize(mediaResult->outputBytes, true);
            }
            std::wstring status;
            if (currentExportSystemAudio_) {
                status = editorResult.copiedToClipboard
                    ? L"有声 MP4 已复制，可直接粘贴"
                    : L"有声 MP4 已保存：" +
                        editorResult.finalPath.wstring();
            } else if (mediaResult->cacheHit) {
                status = editorResult.copiedToClipboard
                    ? L"已直接提交并复制，无需再次编码"
                    : L"已直接提交并保存，无需再次编码：" +
                        editorResult.finalPath.wstring();
            } else if (editorResult.copiedToClipboard) {
                status = currentExportFormat_ == OutputFormat::Gif
                    ? L"GIF 已复制，可直接粘贴"
                    : L"裁切 MP4 已复制，可直接粘贴";
            } else if (mediaResult->disposition ==
                       MediaExportDisposition::HardLinkedPassthrough ||
                       mediaResult->disposition ==
                       MediaExportDisposition::CopiedPassthrough ||
                       mediaResult->disposition ==
                       MediaExportDisposition::OriginalPassthrough) {
                status = L"原始 MP4 已保存（未重新编码）：" +
                    editorResult.finalPath.wstring();
            } else {
                status = currentExportFormat_ == OutputFormat::Gif
                    ? L"GIF 已保存：" + editorResult.finalPath.wstring()
                    : L"裁切 MP4 已保存：" + editorResult.finalPath.wstring();
            }
            SetStatus(status, EditorStatusTone::Success);
        } else if (mediaResult->cancelled) {
            SetStatus(L"导出已取消；原始录屏已保留", EditorStatusTone::Neutral);
        } else {
            const std::wstring error = editorResult.errorMessage.empty()
                ? L"导出失败，源录屏已保留。"
                : editorResult.errorMessage + L"\n源录屏已保留：" + recording_.sourcePath.wstring();
            SetStatus(L"导出失败，源录屏已保留。", EditorStatusTone::Error);
            if (!mediaResult->cancelled) {
                win32::ShowError(window_, L"导出失败", error);
            }
        }

        const std::wstring logMessage = std::format(
            L"编辑器导出结果：action={}，format={}，success={}，cancelled={}，"
            L"includeSystemAudio={}，disposition={}，delivery={}，cacheHit={}，"
            L"mediaElapsed={} ms，"
            L"waitedForBuilder={}，builderWait={} ms，generationElapsed={} ms，"
            L"deliveryElapsed={} ms，"
            L"wallElapsed={} ms，sourceBytes={}，outputBytes={}，nativeError=0x{:08X}，"
            L"output={}，cacheKey={}，diagnostic={}，error={}",
            ExportActionName(currentExportAction_),
            OutputFormatName(currentExportFormat_),
            editorResult.success ? L"是" : L"否",
            mediaResult->cancelled ? L"是" : L"否",
            currentExportSystemAudio_ ? L"是" : L"否",
            ExportDispositionName(mediaResult->disposition),
            MediaExporter::DeliveryName(mediaResult->delivery),
            mediaResult->cacheHit ? L"是" : L"否",
            mediaResult->elapsed.count(),
            mediaResult->waitedForCacheBuilder ? L"是" : L"否",
            mediaResult->cacheBuilderWait.count(),
            mediaResult->cacheGeneration.count(),
            mediaResult->deliveryElapsed.count(),
            wallElapsed.count(),
            mediaResult->sourceBytes,
            mediaResult->outputBytes,
            static_cast<unsigned long>(mediaResult->nativeError),
            editorResult.finalPath.wstring(),
            mediaResult->cacheKey,
            mediaResult->diagnosticSummary,
            editorResult.errorMessage.empty() ? L"无" : editorResult.errorMessage);
        WriteDiagnostic(logMessage);

        if (callbacks_.exportCompleted) {
            try {
                callbacks_.exportCompleted(editorResult);
            } catch (...) {
            }
        }
    }

    HINSTANCE instance_{};
    HWND owner_{};
    HWND window_{};
    std::atomic<HWND> notificationTarget_{};

    HWND previewHost_{};
    HWND headerTitle_{};
    HWND headerSubtitle_{};
    HWND tooltipWindow_{};
    HWND trimStartButton_{};
    HWND trimEndButton_{};
    HWND playButton_{};
    HWND timeText_{};
    HWND rangeText_{};
    HWND outputSizeText_{};
    HWND formatLabel_{};
    HWND mp4Radio_{};
    HWND gifRadio_{};
    HWND saveButton_{};
    HWND copyButton_{};
    HWND statusText_{};
    int editorPanelTop_{};
    RECT previewStage_{};
    POINT statusDotCenter_{};
    EditorStatusTone statusTone_{EditorStatusTone::Neutral};

    EditorChrome chrome_;
    TrimTimeline timeline_;
    EditorSpeedControl speedControl_;
    EditorSpeedControl qualityControl_;
    EditorAudioToggle audioToggle_;
    MediaPreview preview_;
    MediaExporter exporter_;
    RecordingResult recording_;
    std::uint64_t boundaryEncoderGeneration_{};
    AppSettings settings_;
    EditorWindowCallbacks callbacks_;
    OutputFormat selectedFormat_{OutputFormat::Mp4};
    OutputFormat currentExportFormat_{OutputFormat::Mp4};
    bool audioRequestedForMp4_{};
    bool currentExportSystemAudio_{};
    ExportAction currentExportAction_{ExportAction::Save};
    bool currentExportPassthrough_{};
    bool currentExportWarmReady_{};
    bool forceClose_{};
    std::chrono::steady_clock::time_point currentExportStartedAt_{};
    bool currentExportTimingActive_{};
    std::chrono::milliseconds duration_{1};
    std::chrono::milliseconds trimStart_{};
    std::chrono::milliseconds trimEnd_{1};
    int playbackSpeedTenths_{EditorSpeedControl::DefaultSpeedTenths};
    int qualityPercent_{media::ExportQuality::DefaultPercent};
    std::uint64_t estimatedOutputBytes_{};
    std::uint64_t exactOutputBytes_{};
    bool outputSizeExact_{};
    bool trimRangeEdited_{};
    bool playing_{};
    bool busy_{};
    std::wstring creationError_;
    std::wstring displayedPlayButtonText_;
    std::wstring displayedTimeText_;
    std::wstring displayedRangeText_;
    std::wstring displayedOutputSizeText_;
    std::wstring displayedStatusText_;
    std::optional<std::chrono::milliseconds> pendingTimelineSeek_;
    std::optional<std::chrono::milliseconds> inFlightTimelineSeek_;
    std::chrono::steady_clock::time_point lastPlaybackTextRefresh_{};
    std::chrono::steady_clock::time_point lastTimelineLabelRefresh_{};
    std::chrono::steady_clock::time_point lastPreviewSeekIssuedAt_{};
    std::chrono::milliseconds lastTimeLabelPositionBucket_{-1};
    std::chrono::milliseconds lastTimeLabelDuration_{-1};
    std::chrono::milliseconds lastRangeLabelStart_{-1};
    std::chrono::milliseconds lastRangeLabelEnd_{-1};
    int lastRangeLabelSpeedTenths_{-1};
    bool playbackTimerArmed_{};
    bool timelineSeekTimerArmed_{};
    bool timelineInteractionTimerArmed_{};
    bool pendingTimelinePause_{};
    bool pendingTimelineSeekFinal_{};
    bool previewSeekInFlight_{};
    bool playAfterTimelineSeek_{};
    bool awaitingNaturalPlaybackEnd_{};
    bool timelineRangeInteractionActive_{};
    bool speedInteractionActive_{};
    bool qualityInteractionActive_{};
    bool mediaItemReady_{};
    bool previewVideoRefreshPosted_{};
    std::uint64_t timelinePreviewNotificationCount_{};
    std::uint64_t timelineCommittedNotificationCount_{};
    std::uint64_t previewSeekRequestCount_{};
    std::uint64_t previewSeekIssuedCount_{};
    std::uint64_t previewSeekCoalescedCount_{};
    std::array<std::int64_t, 256> previewSeekDurationSamples_{};
    std::size_t previewSeekDurationSampleCount_{};

    WarmCacheCoordinator warmCacheCoordinator_;
    std::uint64_t warmCacheGeneration_{};
    bool warmCacheDebounceArmed_{};
    bool warmCacheInProgress_{};
    bool warmCacheReady_{};
    bool warmCacheCoordinatorStarted_{};
    std::atomic_bool warmCacheProgressMessagePending_{};
    std::atomic_bool warmCacheCompletionMessagePending_{};
    std::mutex warmCachePendingMutex_;
    std::optional<WarmCacheProgressState> pendingWarmCacheProgress_;
    std::optional<WarmCacheResultState> pendingWarmCacheResult_;

    WarmCacheCoordinator previewProxyCoordinator_;
    std::uint64_t previewProxyGeneration_{};
    bool previewProxyDebounceArmed_{};
    bool previewProxyInProgress_{};
    bool previewProxyCoordinatorStarted_{};
    std::atomic_bool previewProxyCompletionMessagePending_{};
    std::mutex previewProxyPendingMutex_;
    std::optional<WarmCacheResultState> pendingPreviewProxyResult_;
    std::filesystem::path activePreviewPath_;
    std::optional<PreviewReloadState> previewReloadState_;

    std::mutex pendingMutex_;
    std::atomic_bool exportProgressMessagePending_{};
    std::atomic<std::int64_t> lastExportProgressSignalMilliseconds_{};
    std::optional<ExportProgress> pendingProgress_;
    std::optional<MediaExportResult> pendingResult_;
};

EditorWindow::EditorWindow(const HINSTANCE instance, const HWND owner)
    : impl_(std::make_unique<Impl>(instance, owner)) {}

EditorWindow::~EditorWindow() = default;

bool EditorWindow::Open(
    RecordingResult recording,
    AppSettings settings,
    EditorWindowCallbacks callbacks,
    std::wstring* errorMessage) {
    return impl_->Open(
        std::move(recording), std::move(settings), std::move(callbacks), errorMessage);
}

bool EditorWindow::Close() {
    return impl_->Close();
}

void EditorWindow::CloseForShutdown() noexcept {
    impl_->CloseForShutdown();
}

HWND EditorWindow::WindowHandle() const noexcept {
    return impl_->WindowHandle();
}

bool EditorWindow::IsOpen() const noexcept {
    return impl_->IsOpen();
}

}  // namespace qrec
