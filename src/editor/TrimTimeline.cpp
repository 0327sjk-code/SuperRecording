#include "editor/TrimTimeline.h"

#include "editor/EditorTheme.h"
#include "editor/EditorTimeFormat.h"
#include "ui/AntiAliasedDrawing.h"

#include <windowsx.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <string>

namespace qrec {
namespace {

constexpr wchar_t kTimelineClassName[] = L"SuperRecording.TrimTimeline";
constexpr COLORREF kTrackBase = editor_theme::TimelineTrack;
constexpr COLORREF kSelectedFill = editor_theme::TimelineSelected;
constexpr COLORREF kHandleHover = editor_theme::TimelineHandleHover;
constexpr COLORREF kHandlePressed = editor_theme::TimelineHandlePressed;
constexpr COLORREF kTick = editor_theme::TimelineTick;
constexpr COLORREF kDisabledFill = editor_theme::ControlDisabled;
constexpr COLORREF kDisabledInk = editor_theme::TextDisabled;
constexpr UINT_PTR kMotionTimerId = 0x5110;
constexpr UINT_PTR kBoundaryFeedbackTimerId = 0x5111;
constexpr UINT kMotionFrameMilliseconds = 16;
constexpr UINT kBoundaryFeedbackMilliseconds = 120;
constexpr auto kHoverEnterDuration = std::chrono::milliseconds(160);
constexpr auto kHoverExitDuration = std::chrono::milliseconds(120);
constexpr auto kPressEnterDuration = std::chrono::milliseconds(100);
constexpr auto kPressExitDuration = std::chrono::milliseconds(140);
constexpr auto kFocusEnterDuration = std::chrono::milliseconds(180);
constexpr auto kFocusExitDuration = std::chrono::milliseconds(120);

int ScaleForDpi(const UINT dpi, const int value) noexcept {
    return timeline_detail::ScaleDip(dpi, value);
}

float ScaleForDpi(const UINT dpi, const float value) noexcept {
    return value * static_cast<float>(dpi) / static_cast<float>(USER_DEFAULT_SCREEN_DPI);
}

void FillRectangle(HDC dc, const RECT& rectangle, const COLORREF color) {
    const HBRUSH brush = ::CreateSolidBrush(color);
    ::FillRect(dc, &rectangle, brush);
    ::DeleteObject(brush);
}

void FillRoundedRectangle(
    ui::Canvas& canvas,
    const HDC dc,
    const RECT& rectangle,
    const int radius,
    const COLORREF fill,
    const COLORREF border,
    const int borderWidth = 1) {
    if (canvas.Valid()) {
        canvas.DrawRoundedRectangle(
            rectangle,
            static_cast<float>(radius),
            fill,
            border,
            static_cast<float>(std::max(1, borderWidth)));
        return;
    }
    const HBRUSH brush = ::CreateSolidBrush(fill);
    const HPEN pen = ::CreatePen(PS_SOLID, std::max(1, borderWidth), border);
    const HGDIOBJ previousBrush = ::SelectObject(dc, brush);
    const HGDIOBJ previousPen = ::SelectObject(dc, pen);
    ::RoundRect(
        dc,
        rectangle.left,
        rectangle.top,
        rectangle.right,
        rectangle.bottom,
        radius * 2,
        radius * 2);
    ::SelectObject(dc, previousPen);
    ::SelectObject(dc, previousBrush);
    ::DeleteObject(pen);
    ::DeleteObject(brush);
}

void SetTimelineMotionTarget(
    ui::MotionState& motion,
    const bool active,
    const std::chrono::milliseconds enterDuration,
    const std::chrono::milliseconds exitDuration,
    const bool animationsEnabled,
    const ui::MotionEasing easing = ui::MotionEasing::EaseOutQuart) noexcept {
    (void)motion.SetTarget(
        active ? 1.0F : 0.0F,
        active ? enterDuration : exitDuration,
        easing,
        animationsEnabled);
}

}  // namespace

TrimTimeline::~TrimTimeline() {
    Destroy();
}

bool TrimTimeline::RegisterWindowClass(const HINSTANCE instance) {
    WNDCLASSEXW existing{};
    existing.cbSize = sizeof(existing);
    if (::GetClassInfoExW(instance, kTimelineClassName, &existing) != FALSE) {
        return true;
    }

    WNDCLASSEXW windowClass{};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.style = CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS;
    windowClass.lpfnWndProc = &TrimTimeline::WindowProc;
    windowClass.hInstance = instance;
    windowClass.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
    windowClass.lpszClassName = kTimelineClassName;
    return ::RegisterClassExW(&windowClass) != 0 || ::GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
}

bool TrimTimeline::Create(const HWND parent, const int controlId, const HINSTANCE instance) {
    Destroy();
    if (parent == nullptr || !RegisterWindowClass(instance)) {
        return false;
    }

    controlId_ = controlId;
    animationsEnabled_ = ui::ClientAreaAnimationsEnabled();
    window_ = ::CreateWindowExW(
        0,
        kTimelineClassName,
        L"裁剪时间轴",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP,
        0,
        0,
        1,
        1,
        parent,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(controlId)),
        instance,
        this);
    return window_ != nullptr;
}

void TrimTimeline::Destroy() noexcept {
    if (window_ != nullptr && ::IsWindow(window_) != FALSE) {
        if (motionTimerArmed_) {
            ::KillTimer(window_, kMotionTimerId);
        }
        ::KillTimer(window_, kBoundaryFeedbackTimerId);
        ::DestroyWindow(window_);
    }
    ReleaseBackBuffer();
    window_ = nullptr;
    pendingNotification_.reset();
    deferredNotification_.reset();
    notificationMessagePosted_ = false;
    motionTimerArmed_ = false;
    feedbackPart_ = ActivePart::None;
    layoutCacheValid_ = false;
    ResetMotion();
}

void TrimTimeline::SetHoverPart(const ActivePart part) noexcept {
    if (hoverPart_ == part) {
        return;
    }
    hoverPart_ = part;
    SetTimelineMotionTarget(
        startHoverMotion_,
        part == ActivePart::Start,
        kHoverEnterDuration,
        kHoverExitDuration,
        animationsEnabled_);
    SetTimelineMotionTarget(
        endHoverMotion_,
        part == ActivePart::End,
        kHoverEnterDuration,
        kHoverExitDuration,
        animationsEnabled_);
    SetTimelineMotionTarget(
        playheadHoverMotion_,
        part == ActivePart::Playhead,
        kHoverEnterDuration,
        kHoverExitDuration,
        animationsEnabled_);
    UpdateMotionTimer();
}

void TrimTimeline::SetPressedPart(const ActivePart part) noexcept {
    SetTimelineMotionTarget(
        startPressMotion_,
        part == ActivePart::Start,
        kPressEnterDuration,
        kPressExitDuration,
        animationsEnabled_,
        ui::MotionEasing::EaseOutQuint);
    SetTimelineMotionTarget(
        endPressMotion_,
        part == ActivePart::End,
        kPressEnterDuration,
        kPressExitDuration,
        animationsEnabled_,
        ui::MotionEasing::EaseOutQuint);
    SetTimelineMotionTarget(
        playheadPressMotion_,
        part == ActivePart::Playhead,
        kPressEnterDuration,
        kPressExitDuration,
        animationsEnabled_,
        ui::MotionEasing::EaseOutQuint);
    UpdateMotionTimer();
}

void TrimTimeline::SetFocusMotion(const bool focused) noexcept {
    SetTimelineMotionTarget(
        focusMotion_,
        focused,
        kFocusEnterDuration,
        kFocusExitDuration,
        animationsEnabled_,
        ui::MotionEasing::EaseOutQuint);
    UpdateMotionTimer();
}

void TrimTimeline::UpdateMotionTimer() noexcept {
    if (window_ == nullptr) {
        return;
    }
    const bool active = startHoverMotion_.IsActive() || endHoverMotion_.IsActive() ||
        playheadHoverMotion_.IsActive() || startPressMotion_.IsActive() ||
        endPressMotion_.IsActive() || playheadPressMotion_.IsActive() ||
        focusMotion_.IsActive();
    if (active) {
        if (!motionTimerArmed_ &&
            ::SetTimer(window_, kMotionTimerId, kMotionFrameMilliseconds, nullptr) == 0) {
            startHoverMotion_.JumpTo(startHoverMotion_.Target());
            endHoverMotion_.JumpTo(endHoverMotion_.Target());
            playheadHoverMotion_.JumpTo(playheadHoverMotion_.Target());
            startPressMotion_.JumpTo(startPressMotion_.Target());
            endPressMotion_.JumpTo(endPressMotion_.Target());
            playheadPressMotion_.JumpTo(playheadPressMotion_.Target());
            focusMotion_.JumpTo(focusMotion_.Target());
            ::InvalidateRect(window_, nullptr, FALSE);
        } else {
            motionTimerArmed_ = true;
        }
    } else if (motionTimerArmed_) {
        ::KillTimer(window_, kMotionTimerId);
        motionTimerArmed_ = false;
    }
}

bool TrimTimeline::AdvanceMotion() noexcept {
    if (!animationsEnabled_) {
        startHoverMotion_.JumpTo(startHoverMotion_.Target());
        endHoverMotion_.JumpTo(endHoverMotion_.Target());
        playheadHoverMotion_.JumpTo(playheadHoverMotion_.Target());
        startPressMotion_.JumpTo(startPressMotion_.Target());
        endPressMotion_.JumpTo(endPressMotion_.Target());
        playheadPressMotion_.JumpTo(playheadPressMotion_.Target());
        focusMotion_.JumpTo(focusMotion_.Target());
        return false;
    }
    const auto now = ui::MotionState::Clock::now();
    const bool startHover = startHoverMotion_.Advance(now);
    const bool endHover = endHoverMotion_.Advance(now);
    const bool playheadHover = playheadHoverMotion_.Advance(now);
    const bool startPress = startPressMotion_.Advance(now);
    const bool endPress = endPressMotion_.Advance(now);
    const bool playheadPress = playheadPressMotion_.Advance(now);
    const bool focus = focusMotion_.Advance(now);
    return startHover || endHover || playheadHover || startPress || endPress ||
        playheadPress || focus;
}

void TrimTimeline::ResetMotion() noexcept {
    hoverPart_ = ActivePart::None;
    startHoverMotion_.JumpTo(0.0F);
    endHoverMotion_.JumpTo(0.0F);
    playheadHoverMotion_.JumpTo(0.0F);
    startPressMotion_.JumpTo(0.0F);
    endPressMotion_.JumpTo(0.0F);
    playheadPressMotion_.JumpTo(0.0F);
    focusMotion_.JumpTo(0.0F);
}

void TrimTimeline::SetRange(
    const std::chrono::milliseconds duration,
    const std::chrono::milliseconds trimStart,
    const std::chrono::milliseconds trimEnd) {
    duration_ = std::max(duration, std::chrono::milliseconds(1));
    trimStart_ = std::clamp(trimStart, std::chrono::milliseconds(0), duration_);
    trimEnd_ = std::clamp(trimEnd, trimStart_, duration_);
    if (trimEnd_ <= trimStart_) {
        trimEnd_ = std::min(duration_, trimStart_ + std::chrono::milliseconds(1));
    }
    playhead_ = std::clamp(
        playhead_,
        std::chrono::milliseconds::zero(),
        duration_);
    if (window_ != nullptr) {
        ::InvalidateRect(window_, nullptr, FALSE);
    }
}

void TrimTimeline::SetPlayhead(const std::chrono::milliseconds position) {
    const auto clamped = std::clamp(
        position,
        std::chrono::milliseconds::zero(),
        duration_);
    if (clamped == playhead_) {
        return;
    }
    playhead_ = clamped;
    if (window_ != nullptr) {
        ::InvalidateRect(window_, nullptr, FALSE);
    }
}

bool TrimTimeline::CommitTrimStart(const std::chrono::milliseconds position) {
    const auto minimumSpan = std::min(duration_, std::chrono::milliseconds(100));
    const auto candidate = std::clamp(
        position,
        std::chrono::milliseconds::zero(),
        duration_);
    if (candidate > trimEnd_ - minimumSpan) {
        return false;
    }
    activePart_ = ActivePart::Start;
    playhead_ = candidate;
    if (candidate == trimStart_) {
        ShowBoundaryFeedback(ActivePart::Start);
        return false;
    }
    trimStart_ = candidate;
    ShowBoundaryFeedback(ActivePart::Start);
    if (window_ != nullptr) {
        ::InvalidateRect(window_, nullptr, FALSE);
    }
    QueueNotification(
        TimelineRangeChanged,
        TimelineInteractionPhase::Committed,
        playhead_);
    return true;
}

bool TrimTimeline::CommitTrimEnd(const std::chrono::milliseconds position) {
    const auto minimumSpan = std::min(duration_, std::chrono::milliseconds(100));
    const auto candidate = std::clamp(
        position,
        std::chrono::milliseconds::zero(),
        duration_);
    if (candidate < trimStart_ + minimumSpan) {
        return false;
    }
    activePart_ = ActivePart::End;
    playhead_ = candidate;
    if (candidate == trimEnd_) {
        ShowBoundaryFeedback(ActivePart::End);
        return false;
    }
    trimEnd_ = candidate;
    ShowBoundaryFeedback(ActivePart::End);
    if (window_ != nullptr) {
        ::InvalidateRect(window_, nullptr, FALSE);
    }
    QueueNotification(
        TimelineRangeChanged,
        TimelineInteractionPhase::Committed,
        playhead_);
    return true;
}

void TrimTimeline::SetEnabled(const bool enabled) {
    if (!enabled) {
        CommitKeyboardAdjustment();
    }
    if (!enabled && dragging_) {
        const ActivePart committedPart = dragPart_;
        dragging_ = false;
        dragPart_ = ActivePart::None;
        dragAnchorOffsetX_ = 0;
        boundarySnap_ = BoundarySnap::None;
        QueueNotificationForPart(committedPart, TimelineInteractionPhase::Committed);
        if (::GetCapture() == window_) {
            ::ReleaseCapture();
        }
    }
    if (!enabled) {
        if (window_ != nullptr) {
            ::KillTimer(window_, kBoundaryFeedbackTimerId);
        }
        feedbackPart_ = ActivePart::None;
        ResetMotion();
        if (window_ != nullptr && motionTimerArmed_) {
            ::KillTimer(window_, kMotionTimerId);
            motionTimerArmed_ = false;
        }
    }
    enabled_ = enabled;
    if (window_ != nullptr) {
        ::EnableWindow(window_, enabled ? TRUE : FALSE);
        ::InvalidateRect(window_, nullptr, FALSE);
    }
}

bool TrimTimeline::ConsumePendingNotification(
    TimelineNotification* const notification) noexcept {
    if (notification == nullptr || !pendingNotification_.has_value()) {
        return false;
    }
    *notification = *pendingNotification_;
    pendingNotification_.reset();
    notificationMessagePosted_ = false;
    if (deferredNotification_.has_value()) {
        pendingNotification_ = *deferredNotification_;
        deferredNotification_.reset();
        PostPendingNotificationMessage();
    }
    return true;
}

std::chrono::milliseconds TrimTimeline::TrimStart() const noexcept {
    return trimStart_;
}

std::chrono::milliseconds TrimTimeline::TrimEnd() const noexcept {
    return trimEnd_;
}

LRESULT CALLBACK TrimTimeline::WindowProc(
    const HWND window,
    const UINT message,
    const WPARAM wParam,
    const LPARAM lParam) {
    TrimTimeline* self = reinterpret_cast<TrimTimeline*>(
        ::GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lParam);
        self = static_cast<TrimTimeline*>(create->lpCreateParams);
        self->window_ = window;
        ::SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    }
    if (self != nullptr) {
        return self->HandleMessage(message, wParam, lParam);
    }
    return ::DefWindowProcW(window, message, wParam, lParam);
}

LRESULT TrimTimeline::HandleMessage(
    const UINT message,
    const WPARAM wParam,
    const LPARAM lParam) {
    switch (message) {
    case WM_SIZE:
        UpdateLayoutCache(
            std::max(1, static_cast<int>(LOWORD(lParam))),
            std::max(1, static_cast<int>(HIWORD(lParam))),
            window_ != nullptr ? ::GetDpiForWindow(window_) : USER_DEFAULT_SCREEN_DPI);
        ::InvalidateRect(window_, nullptr, FALSE);
        return 0;
#ifdef WM_DPICHANGED_AFTERPARENT
    case WM_DPICHANGED_AFTERPARENT:
#endif
    case WM_DPICHANGED: {
        RECT client{};
        ::GetClientRect(window_, &client);
        UpdateLayoutCache(
            std::max(1, static_cast<int>(client.right - client.left)),
            std::max(1, static_cast<int>(client.bottom - client.top)),
            window_ != nullptr ? ::GetDpiForWindow(window_) : USER_DEFAULT_SCREEN_DPI);
        ::InvalidateRect(window_, nullptr, FALSE);
        return 0;
    }
    case WM_SETTINGCHANGE:
        animationsEnabled_ = ui::RefreshClientAreaAnimationsEnabled();
        UpdateMotionTimer();
        ::InvalidateRect(window_, nullptr, FALSE);
        break;
    case WM_SETFONT:
        font_ = reinterpret_cast<HFONT>(wParam);
        if (lParam != 0) {
            ::InvalidateRect(window_, nullptr, FALSE);
        }
        return 0;
    case WM_GETFONT:
        return reinterpret_cast<LRESULT>(font_);
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT:
        Paint();
        return 0;
    case WM_SETFOCUS:
        SetFocusMotion(true);
        ::InvalidateRect(window_, nullptr, FALSE);
        return 0;
    case WM_KILLFOCUS:
        CommitKeyboardAdjustment();
        SetFocusMotion(false);
        ::InvalidateRect(window_, nullptr, FALSE);
        return 0;
    case WM_GETDLGCODE:
        return static_cast<LRESULT>(DLGC_WANTARROWS);
    case WM_SETCURSOR: {
        POINT point{};
        ::GetCursorPos(&point);
        ::ScreenToClient(window_, &point);
        const ActivePart hit = HitTestPart(point);
        ::SetCursor(::LoadCursorW(
            nullptr, hit != ActivePart::None ? IDC_SIZEWE : IDC_HAND));
        return TRUE;
    }
    case WM_LBUTTONDOWN: {
        if (!enabled_) {
            return 0;
        }
        if (feedbackPart_ != ActivePart::None) {
            ::KillTimer(window_, kBoundaryFeedbackTimerId);
            feedbackPart_ = ActivePart::None;
            startPressMotion_.JumpTo(0.0F);
            endPressMotion_.JumpTo(0.0F);
        }
        ::SetFocus(window_);
        const POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        ActivePart hit = HitTestPart(point);
        const bool clickedExistingPart = hit != ActivePart::None;
        if (hit == ActivePart::None) {
            hit = ActivePart::Playhead;
        }
        if (hit == ActivePart::Start || hit == ActivePart::End) {
            activePart_ = hit;
        }
        dragPart_ = hit;
        dragAnchorOffsetX_ = clickedExistingPart ? point.x - PartCenterX(hit) : 0;
        if (hit == ActivePart::Start && trimStart_ == std::chrono::milliseconds::zero()) {
            boundarySnap_ = BoundarySnap::Start;
        } else if (hit == ActivePart::End && trimEnd_ == duration_) {
            boundarySnap_ = BoundarySnap::End;
        } else {
            boundarySnap_ = BoundarySnap::None;
        }
        ::SetCapture(window_);
        dragging_ = ::GetCapture() == window_;
        SetPressedPart(dragging_ ? hit : ActivePart::None);
        UpdateFromPointer(point.x - dragAnchorOffsetX_, true);
        if (!dragging_) {
            QueueNotificationForPart(hit, TimelineInteractionPhase::Committed);
            dragPart_ = ActivePart::None;
            dragAnchorOffsetX_ = 0;
            boundarySnap_ = BoundarySnap::None;
        }
        return 0;
    }
    case WM_MOUSEMOVE: {
        if (dragging_ && enabled_ && ::GetCapture() == window_) {
            UpdateFromPointer(GET_X_LPARAM(lParam) - dragAnchorOffsetX_, true);
        } else {
            const POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            const ActivePart nextHover = HitTestPart(point);
            if (nextHover != hoverPart_) {
                SetHoverPart(nextHover);
                ::InvalidateRect(window_, nullptr, FALSE);
            }
        }
        if (!trackingMouse_) {
            TRACKMOUSEEVENT tracking{};
            tracking.cbSize = sizeof(tracking);
            tracking.dwFlags = TME_LEAVE;
            tracking.hwndTrack = window_;
            trackingMouse_ = ::TrackMouseEvent(&tracking) != FALSE;
            if (!trackingMouse_) {
                SetHoverPart(ActivePart::None);
                ::InvalidateRect(window_, nullptr, FALSE);
            }
        }
        return 0;
    }
    case WM_MOUSELEAVE:
        trackingMouse_ = false;
        SetHoverPart(ActivePart::None);
        ::InvalidateRect(window_, nullptr, FALSE);
        return 0;
    case WM_LBUTTONUP:
        if (dragging_) {
            const ActivePart committedPart = dragPart_;
            UpdateFromPointer(GET_X_LPARAM(lParam) - dragAnchorOffsetX_, true);
            dragging_ = false;
            dragPart_ = ActivePart::None;
            dragAnchorOffsetX_ = 0;
            boundarySnap_ = BoundarySnap::None;
            SetPressedPart(ActivePart::None);
            QueueNotificationForPart(committedPart, TimelineInteractionPhase::Committed);
            if (::GetCapture() == window_) {
                ::ReleaseCapture();
            }
            ::InvalidateRect(window_, nullptr, FALSE);
        }
        return 0;
    case WM_CAPTURECHANGED:
        if (dragging_ || dragPart_ != ActivePart::None) {
            const ActivePart committedPart = dragPart_;
            dragging_ = false;
            dragPart_ = ActivePart::None;
            dragAnchorOffsetX_ = 0;
            boundarySnap_ = BoundarySnap::None;
            SetPressedPart(ActivePart::None);
            QueueNotificationForPart(committedPart, TimelineInteractionPhase::Committed);
            ::InvalidateRect(window_, nullptr, FALSE);
        }
        return 0;
    case WM_CANCELMODE:
        CommitKeyboardAdjustment();
        if (dragging_ || dragPart_ != ActivePart::None) {
            const ActivePart committedPart = dragPart_;
            dragging_ = false;
            dragPart_ = ActivePart::None;
            dragAnchorOffsetX_ = 0;
            boundarySnap_ = BoundarySnap::None;
            SetPressedPart(ActivePart::None);
            QueueNotificationForPart(committedPart, TimelineInteractionPhase::Committed);
            if (::GetCapture() == window_) {
                ::ReleaseCapture();
            }
            ::InvalidateRect(window_, nullptr, FALSE);
        }
        return 0;
    case WM_TIMER:
        if (wParam == kMotionTimerId) {
            const bool active = AdvanceMotion();
            ::InvalidateRect(window_, nullptr, FALSE);
            if (!active) {
                ::KillTimer(window_, kMotionTimerId);
                motionTimerArmed_ = false;
            }
            return 0;
        }
        if (wParam == kBoundaryFeedbackTimerId) {
            ::KillTimer(window_, kBoundaryFeedbackTimerId);
            feedbackPart_ = ActivePart::None;
            if (!dragging_) {
                startPressMotion_.JumpTo(0.0F);
                endPressMotion_.JumpTo(0.0F);
                ::InvalidateRect(window_, nullptr, FALSE);
            }
            return 0;
        }
        break;
    case WM_KEYDOWN: {
        if (!enabled_) {
            break;
        }
        if (wParam == VK_UP || wParam == VK_HOME) {
            activePart_ = ActivePart::Start;
            ::InvalidateRect(window_, nullptr, FALSE);
            return 0;
        }
        if (wParam == VK_DOWN || wParam == VK_END) {
            activePart_ = ActivePart::End;
            ::InvalidateRect(window_, nullptr, FALSE);
            return 0;
        }
        if (wParam != VK_LEFT && wParam != VK_RIGHT) {
            break;
        }
        if (activePart_ == ActivePart::None) {
            activePart_ = ActivePart::Start;
        }
        const bool coarse = (::GetKeyState(VK_CONTROL) & 0x8000) != 0;
        const auto step = coarse ? std::chrono::milliseconds(1000)
                                 : std::chrono::milliseconds(100);
        const auto delta = wParam == VK_LEFT ? -step : step;
        const auto minimumSpan = std::min(duration_, std::chrono::milliseconds(100));
        const auto previousStart = trimStart_;
        const auto previousEnd = trimEnd_;
        const auto previousPlayhead = playhead_;
        if (activePart_ == ActivePart::Start) {
            trimStart_ = std::clamp(
                trimStart_ + delta,
                std::chrono::milliseconds(0),
                trimEnd_ - minimumSpan);
            playhead_ = trimStart_;
        } else {
            trimEnd_ = std::clamp(
                trimEnd_ + delta,
                trimStart_ + minimumSpan,
                duration_);
            playhead_ = trimEnd_;
        }
        if (trimStart_ == previousStart && trimEnd_ == previousEnd &&
            playhead_ == previousPlayhead) {
            return 0;
        }
        keyboardAdjusting_ = true;
        ::InvalidateRect(window_, nullptr, FALSE);
        QueueNotification(
            TimelineRangeChanged,
            TimelineInteractionPhase::Preview,
            playhead_);
        return 0;
    }
    case WM_KEYUP:
        if ((wParam == VK_LEFT || wParam == VK_RIGHT) && keyboardAdjusting_) {
            CommitKeyboardAdjustment();
            return 0;
        }
        break;
    case WM_NCDESTROY: {
        const HWND destroyedWindow = window_;
        if (motionTimerArmed_) {
            ::KillTimer(destroyedWindow, kMotionTimerId);
        }
        ::KillTimer(destroyedWindow, kBoundaryFeedbackTimerId);
        ReleaseBackBuffer();
        ::SetWindowLongPtrW(destroyedWindow, GWLP_USERDATA, 0);
        trackingMouse_ = false;
        dragging_ = false;
        keyboardAdjusting_ = false;
        motionTimerArmed_ = false;
        feedbackPart_ = ActivePart::None;
        layoutCacheValid_ = false;
        pendingNotification_.reset();
        deferredNotification_.reset();
        notificationMessagePosted_ = false;
        window_ = nullptr;
        return ::DefWindowProcW(destroyedWindow, message, wParam, lParam);
    }
    default:
        break;
    }
    return ::DefWindowProcW(window_, message, wParam, lParam);
}

void TrimTimeline::EnsureLayoutCache() noexcept {
    if (layoutCacheValid_ || window_ == nullptr) {
        return;
    }
    RECT client{};
    ::GetClientRect(window_, &client);
    UpdateLayoutCache(
        std::max(1, static_cast<int>(client.right - client.left)),
        std::max(1, static_cast<int>(client.bottom - client.top)),
        ::GetDpiForWindow(window_));
}

void TrimTimeline::UpdateLayoutCache(
    const int width,
    const int height,
    const UINT dpi) noexcept {
    const int safeWidth = std::max(1, width);
    const int safeHeight = std::max(1, height);
    const UINT safeDpi = dpi == 0 ? USER_DEFAULT_SCREEN_DPI : dpi;
    if (layoutCacheValid_ && cachedClientWidth_ == safeWidth &&
        cachedClientHeight_ == safeHeight && cachedDpi_ == safeDpi) {
        return;
    }
    ReleaseBackBuffer();
    cachedClientWidth_ = safeWidth;
    cachedClientHeight_ = safeHeight;
    cachedDpi_ = safeDpi;
    cachedTrack_ = timeline_detail::BuildTrackGeometry(
        cachedClientWidth_, cachedClientHeight_, cachedDpi_);
    layoutCacheValid_ = true;
}

void TrimTimeline::ReleaseBackBuffer() noexcept {
    if (backBufferDc_ != nullptr && backBufferPreviousBitmap_ != nullptr) {
        ::SelectObject(backBufferDc_, backBufferPreviousBitmap_);
    }
    backBufferPreviousBitmap_ = nullptr;
    if (backBufferBitmap_ != nullptr) {
        ::DeleteObject(backBufferBitmap_);
        backBufferBitmap_ = nullptr;
    }
    if (backBufferDc_ != nullptr) {
        ::DeleteDC(backBufferDc_);
        backBufferDc_ = nullptr;
    }
}

bool TrimTimeline::EnsureBackBuffer(const HDC targetDc) noexcept {
    EnsureLayoutCache();
    if (backBufferDc_ != nullptr && backBufferBitmap_ != nullptr) {
        return true;
    }
    if (targetDc == nullptr) {
        return false;
    }

    const HDC candidateDc = ::CreateCompatibleDC(targetDc);
    if (candidateDc == nullptr) {
        return false;
    }
    const HBITMAP candidateBitmap = ::CreateCompatibleBitmap(
        targetDc, cachedClientWidth_, cachedClientHeight_);
    if (candidateBitmap == nullptr) {
        ::DeleteDC(candidateDc);
        return false;
    }
    const HGDIOBJ previousBitmap = ::SelectObject(candidateDc, candidateBitmap);
    if (previousBitmap == nullptr || previousBitmap == HGDI_ERROR) {
        ::DeleteObject(candidateBitmap);
        ::DeleteDC(candidateDc);
        return false;
    }
    backBufferDc_ = candidateDc;
    backBufferBitmap_ = candidateBitmap;
    backBufferPreviousBitmap_ = previousBitmap;
    return true;
}

void TrimTimeline::Paint() {
    PAINTSTRUCT paint{};
    const HDC targetDc = ::BeginPaint(window_, &paint);
    EnsureLayoutCache();
    const RECT client{0, 0, cachedClientWidth_, cachedClientHeight_};
    const int width = cachedClientWidth_;
    const int height = cachedClientHeight_;
    if (!EnsureBackBuffer(targetDc)) {
        FillRectangle(targetDc, client, editor_theme::Panel);
        ::EndPaint(window_, &paint);
        return;
    }
    const HDC dc = backBufferDc_;
    FillRectangle(dc, client, editor_theme::Panel);
    ui::Canvas canvas(dc);
    const int startX = TimeToX(trimStart_);
    const int endX = TimeToX(trimEnd_);
    const int playheadX = TimeToX(playhead_);
    const timeline_detail::TimelineGeometry geometry =
        timeline_detail::BuildTimelineGeometry(
            cachedTrack_,
            height,
            cachedDpi_,
            startX,
            endX,
            playheadX);
    const RECT track = geometry.track.bounds;
    const int trackLeft = track.left;
    const int trackRight = track.right;
    const int trackTop = track.top;
    const int centerY = geometry.track.centerY;
    const int trackRadius = ScaleForDpi(cachedDpi_, 7);
    FillRoundedRectangle(
        canvas,
        dc,
        track,
        trackRadius,
        enabled_ ? kTrackBase : kDisabledFill,
        enabled_ ? editor_theme::BorderSubtle : editor_theme::BorderDisabled);

    const HFONT labelFont = font_ != nullptr
        ? font_
        : static_cast<HFONT>(::GetStockObject(DEFAULT_GUI_FONT));
    const HGDIOBJ previousFont = ::SelectObject(dc, labelFont);
    ::SetBkMode(dc, TRANSPARENT);
    ::SetTextColor(dc, enabled_ ? editor_theme::TextSecondary : kDisabledInk);
    const std::wstring startLabel = L"起点  " + FormatEditorTime(trimStart_);
    const std::wstring endLabel = L"终点  " + FormatEditorTime(trimEnd_);
    RECT startLabelBounds{
        trackLeft + ScaleForDpi(cachedDpi_, 14),
        0,
        width / 2 - ScaleForDpi(cachedDpi_, 8),
        trackTop - ScaleForDpi(cachedDpi_, 5)};
    RECT endLabelBounds{
        width / 2 + ScaleForDpi(cachedDpi_, 8),
        0,
        trackRight - ScaleForDpi(cachedDpi_, 14),
        trackTop - ScaleForDpi(cachedDpi_, 5)};
    ::DrawTextW(
        dc,
        startLabel.c_str(),
        static_cast<int>(startLabel.size()),
        &startLabelBounds,
        DT_LEFT | DT_BOTTOM | DT_SINGLELINE | DT_NOPREFIX);
    ::DrawTextW(
        dc,
        endLabel.c_str(),
        static_cast<int>(endLabel.size()),
        &endLabelBounds,
        DT_RIGHT | DT_BOTTOM | DT_SINGLELINE | DT_NOPREFIX);

    RECT selected{
        startX,
        track.top,
        std::max(startX + ScaleForDpi(cachedDpi_, 2), endX),
        track.bottom};
    FillRoundedRectangle(
        canvas,
        dc,
        selected,
        trackRadius,
        enabled_ ? kSelectedFill : kDisabledFill,
        enabled_ ? editor_theme::TimelineSelectedBorder : kDisabledInk,
        ScaleForDpi(cachedDpi_, 1));

    if (enabled_) {
        const HPEN tickPen = ::CreatePen(PS_SOLID, 1, kTick);
        const HGDIOBJ previousPen = ::SelectObject(dc, tickPen);
        for (int index = 1; index < 10; ++index) {
            const int tickX = trackLeft + (trackRight - trackLeft) * index / 10;
            if (tickX > startX + ScaleForDpi(cachedDpi_, 8) &&
                tickX < endX - ScaleForDpi(cachedDpi_, 8)) {
                ::MoveToEx(dc, tickX, track.top + ScaleForDpi(cachedDpi_, 5), nullptr);
                ::LineTo(dc, tickX, track.bottom - ScaleForDpi(cachedDpi_, 5));
            }
        }
        ::SelectObject(dc, previousPen);
        ::DeleteObject(tickPen);
    }

    const RECT startHandle = geometry.startHandle;
    const RECT endHandle = geometry.endHandle;

    struct HandleColors final {
        COLORREF fill{};
        COLORREF border{};
        COLORREF grip{};
    };
    const auto hoverAmount = [this](const ActivePart part) noexcept -> float {
        switch (part) {
        case ActivePart::Start:
            return startHoverMotion_.Value();
        case ActivePart::End:
            return endHoverMotion_.Value();
        case ActivePart::Playhead:
            return playheadHoverMotion_.Value();
        case ActivePart::None:
        default:
            return 0.0F;
        }
    };
    const auto pressAmount = [this](const ActivePart part) noexcept -> float {
        switch (part) {
        case ActivePart::Start:
            return startPressMotion_.Value();
        case ActivePart::End:
            return endPressMotion_.Value();
        case ActivePart::Playhead:
            return playheadPressMotion_.Value();
        case ActivePart::None:
        default:
            return 0.0F;
        }
    };
    const auto handleColors = [this, &hoverAmount, &pressAmount](
                                  const ActivePart part) noexcept -> HandleColors {
        if (!enabled_) {
            return {kDisabledFill, editor_theme::BorderDisabled, kDisabledInk};
        }
        const float focusedAmount = activePart_ == part
            ? focusMotion_.Value() * 0.65F
            : 0.0F;
        const float hotAmount = std::max(hoverAmount(part), focusedAmount);
        const float downAmount = pressAmount(part);
        COLORREF fill = ui::InterpolateColor(
            editor_theme::TimelineHandle,
            kHandleHover,
            hotAmount);
        fill = ui::InterpolateColor(fill, kHandlePressed, downAmount);
        COLORREF border = ui::InterpolateColor(
            editor_theme::TimelineSelectedBorder,
            editor_theme::Focus,
            hotAmount);
        border = ui::InterpolateColor(
            border,
            editor_theme::FocusPressed,
            downAmount);
        COLORREF grip = editor_theme::TimelineHandleGrip;
        grip = ui::InterpolateColor(grip, editor_theme::White, downAmount);
        return {fill, border, grip};
    };
    const auto drawHandle = [&](const RECT& bounds, const int handleCenter, const ActivePart part) {
        const HandleColors colors = handleColors(part);
        FillRoundedRectangle(
            canvas,
            dc,
            bounds,
            ScaleForDpi(cachedDpi_, 6),
            colors.fill,
            colors.border,
            ScaleForDpi(cachedDpi_, 1));
        const float gripWidth = std::max(1.0F, ScaleForDpi(cachedDpi_, 1.25F));
        canvas.DrawLine(
            static_cast<float>(handleCenter - ScaleForDpi(cachedDpi_, 2)),
            static_cast<float>(centerY - ScaleForDpi(cachedDpi_, 5)),
            static_cast<float>(handleCenter - ScaleForDpi(cachedDpi_, 2)),
            static_cast<float>(centerY + ScaleForDpi(cachedDpi_, 5)),
            colors.grip,
            gripWidth);
        canvas.DrawLine(
            static_cast<float>(handleCenter + ScaleForDpi(cachedDpi_, 2)),
            static_cast<float>(centerY - ScaleForDpi(cachedDpi_, 5)),
            static_cast<float>(handleCenter + ScaleForDpi(cachedDpi_, 2)),
            static_cast<float>(centerY + ScaleForDpi(cachedDpi_, 5)),
            colors.grip,
            gripWidth);
    };
    drawHandle(startHandle, startX, ActivePart::Start);
    drawHandle(endHandle, endX, ActivePart::End);

    const float playheadHoverAmount = hoverAmount(ActivePart::Playhead);
    const float playheadPressAmount = pressAmount(ActivePart::Playhead);
    COLORREF playheadColor = enabled_
        ? ui::InterpolateColor(
            editor_theme::TimelinePlayhead,
            editor_theme::Focus,
            playheadHoverAmount)
        : kDisabledInk;
    if (enabled_) {
        playheadColor = ui::InterpolateColor(
            playheadColor,
            editor_theme::FocusPressed,
            playheadPressAmount);
    }
    const int playheadTop = geometry.playheadTop;
    const int playheadBottom = geometry.playheadBottom;
    canvas.DrawLine(
        static_cast<float>(playheadX),
        static_cast<float>(playheadTop),
        static_cast<float>(playheadX),
        static_cast<float>(playheadBottom),
        editor_theme::TimelinePlayheadHalo,
        std::max(1.0F, ScaleForDpi(cachedDpi_, 3.0F)));
    canvas.DrawLine(
        static_cast<float>(playheadX),
        static_cast<float>(playheadTop),
        static_cast<float>(playheadX),
        static_cast<float>(playheadBottom),
        playheadColor,
        std::max(
            1.0F,
            ScaleForDpi(cachedDpi_, 1.5F + playheadPressAmount * 0.5F)));
    const std::array<POINT, 3> markerOutline{{
        {playheadX - ScaleForDpi(cachedDpi_, 5), playheadTop - ScaleForDpi(cachedDpi_, 1)},
        {playheadX + ScaleForDpi(cachedDpi_, 5), playheadTop - ScaleForDpi(cachedDpi_, 1)},
        {playheadX, playheadTop + ScaleForDpi(cachedDpi_, 6)}}};
    canvas.FillPolygon(markerOutline, editor_theme::TimelinePlayheadHalo);
    const std::array<POINT, 3> marker{{
        {playheadX - ScaleForDpi(cachedDpi_, 3), playheadTop},
        {playheadX + ScaleForDpi(cachedDpi_, 3), playheadTop},
        {playheadX, playheadTop + ScaleForDpi(cachedDpi_, 4)}}};
    canvas.FillPolygon(marker, playheadColor);

    if (focusMotion_.Value() > 0.001F) {
        RECT focus = client;
        ::InflateRect(&focus, -ScaleForDpi(cachedDpi_, 1), -ScaleForDpi(cachedDpi_, 1));
        const COLORREF focusColor = ui::InterpolateColor(
            editor_theme::Panel,
            editor_theme::Focus,
            focusMotion_.Value());
        canvas.StrokeRoundedRectangle(
            focus,
            static_cast<float>(ScaleForDpi(cachedDpi_, 4)),
            focusColor,
            static_cast<float>(std::max(1, ScaleForDpi(cachedDpi_, 2))));
    }

    ::SelectObject(dc, previousFont);
    RECT blitBounds = paint.rcPaint;
    if (::IsRectEmpty(&blitBounds) != FALSE) {
        blitBounds = client;
    }
    ::BitBlt(
        targetDc,
        blitBounds.left,
        blitBounds.top,
        blitBounds.right - blitBounds.left,
        blitBounds.bottom - blitBounds.top,
        dc,
        blitBounds.left,
        blitBounds.top,
        SRCCOPY);
    ::EndPaint(window_, &paint);
}

void TrimTimeline::UpdateFromPointer(const int x, const bool notify) {
    const auto previousStart = trimStart_;
    const auto previousEnd = trimEnd_;
    const auto previousPlayhead = playhead_;
    auto value = XToTime(x);

    if (dragPart_ == ActivePart::Playhead) {
        boundarySnap_ = BoundarySnap::None;
        playhead_ = std::clamp(
            value,
            std::chrono::milliseconds::zero(),
            duration_);
        if (playhead_ == previousPlayhead) {
            return;
        }
        ::InvalidateRect(window_, nullptr, FALSE);
        if (notify) {
            QueueNotification(
                TimelineSeekRequested,
                TimelineInteractionPhase::Preview,
                playhead_);
        }
        return;
    }

    const auto minimumSpan = std::min(duration_, std::chrono::milliseconds(100));
    EnsureLayoutCache();
    const int enterSnapDistance = std::max(1, ScaleForDpi(cachedDpi_, 8));
    const int exitSnapDistance = std::max(enterSnapDistance, ScaleForDpi(cachedDpi_, 12));

    if (dragPart_ == ActivePart::Start) {
        const int distance = std::abs(x - cachedTrack_.bounds.left);
        const bool snapped = boundarySnap_ == BoundarySnap::Start
            ? distance <= exitSnapDistance
            : distance <= enterSnapDistance;
        boundarySnap_ = snapped ? BoundarySnap::Start : BoundarySnap::None;
        if (snapped) {
            value = std::chrono::milliseconds::zero();
        }
        trimStart_ = std::clamp(value, std::chrono::milliseconds(0), trimEnd_ - minimumSpan);
        playhead_ = trimStart_;
    } else if (dragPart_ == ActivePart::End) {
        const int distance = std::abs(x - cachedTrack_.bounds.right);
        const bool snapped = boundarySnap_ == BoundarySnap::End
            ? distance <= exitSnapDistance
            : distance <= enterSnapDistance;
        boundarySnap_ = snapped ? BoundarySnap::End : BoundarySnap::None;
        if (snapped) {
            value = duration_;
        }
        trimEnd_ = std::clamp(value, trimStart_ + minimumSpan, duration_);
        playhead_ = trimEnd_;
    } else {
        return;
    }
    if (trimStart_ == previousStart && trimEnd_ == previousEnd &&
        playhead_ == previousPlayhead) {
        return;
    }
    ::InvalidateRect(window_, nullptr, FALSE);
    if (notify) {
        QueueNotification(
            TimelineRangeChanged,
            TimelineInteractionPhase::Preview,
            playhead_);
    }
}

void TrimTimeline::QueueNotification(
    const UINT code,
    const TimelineInteractionPhase phase,
    const std::chrono::milliseconds seekPosition) {
    TimelineNotification notification{};
    notification.header.hwndFrom = window_;
    notification.header.idFrom = static_cast<UINT_PTR>(controlId_);
    notification.header.code = code;
    notification.phase = phase;
    notification.trimStart = trimStart_;
    notification.trimEnd = trimEnd_;
    notification.seekPosition = seekPosition;
    if (pendingNotification_.has_value() &&
        pendingNotification_->phase == TimelineInteractionPhase::Committed &&
        phase == TimelineInteractionPhase::Preview) {
        deferredNotification_ = notification;
        return;
    }
    if (phase == TimelineInteractionPhase::Committed) {
        deferredNotification_.reset();
    }
    pendingNotification_ = notification;
    PostPendingNotificationMessage();
}

void TrimTimeline::PostPendingNotificationMessage() noexcept {
    if (notificationMessagePosted_ || !pendingNotification_.has_value() ||
        window_ == nullptr) {
        return;
    }
    const HWND parent = ::GetParent(window_);
    if (parent == nullptr) {
        return;
    }
    notificationMessagePosted_ = ::PostMessageW(
        parent,
        TimelineInteractionMessage,
        static_cast<WPARAM>(controlId_),
        0) != FALSE;
}

void TrimTimeline::QueueNotificationForPart(
    const ActivePart part,
    const TimelineInteractionPhase phase) {
    if (part == ActivePart::Playhead) {
        QueueNotification(TimelineSeekRequested, phase, playhead_);
    } else if (part == ActivePart::Start || part == ActivePart::End) {
        QueueNotification(TimelineRangeChanged, phase, playhead_);
    }
}

void TrimTimeline::CommitKeyboardAdjustment() {
    if (!keyboardAdjusting_) {
        return;
    }
    keyboardAdjusting_ = false;
    QueueNotification(
        TimelineRangeChanged,
        TimelineInteractionPhase::Committed,
        playhead_);
}

void TrimTimeline::ShowBoundaryFeedback(const ActivePart part) noexcept {
    if (window_ == nullptr ||
        (part != ActivePart::Start && part != ActivePart::End)) {
        return;
    }
    ::KillTimer(window_, kBoundaryFeedbackTimerId);
    feedbackPart_ = part;
    startPressMotion_.JumpTo(part == ActivePart::Start ? 1.0F : 0.0F);
    endPressMotion_.JumpTo(part == ActivePart::End ? 1.0F : 0.0F);
    if (::SetTimer(
            window_,
            kBoundaryFeedbackTimerId,
            kBoundaryFeedbackMilliseconds,
            nullptr) == 0) {
        feedbackPart_ = ActivePart::None;
        startPressMotion_.JumpTo(0.0F);
        endPressMotion_.JumpTo(0.0F);
    }
    ::InvalidateRect(window_, nullptr, FALSE);
}

int TrimTimeline::TimeToX(const std::chrono::milliseconds value) noexcept {
    if (window_ == nullptr) {
        return 0;
    }
    EnsureLayoutCache();
    const int left = cachedTrack_.bounds.left;
    const int right = cachedTrack_.bounds.right;
    const double ratio = std::clamp(
        static_cast<double>(value.count()) / static_cast<double>(duration_.count()), 0.0, 1.0);
    return left + static_cast<int>(std::lround(ratio * static_cast<double>(right - left)));
}

std::chrono::milliseconds TrimTimeline::XToTime(const int x) noexcept {
    if (window_ == nullptr) {
        return {};
    }
    EnsureLayoutCache();
    const int left = cachedTrack_.bounds.left;
    const int right = cachedTrack_.bounds.right;
    const double ratio = std::clamp(
        static_cast<double>(x - left) / static_cast<double>(right - left), 0.0, 1.0);
    return std::chrono::milliseconds(
        static_cast<std::int64_t>(std::llround(ratio * static_cast<double>(duration_.count()))));
}

TrimTimeline::ActivePart TrimTimeline::HitTestPart(const POINT point) noexcept {
    if (window_ == nullptr) {
        return ActivePart::None;
    }
    EnsureLayoutCache();
    const timeline_detail::TimelineGeometry geometry =
        timeline_detail::BuildTimelineGeometry(
            cachedTrack_,
            cachedClientHeight_,
            cachedDpi_,
            TimeToX(trimStart_),
            TimeToX(trimEnd_),
            TimeToX(playhead_));
    return timeline_detail::HitTestTimeline(geometry, point);
}

int TrimTimeline::PartCenterX(const ActivePart part) noexcept {
    switch (part) {
    case ActivePart::Start:
        return TimeToX(trimStart_);
    case ActivePart::End:
        return TimeToX(trimEnd_);
    case ActivePart::Playhead:
        return TimeToX(playhead_);
    case ActivePart::None:
    default:
        return 0;
    }
}

}  // namespace qrec
