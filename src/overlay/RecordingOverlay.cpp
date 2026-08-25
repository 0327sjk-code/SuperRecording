#include "RecordingOverlay.h"
#include "RecordingOverlayPlacement.h"
#include "RecordingOverlayView.h"

#include <algorithm>
#include <utility>

#include "../ui/Theme.h"

namespace qrec::overlay {
namespace {

constexpr wchar_t kWindowClassName[] = L"SuperRecording.RecordingOverlay.Window";
constexpr auto kHoverEnterDuration = std::chrono::milliseconds(150);
constexpr auto kHoverExitDuration = std::chrono::milliseconds(110);
constexpr auto kPressEnterDuration = std::chrono::milliseconds(100);
constexpr auto kPressExitDuration = std::chrono::milliseconds(130);
constexpr auto kStateDuration = std::chrono::milliseconds(180);

constexpr wchar_t kWindowTitle[] = L"SuperRecording - 正在录制";
[[nodiscard]] int ScaleForDpi(int value, UINT dpi) noexcept {
    return MulDiv(value, static_cast<int>(dpi), USER_DEFAULT_SCREEN_DPI);
}

void SetOverlayMotionTarget(
    ui::MotionState& motion,
    const bool active,
    const std::chrono::milliseconds enterDuration,
    const std::chrono::milliseconds exitDuration,
    const ui::MotionEasing easing = ui::MotionEasing::EaseOutQuart) noexcept {
    (void)motion.SetTarget(
        active ? 1.0F : 0.0F,
        active ? enterDuration : exitDuration,
        easing,
        ui::ClientAreaAnimationsEnabled());
}

}  // namespace

RecordingOverlay::~RecordingOverlay() {
    Destroy();
}

bool RecordingOverlay::Create(
    HWND owner,
    const IntRect& recordingRegion,
    RecordingOverlayCallbacks callbacks,
    std::chrono::milliseconds initialElapsed) {
    if (window_ != nullptr || !recordingRegion.IsValid()) {
        return false;
    }

    owner_ = owner;
    recordingRegion_ = recordingRegion;
    callbacks_ = std::move(callbacks);
    paused_ = false;
    stopping_ = false;
    trackingMouse_ = false;
    dragging_ = false;
    manuallyPositioned_ = false;
    hovered_ = HitTarget::None;
    pressed_ = HitTarget::None;
    ResetMotion();
    accumulatedElapsed_ = std::max(initialElapsed, std::chrono::milliseconds::zero());
    segmentStarted_ = std::chrono::steady_clock::now();

    if (!CreateWindowForRegion(owner, recordingRegion) || window_ == nullptr) {
        callbacks_ = {};
        owner_ = nullptr;
        return false;
    }
    const HWND overlayWindow = window_;

    ShowWindow(overlayWindow, SW_SHOWNOACTIVATE);
    SetWindowPos(
        overlayWindow,
        HWND_TOPMOST,
        0,
        0,
        0,
        0,
        SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
    UpdateWindow(overlayWindow);
    return true;
}

void RecordingOverlay::Destroy() {
    if (window_ != nullptr) {
        KillTimer(window_, RefreshTimerId);
        KillTimer(window_, MotionTimerId);
        DestroyWindow(window_);
        window_ = nullptr;
    }
    if (bodyFont_ != nullptr) {
        DeleteObject(bodyFont_);
        bodyFont_ = nullptr;
    }
    if (actionFont_ != nullptr) {
        DeleteObject(actionFont_);
        actionFont_ = nullptr;
    }
    if (timerFont_ != nullptr) {
        DeleteObject(timerFont_);
        timerFont_ = nullptr;
    }
    callbacks_ = {};
    owner_ = nullptr;
    captureExcluded_ = false;
    dragging_ = false;
    trackingMouse_ = false;
    ResetMotion();
}

void RecordingOverlay::SetPaused(bool paused) {
    if (paused_ == paused || stopping_) {
        return;
    }

    const auto now = std::chrono::steady_clock::now();
    if (paused) {
        accumulatedElapsed_ += std::chrono::duration_cast<std::chrono::milliseconds>(now - segmentStarted_);
    } else {
        segmentStarted_ = now;
    }
    paused_ = paused;
    SetOverlayMotionTarget(
        pauseStateMotion_,
        paused,
        kStateDuration,
        kStateDuration,
        ui::MotionEasing::EaseOutQuint);
    UpdateMotionTimer();
    if (window_ != nullptr) {
        InvalidateTarget(HitTarget::Pause);
        InvalidateStatusRegion();
    }
}

void RecordingOverlay::SetElapsed(std::chrono::milliseconds elapsed) {
    accumulatedElapsed_ = std::max(elapsed, std::chrono::milliseconds::zero());
    segmentStarted_ = std::chrono::steady_clock::now();
    if (window_ != nullptr) {
        InvalidateStatusRegion();
    }
}

void RecordingOverlay::SetRegion(const IntRect& recordingRegion) {
    if (!recordingRegion.IsValid()) {
        return;
    }
    recordingRegion_ = recordingRegion;
    manuallyPositioned_ = false;

    const POINT anchor{recordingRegion.right - 1, recordingRegion.bottom - 1};
    UpdateDpi(placement::DpiAt(anchor));
    Reposition(false);
}

void RecordingOverlay::SetStopping(bool stopping) {
    if (stopping_ == stopping) {
        return;
    }
    if (stopping && !paused_) {
        accumulatedElapsed_ = Elapsed();
    }
    stopping_ = stopping;
    SetOverlayMotionTarget(
        stopStateMotion_,
        stopping,
        kStateDuration,
        kStateDuration,
        ui::MotionEasing::EaseOutQuint);
    UpdateMotionTimer();
    if (!stopping && !paused_) {
        segmentStarted_ = std::chrono::steady_clock::now();
    }
    if (window_ != nullptr) {
        InvalidateTarget(HitTarget::Pause);
        InvalidateTarget(HitTarget::Stop);
        InvalidateStatusRegion();
    }
}

std::chrono::milliseconds RecordingOverlay::Elapsed() const noexcept {
    if (paused_ || stopping_) {
        return accumulatedElapsed_;
    }
    return accumulatedElapsed_ + std::chrono::duration_cast<std::chrono::milliseconds>(
                                     std::chrono::steady_clock::now() - segmentStarted_);
}

bool RecordingOverlay::CreateWindowForRegion(HWND owner, const IntRect& recordingRegion) {
    const HINSTANCE instance = GetModuleHandleW(nullptr);
    WNDCLASSEXW windowClass{};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.style = CS_HREDRAW | CS_VREDRAW;
    windowClass.lpfnWndProc = &RecordingOverlay::WindowProcedure;
    windowClass.hInstance = instance;
    windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    windowClass.lpszClassName = kWindowClassName;
    if (RegisterClassExW(&windowClass) == 0 && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        return false;
    }

    const POINT anchor{recordingRegion.right - 1, recordingRegion.bottom - 1};
    dpi_ = placement::DpiAt(anchor);
    const SIZE desired = DesiredSize();
    window_ = CreateWindowExW(
        WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
        kWindowClassName,
        kWindowTitle,
        WS_POPUP,
        0,
        0,
        desired.cx,
        desired.cy,
        owner,
        nullptr,
        instance,
        this);
    if (window_ == nullptr) {
        return false;
    }

#ifndef WDA_EXCLUDEFROMCAPTURE
#define WDA_EXCLUDEFROMCAPTURE 0x00000011
#endif
    captureExcluded_ = SetWindowDisplayAffinity(window_, WDA_EXCLUDEFROMCAPTURE) != FALSE;
    if (captureExcluded_) {
        DWORD affinity = WDA_NONE;
        captureExcluded_ = GetWindowDisplayAffinity(window_, &affinity) != FALSE &&
            affinity == WDA_EXCLUDEFROMCAPTURE;
    }
    RecreateFonts();
    UpdateRoundedRegion();
    Reposition(false);
    if (SetTimer(window_, RefreshTimerId, RefreshIntervalMilliseconds, nullptr) == 0) {
        Destroy();
        return false;
    }
    return true;
}

void RecordingOverlay::Reposition(bool preserveManualPosition) {
    if (window_ == nullptr) {
        return;
    }

    const SIZE desired = DesiredSize();
    RECT position{};

    if (preserveManualPosition && manuallyPositioned_ && GetWindowRect(window_, &position)) {
        position.right = position.left + desired.cx;
        position.bottom = position.top + desired.cy;
        position = placement::ClampToWorkArea(position);
    } else {
        position = placement::AutomaticBounds(recordingRegion_, desired, dpi_);
    }

    SetWindowPos(
        window_,
        HWND_TOPMOST,
        position.left,
        position.top,
        desired.cx,
        desired.cy,
        SWP_NOACTIVATE | SWP_SHOWWINDOW);
    UpdateRoundedRegion();
}

void RecordingOverlay::UpdateDpi(UINT dpi) {
    dpi = dpi == 0 ? USER_DEFAULT_SCREEN_DPI : dpi;
    if (dpi_ == dpi && bodyFont_ != nullptr && actionFont_ != nullptr &&
        timerFont_ != nullptr) {
        return;
    }
    dpi_ = dpi;
    RecreateFonts();
    UpdateRoundedRegion();
}

void RecordingOverlay::RecreateFonts() {
    if (bodyFont_ != nullptr) {
        DeleteObject(bodyFont_);
        bodyFont_ = nullptr;
    }
    if (actionFont_ != nullptr) {
        DeleteObject(actionFont_);
        actionFont_ = nullptr;
    }
    if (timerFont_ != nullptr) {
        DeleteObject(timerFont_);
        timerFont_ = nullptr;
    }

    bodyFont_ = CreateFontW(
        -ScaleForDpi(12, dpi_),
        0,
        0,
        0,
        FW_NORMAL,
        FALSE,
        FALSE,
        FALSE,
        DEFAULT_CHARSET,
        OUT_TT_ONLY_PRECIS,
        CLIP_DEFAULT_PRECIS,
        theme::FontQuality,
        VARIABLE_PITCH | FF_SWISS,
        theme::FontFamily);
    actionFont_ = CreateFontW(
        -ScaleForDpi(12, dpi_),
        0,
        0,
        0,
        FW_SEMIBOLD,
        FALSE,
        FALSE,
        FALSE,
        DEFAULT_CHARSET,
        OUT_TT_ONLY_PRECIS,
        CLIP_DEFAULT_PRECIS,
        theme::FontQuality,
        VARIABLE_PITCH | FF_SWISS,
        theme::FontFamily);
    timerFont_ = CreateFontW(
        -ScaleForDpi(12, dpi_),
        0,
        0,
        0,
        FW_NORMAL,
        FALSE,
        FALSE,
        FALSE,
        DEFAULT_CHARSET,
        OUT_TT_ONLY_PRECIS,
        CLIP_DEFAULT_PRECIS,
        theme::FontQuality,
        VARIABLE_PITCH | FF_SWISS,
        theme::LatinFontFamily);
}

void RecordingOverlay::UpdateRoundedRegion() {
    view::ApplyRoundedWindowRegion(window_, dpi_);
}

RECT RecordingOverlay::DragBounds() const noexcept {
    return view::DragBounds(dpi_);
}

RECT RecordingOverlay::PauseBounds() const noexcept {
    return view::PauseBounds(dpi_);
}

RECT RecordingOverlay::StopBounds() const noexcept {
    return view::StopBounds(dpi_);
}

SIZE RecordingOverlay::DesiredSize() const noexcept {
    return view::DesiredSize(dpi_);
}

void RecordingOverlay::SetHoveredTarget(const HitTarget target) noexcept {
    if (hovered_ == target) {
        return;
    }
    const HitTarget previous = hovered_;
    hovered_ = target;
    SetOverlayMotionTarget(
        dragHoverMotion_,
        target == HitTarget::DragHandle,
        kHoverEnterDuration,
        kHoverExitDuration);
    SetOverlayMotionTarget(
        pauseHoverMotion_,
        target == HitTarget::Pause,
        kHoverEnterDuration,
        kHoverExitDuration);
    SetOverlayMotionTarget(
        stopHoverMotion_,
        target == HitTarget::Stop,
        kHoverEnterDuration,
        kHoverExitDuration);
    UpdateMotionTimer();
    InvalidateTarget(previous);
    InvalidateTarget(target);
}

void RecordingOverlay::SetPressedTarget(const HitTarget target) noexcept {
    if (pressed_ == target &&
        dragPressMotion_.Target() == (target == HitTarget::DragHandle ? 1.0F : 0.0F)) {
        return;
    }
    const HitTarget previous = pressed_;
    pressed_ = target;
    SetOverlayMotionTarget(
        dragPressMotion_,
        target == HitTarget::DragHandle,
        kPressEnterDuration,
        kPressExitDuration,
        ui::MotionEasing::EaseOutQuint);
    SetOverlayMotionTarget(
        pausePressMotion_,
        target == HitTarget::Pause,
        kPressEnterDuration,
        kPressExitDuration,
        ui::MotionEasing::EaseOutQuint);
    SetOverlayMotionTarget(
        stopPressMotion_,
        target == HitTarget::Stop,
        kPressEnterDuration,
        kPressExitDuration,
        ui::MotionEasing::EaseOutQuint);
    UpdateMotionTimer();
    InvalidateTarget(previous);
    InvalidateTarget(target);
}

void RecordingOverlay::UpdateMotionTimer() noexcept {
    if (window_ == nullptr) {
        return;
    }
    const bool active = dragHoverMotion_.IsActive() || dragPressMotion_.IsActive() ||
        pauseHoverMotion_.IsActive() || pausePressMotion_.IsActive() ||
        pauseStateMotion_.IsActive() || stopHoverMotion_.IsActive() ||
        stopPressMotion_.IsActive() || stopStateMotion_.IsActive();
    if (active) {
        if (SetTimer(window_, MotionTimerId, MotionFrameMilliseconds, nullptr) == 0) {
            dragHoverMotion_.JumpTo(dragHoverMotion_.Target());
            dragPressMotion_.JumpTo(dragPressMotion_.Target());
            pauseHoverMotion_.JumpTo(pauseHoverMotion_.Target());
            pausePressMotion_.JumpTo(pausePressMotion_.Target());
            pauseStateMotion_.JumpTo(pauseStateMotion_.Target());
            stopHoverMotion_.JumpTo(stopHoverMotion_.Target());
            stopPressMotion_.JumpTo(stopPressMotion_.Target());
            stopStateMotion_.JumpTo(stopStateMotion_.Target());
            InvalidateMotionRegions();
        }
    } else {
        KillTimer(window_, MotionTimerId);
    }
}

bool RecordingOverlay::AdvanceMotion() noexcept {
    if (!ui::ClientAreaAnimationsEnabled()) {
        dragHoverMotion_.JumpTo(dragHoverMotion_.Target());
        dragPressMotion_.JumpTo(dragPressMotion_.Target());
        pauseHoverMotion_.JumpTo(pauseHoverMotion_.Target());
        pausePressMotion_.JumpTo(pausePressMotion_.Target());
        pauseStateMotion_.JumpTo(pauseStateMotion_.Target());
        stopHoverMotion_.JumpTo(stopHoverMotion_.Target());
        stopPressMotion_.JumpTo(stopPressMotion_.Target());
        stopStateMotion_.JumpTo(stopStateMotion_.Target());
        return false;
    }
    const auto now = ui::MotionState::Clock::now();
    const bool dragHover = dragHoverMotion_.Advance(now);
    const bool dragPress = dragPressMotion_.Advance(now);
    const bool pauseHover = pauseHoverMotion_.Advance(now);
    const bool pausePress = pausePressMotion_.Advance(now);
    const bool pauseState = pauseStateMotion_.Advance(now);
    const bool stopHover = stopHoverMotion_.Advance(now);
    const bool stopPress = stopPressMotion_.Advance(now);
    const bool stopState = stopStateMotion_.Advance(now);
    return dragHover || dragPress || pauseHover || pausePress || pauseState ||
        stopHover || stopPress || stopState;
}

void RecordingOverlay::ResetMotion() noexcept {
    dragHoverMotion_.JumpTo(0.0F);
    dragPressMotion_.JumpTo(0.0F);
    pauseHoverMotion_.JumpTo(0.0F);
    pausePressMotion_.JumpTo(0.0F);
    pauseStateMotion_.JumpTo(0.0F);
    stopHoverMotion_.JumpTo(0.0F);
    stopPressMotion_.JumpTo(0.0F);
    stopStateMotion_.JumpTo(0.0F);
}

void RecordingOverlay::InvalidateTarget(const HitTarget target) const noexcept {
    if (window_ == nullptr || target == HitTarget::None) {
        return;
    }
    RECT bounds{};
    switch (target) {
    case HitTarget::DragHandle:
        bounds = DragBounds();
        break;
    case HitTarget::Pause:
        bounds = PauseBounds();
        break;
    case HitTarget::Stop:
        bounds = StopBounds();
        break;
    case HitTarget::None:
    default:
        return;
    }
    InflateRect(&bounds, ScaleForDpi(2, dpi_), ScaleForDpi(2, dpi_));
    InvalidateRect(window_, &bounds, FALSE);
}

void RecordingOverlay::InvalidateMotionRegions() const noexcept {
    InvalidateTarget(HitTarget::DragHandle);
    InvalidateTarget(HitTarget::Pause);
    InvalidateTarget(HitTarget::Stop);
    InvalidateStatusRegion();
}

void RecordingOverlay::InvalidateStatusRegion() const noexcept {
    if (window_ == nullptr) {
        return;
    }
    RECT bounds = view::StatusBounds(dpi_);
    InflateRect(&bounds, ScaleForDpi(2, dpi_), ScaleForDpi(1, dpi_));
    InvalidateRect(window_, &bounds, FALSE);
}

RecordingOverlay::HitTarget RecordingOverlay::HitTest(POINT clientPoint) const noexcept {
    const RECT stop = StopBounds();
    if (PtInRect(&stop, clientPoint)) {
        return HitTarget::Stop;
    }
    const RECT pause = PauseBounds();
    if (PtInRect(&pause, clientPoint)) {
        return HitTarget::Pause;
    }
    const RECT drag = DragBounds();
    if (PtInRect(&drag, clientPoint)) {
        return HitTarget::DragHandle;
    }
    return HitTarget::None;
}

void RecordingOverlay::BeginDrag(POINT clientPoint) {
    if (window_ == nullptr) {
        return;
    }
    POINT dragStartScreen = clientPoint;
    RECT dragStartWindow{};
    if (ClientToScreen(window_, &dragStartScreen) == FALSE ||
        GetWindowRect(window_, &dragStartWindow) == FALSE) {
        return;
    }
    dragStartScreen_ = dragStartScreen;
    dragStartWindow_ = dragStartWindow;
    dragging_ = true;
    SetPressedTarget(HitTarget::DragHandle);
    SetCapture(window_);
    if (GetCapture() != window_) {
        dragging_ = false;
        SetPressedTarget(HitTarget::None);
    }
}

void RecordingOverlay::ContinueDrag(POINT screenPoint) {
    if (!dragging_ || window_ == nullptr) {
        return;
    }

    RECT target = dragStartWindow_;
    const int offsetX = screenPoint.x - dragStartScreen_.x;
    const int offsetY = screenPoint.y - dragStartScreen_.y;
    OffsetRect(&target, offsetX, offsetY);
    target = placement::ClampToWorkArea(target);
    SetWindowPos(
        window_,
        HWND_TOPMOST,
        target.left,
        target.top,
        0,
        0,
        SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
    manuallyPositioned_ = true;
}

void RecordingOverlay::EndDrag() {
    if (dragging_ && GetCapture() == window_) {
        ReleaseCapture();
    }
    dragging_ = false;
    SetPressedTarget(HitTarget::None);
}

void RecordingOverlay::Activate(HitTarget target) {
    if (stopping_) {
        return;
    }
    if (target == HitTarget::Pause) {
        const bool nextPaused = !paused_;
        SetPaused(nextPaused);
        const auto callback = callbacks_.pauseChanged;
        if (callback) {
            callback(nextPaused);
        }
        return;
    }
    if (target == HitTarget::Stop) {
        SetStopping(true);
        const auto callback = callbacks_.stopRequested;
        if (callback) {
            callback();
        }
    }
}

void RecordingOverlay::TrackMouseLeave() {
    if (trackingMouse_ || window_ == nullptr) {
        return;
    }
    TRACKMOUSEEVENT tracking{};
    tracking.cbSize = sizeof(tracking);
    tracking.dwFlags = TME_LEAVE;
    tracking.hwndTrack = window_;
    if (TrackMouseEvent(&tracking)) {
        trackingMouse_ = true;
    }
}

void RecordingOverlay::Paint() {
    if (!AdvanceMotion() && window_ != nullptr) {
        KillTimer(window_, MotionTimerId);
    }
    view::VisualState state;
    state.dpi = dpi_;
    state.paused = paused_;
    state.stopping = stopping_;
    state.dragHoverAmount = dragHoverMotion_.Value();
    state.dragPressAmount = dragPressMotion_.Value();
    state.pauseHoverAmount = pauseHoverMotion_.Value();
    state.pausePressAmount = pausePressMotion_.Value();
    state.pauseStateAmount = pauseStateMotion_.Value();
    state.stopHoverAmount = stopHoverMotion_.Value();
    state.stopPressAmount = stopPressMotion_.Value();
    state.stopStateAmount = stopStateMotion_.Value();
    state.bodyFont = bodyFont_;
    state.actionFont = actionFont_;
    state.timerFont = timerFont_;
    state.elapsed = Elapsed();
    view::Paint(window_, state);
}

}  // namespace qrec::overlay
