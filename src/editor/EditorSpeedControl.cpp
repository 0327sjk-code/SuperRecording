#include "editor/EditorSpeedControl.h"

#include "editor/EditorTheme.h"
#include "media/ExportQuality.h"
#include "ui/AntiAliasedDrawing.h"
#include "ui/Theme.h"

#include <windowsx.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <format>
#include <string>
#include <utility>

#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "user32.lib")

namespace qrec {
namespace {

constexpr wchar_t kSpeedControlClassName[] = L"SuperRecording.EditorSpeedControl";
constexpr UINT_PTR kMotionTimerId = 0x6B21;
constexpr UINT kMotionFrameMilliseconds = 16;
constexpr auto kHoverEnterDuration = std::chrono::milliseconds(150);
constexpr auto kHoverExitDuration = std::chrono::milliseconds(110);
constexpr auto kPressEnterDuration = std::chrono::milliseconds(90);
constexpr auto kPressExitDuration = std::chrono::milliseconds(120);
constexpr auto kFocusEnterDuration = std::chrono::milliseconds(170);
constexpr auto kFocusExitDuration = std::chrono::milliseconds(110);

[[nodiscard]] UINT DpiForWindow(const HWND window) noexcept {
    const UINT dpi = window != nullptr ? ::GetDpiForWindow(window) : 0;
    return dpi == 0 ? USER_DEFAULT_SCREEN_DPI : dpi;
}

[[nodiscard]] int ScaleForWindow(const HWND window, const int value) noexcept {
    return ::MulDiv(value, static_cast<int>(DpiForWindow(window)), USER_DEFAULT_SCREEN_DPI);
}

[[nodiscard]] float ScaleForWindow(const HWND window, const float value) noexcept {
    return value * static_cast<float>(DpiForWindow(window)) /
        static_cast<float>(USER_DEFAULT_SCREEN_DPI);
}

[[nodiscard]] HFONT CreateUiFont(
    const HWND window,
    const int heightDip,
    const int weight,
    const wchar_t* family) noexcept {
    LOGFONTW descriptor{};
    descriptor.lfHeight = -ScaleForWindow(window, heightDip);
    descriptor.lfWeight = weight;
    descriptor.lfQuality = theme::FontQuality;
    descriptor.lfOutPrecision = OUT_TT_ONLY_PRECIS;
    descriptor.lfPitchAndFamily = VARIABLE_PITCH | FF_SWISS;
    wcscpy_s(descriptor.lfFaceName, family);
    return ::CreateFontIndirectW(&descriptor);
}

void FillSolidRectangle(const HDC dc, const RECT& bounds, const COLORREF color) noexcept {
    const HBRUSH brush = ::CreateSolidBrush(color);
    if (brush != nullptr) {
        ::FillRect(dc, &bounds, brush);
        ::DeleteObject(brush);
    }
}

void DrawRoundedRectangleFallback(
    const HDC dc,
    const RECT& bounds,
    const int radius,
    const COLORREF fill,
    const COLORREF border,
    const int borderWidth = 1) noexcept {
    const HBRUSH brush = ::CreateSolidBrush(fill);
    const HPEN pen = ::CreatePen(PS_SOLID, std::max(1, borderWidth), border);
    if (brush != nullptr && pen != nullptr) {
        const HGDIOBJ previousBrush = ::SelectObject(dc, brush);
        const HGDIOBJ previousPen = ::SelectObject(dc, pen);
        ::RoundRect(
            dc,
            bounds.left,
            bounds.top,
            bounds.right,
            bounds.bottom,
            radius * 2,
            radius * 2);
        ::SelectObject(dc, previousPen);
        ::SelectObject(dc, previousBrush);
    }
    if (pen != nullptr) {
        ::DeleteObject(pen);
    }
    if (brush != nullptr) {
        ::DeleteObject(brush);
    }
}

void StrokeRoundedRectangleFallback(
    const HDC dc,
    const RECT& bounds,
    const int radius,
    const COLORREF color,
    const int width) noexcept {
    const HPEN pen = ::CreatePen(PS_SOLID, std::max(1, width), color);
    if (pen == nullptr) {
        return;
    }
    const HGDIOBJ previousBrush = ::SelectObject(dc, ::GetStockObject(HOLLOW_BRUSH));
    const HGDIOBJ previousPen = ::SelectObject(dc, pen);
    ::RoundRect(
        dc,
        bounds.left,
        bounds.top,
        bounds.right,
        bounds.bottom,
        radius * 2,
        radius * 2);
    ::SelectObject(dc, previousPen);
    ::SelectObject(dc, previousBrush);
    ::DeleteObject(pen);
}

void DrawTextBlock(
    const HDC dc,
    const RECT& bounds,
    const std::wstring_view text,
    const HFONT font,
    const COLORREF color,
    const UINT alignment) noexcept {
    if (dc == nullptr || font == nullptr || bounds.right <= bounds.left ||
        bounds.bottom <= bounds.top) {
        return;
    }
    const HGDIOBJ previousFont = ::SelectObject(dc, font);
    const int previousMode = ::SetBkMode(dc, TRANSPARENT);
    const COLORREF previousColor = ::SetTextColor(dc, color);
    RECT textBounds = bounds;
    ::DrawTextW(
        dc,
        text.data(),
        static_cast<int>(text.size()),
        &textBounds,
        alignment | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    ::SetTextColor(dc, previousColor);
    ::SetBkMode(dc, previousMode);
    ::SelectObject(dc, previousFont);
}

void SetMotionTarget(
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

EditorSpeedControl::~EditorSpeedControl() {
    Destroy();
}

bool EditorSpeedControl::RegisterWindowClass(const HINSTANCE instance) noexcept {
    WNDCLASSEXW existing{};
    existing.cbSize = sizeof(existing);
    if (::GetClassInfoExW(instance, kSpeedControlClassName, &existing) != FALSE) {
        return true;
    }

    WNDCLASSEXW windowClass{};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.style = CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS;
    windowClass.lpfnWndProc = &EditorSpeedControl::WindowProc;
    windowClass.hInstance = instance;
    windowClass.hCursor = ::LoadCursorW(nullptr, IDC_HAND);
    windowClass.lpszClassName = kSpeedControlClassName;
    return ::RegisterClassExW(&windowClass) != 0 ||
        ::GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
}

bool EditorSpeedControl::Create(
    const HWND parent,
    const int controlId,
    HINSTANCE instance,
    ChangedCallback changed) {
    return Create(
        parent,
        controlId,
        instance,
        EditorSliderPresentation::PlaybackSpeed,
        std::move(changed));
}

bool EditorSpeedControl::Create(
    const HWND parent,
    const int controlId,
    HINSTANCE instance,
    const EditorSliderPresentation presentation,
    ChangedCallback changed) {
    const int desiredValue = valueTenths_;
    const bool desiredEnabled = enabled_;
    Destroy();
    presentation_ = presentation;
    valueTenths_ = SnapValue(
        presentation_ == EditorSliderPresentation::QualityPercent
            ? media::ExportQuality::DefaultPercent
            : desiredValue);
    enabled_ = desiredEnabled;
    changedCallback_ = std::move(changed);

    if (parent == nullptr || ::IsWindow(parent) == FALSE) {
        return false;
    }
    if (instance == nullptr) {
        instance = ::GetModuleHandleW(nullptr);
    }
    if (instance == nullptr || !RegisterWindowClass(instance)) {
        return false;
    }

    parent_ = parent;
    instance_ = instance;
    controlId_ = controlId;
    animationsEnabled_ = ui::ClientAreaAnimationsEnabled();
    window_ = ::CreateWindowExW(
        0,
        kSpeedControlClassName,
        L"",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_CLIPSIBLINGS,
        0,
        0,
        1,
        1,
        parent_,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(controlId_)),
        instance_,
        this);
    if (window_ == nullptr) {
        parent_ = nullptr;
        instance_ = nullptr;
        controlId_ = 0;
        return false;
    }

    RecreateFonts();
    ::EnableWindow(window_, enabled_ ? TRUE : FALSE);
    UpdateAccessibleText();
    return true;
}

void EditorSpeedControl::Destroy() noexcept {
    if (window_ != nullptr && ::IsWindow(window_) != FALSE) {
        ::KillTimer(window_, kMotionTimerId);
        ::DestroyWindow(window_);
    }
    ReleaseBackBuffer();
    DeleteFonts();
    window_ = nullptr;
    parent_ = nullptr;
    instance_ = nullptr;
    controlId_ = 0;
    keyboardAdjustmentKey_ = 0;
    hovered_ = false;
    trackingMouse_ = false;
    dragging_ = false;
    interactionChanged_ = false;
    timerArmed_ = false;
    hoverMotion_.JumpTo(0.0F);
    pressMotion_.JumpTo(0.0F);
    focusMotion_.JumpTo(0.0F);
}

void EditorSpeedControl::SetEnabled(const bool enabled) noexcept {
    enabled_ = enabled;
    if (!enabled_) {
        CancelInput(true);
        hovered_ = false;
        hoverMotion_.JumpTo(0.0F);
        pressMotion_.JumpTo(0.0F);
    }
    if (window_ != nullptr && ::IsWindow(window_) != FALSE) {
        ::EnableWindow(window_, enabled_ ? TRUE : FALSE);
        ::InvalidateRect(window_, nullptr, FALSE);
        ::NotifyWinEvent(
            EVENT_OBJECT_STATECHANGE,
            window_,
            OBJID_CLIENT,
            CHILDID_SELF);
    }
}

void EditorSpeedControl::SetValueTenths(const int speedTenths) noexcept {
    SetValue(speedTenths);
}

void EditorSpeedControl::SetValue(const int value) noexcept {
    const int clamped = SnapValue(value);
    if (clamped == valueTenths_) {
        return;
    }
    valueTenths_ = clamped;
    UpdateAccessibleText();
    if (window_ != nullptr) {
        ::InvalidateRect(window_, nullptr, FALSE);
    }
}

void EditorSpeedControl::SetChangedCallback(ChangedCallback changed) {
    changedCallback_ = std::move(changed);
}

LRESULT CALLBACK EditorSpeedControl::WindowProc(
    const HWND window,
    const UINT message,
    const WPARAM wParam,
    const LPARAM lParam) noexcept {
    EditorSpeedControl* self = reinterpret_cast<EditorSpeedControl*>(
        ::GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lParam);
        self = static_cast<EditorSpeedControl*>(create->lpCreateParams);
        if (self != nullptr) {
            self->window_ = window;
            ::SetWindowLongPtrW(
                window,
                GWLP_USERDATA,
                reinterpret_cast<LONG_PTR>(self));
        }
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

LRESULT EditorSpeedControl::HandleMessage(
    const UINT message,
    const WPARAM wParam,
    const LPARAM lParam) {
    switch (message) {
    case WM_PAINT:
        Paint();
        return 0;
    case WM_PRINTCLIENT: {
        RECT client{};
        ::GetClientRect(window_, &client);
        DrawContent(reinterpret_cast<HDC>(wParam), client);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_SIZE:
        ReleaseBackBuffer();
        ::InvalidateRect(window_, nullptr, FALSE);
        return 0;
#ifdef WM_DPICHANGED_AFTERPARENT
    case WM_DPICHANGED_AFTERPARENT:
#endif
    case WM_DPICHANGED:
        ReleaseBackBuffer();
        RecreateFonts();
        ::InvalidateRect(window_, nullptr, FALSE);
        return 0;
    case WM_SETTINGCHANGE:
        animationsEnabled_ = ui::RefreshClientAreaAnimationsEnabled();
        if (!animationsEnabled_) {
            JumpMotionToTargets();
        }
        UpdateMotionTimer();
        ::InvalidateRect(window_, nullptr, FALSE);
        return 0;
    case WM_TIMER:
        if (wParam == kMotionTimerId) {
            const bool active = AdvanceMotion();
            ::InvalidateRect(window_, nullptr, FALSE);
            if (!active) {
                ::KillTimer(window_, kMotionTimerId);
                timerArmed_ = false;
            }
            return 0;
        }
        break;
    case WM_GETDLGCODE:
        return DLGC_WANTARROWS | DLGC_WANTCHARS;
    case WM_SETFOCUS:
        SetFocusState(true);
        return 0;
    case WM_KILLFOCUS:
        CancelInput(true);
        SetFocusState(false);
        return 0;
    case WM_MOUSEMOVE: {
        if (!enabled_) {
            return 0;
        }
        SetHoverState(true);
        if (!trackingMouse_) {
            TRACKMOUSEEVENT tracking{};
            tracking.cbSize = sizeof(tracking);
            tracking.dwFlags = TME_LEAVE;
            tracking.hwndTrack = window_;
            trackingMouse_ = ::TrackMouseEvent(&tracking) != FALSE;
        }
        if (dragging_) {
            interactionChanged_ = ApplyUserValue(
                ValueFromClientX(GET_X_LPARAM(lParam))) ||
                interactionChanged_;
        }
        return 0;
    }
    case WM_MOUSELEAVE:
        trackingMouse_ = false;
        SetHoverState(false);
        return 0;
    case WM_LBUTTONDOWN:
    case WM_LBUTTONDBLCLK: {
        if (!enabled_) {
            return 0;
        }
        ::SetFocus(window_);
        const RECT track = TrackBounds();
        const int x = GET_X_LPARAM(lParam);
        const int horizontalSlop = ScaleForWindow(window_, 8);
        if (x < track.left - horizontalSlop || x > track.right + horizontalSlop) {
            return 0;
        }
        ::SetCapture(window_);
        dragging_ = ::GetCapture() == window_;
        interactionChanged_ = false;
        SetPressState(dragging_);
        if (dragging_) {
            interactionChanged_ = ApplyUserValue(ValueFromClientX(x));
        }
        return 0;
    }
    case WM_LBUTTONUP:
        if (dragging_) {
            interactionChanged_ = ApplyUserValue(
                ValueFromClientX(GET_X_LPARAM(lParam))) || interactionChanged_;
            dragging_ = false;
            SetPressState(false);
            if (::GetCapture() == window_) {
                ::ReleaseCapture();
            }
            CommitInteraction();
        }
        return 0;
    case WM_CAPTURECHANGED:
        if (dragging_) {
            dragging_ = false;
            SetPressState(false);
            CommitInteraction();
        }
        return 0;
    case WM_CANCELMODE:
        CancelInput(true);
        return 0;
    case WM_KEYDOWN: {
        if (!enabled_) {
            return 0;
        }
        int requestedValue = valueTenths_;
        if (wParam == VK_LEFT || wParam == VK_DOWN) {
            requestedValue -= ValueStep();
        } else if (wParam == VK_RIGHT || wParam == VK_UP) {
            requestedValue += ValueStep();
        } else if (wParam == VK_HOME) {
            requestedValue = MinimumValue();
        } else if (wParam == VK_END) {
            requestedValue = MaximumValue();
        } else {
            break;
        }
        keyboardAdjustmentKey_ = static_cast<UINT>(wParam);
        SetPressState(true);
        interactionChanged_ = ApplyUserValue(requestedValue) || interactionChanged_;
        return 0;
    }
    case WM_KEYUP:
        if (keyboardAdjustmentKey_ != 0 &&
            wParam == keyboardAdjustmentKey_) {
            keyboardAdjustmentKey_ = 0;
            SetPressState(false);
            CommitInteraction();
            return 0;
        }
        break;
    case WM_ENABLE:
        enabled_ = wParam != FALSE;
        if (!enabled_) {
            CancelInput(true);
        }
        ::InvalidateRect(window_, nullptr, FALSE);
        return 0;
    case WM_NCDESTROY: {
        const HWND destroyedWindow = window_;
        ::KillTimer(destroyedWindow, kMotionTimerId);
        ReleaseBackBuffer();
        DeleteFonts();
        ::SetWindowLongPtrW(destroyedWindow, GWLP_USERDATA, 0);
        window_ = nullptr;
        timerArmed_ = false;
        trackingMouse_ = false;
        dragging_ = false;
        return ::DefWindowProcW(destroyedWindow, message, wParam, lParam);
    }
    default:
        break;
    }
    return ::DefWindowProcW(window_, message, wParam, lParam);
}

void EditorSpeedControl::Paint() {
    PAINTSTRUCT paint{};
    const HDC target = ::BeginPaint(window_, &paint);
    RECT client{};
    ::GetClientRect(window_, &client);
    const int width = std::max(1, static_cast<int>(client.right - client.left));
    const int height = std::max(1, static_cast<int>(client.bottom - client.top));
    if (EnsureBackBuffer(target, width, height)) {
        DrawContent(backBufferDc_, client);
        ::BitBlt(
            target,
            paint.rcPaint.left,
            paint.rcPaint.top,
            paint.rcPaint.right - paint.rcPaint.left,
            paint.rcPaint.bottom - paint.rcPaint.top,
            backBufferDc_,
            paint.rcPaint.left,
            paint.rcPaint.top,
            SRCCOPY);
    } else {
        DrawContent(target, client);
    }
    ::EndPaint(window_, &paint);
}

void EditorSpeedControl::DrawContent(const HDC dc, const RECT& client) const {
    FillSolidRectangle(dc, client, editor_theme::Panel);
    if (client.right <= client.left || client.bottom <= client.top) {
        return;
    }

    const bool focused = window_ != nullptr && ::GetFocus() == window_;
    const float hoverAmount = enabled_ ? hoverMotion_.Value() : 0.0F;
    const float pressAmount = enabled_ ? pressMotion_.Value() : 0.0F;
    const float focusAmount = enabled_ && focused ? focusMotion_.Value() : 0.0F;
    COLORREF fill = editor_theme::Control;
    COLORREF border = editor_theme::Border;
    COLORREF labelColor = editor_theme::TextSecondary;
    COLORREF valueColor = editor_theme::TextPrimary;
    if (!enabled_) {
        fill = editor_theme::ControlDisabled;
        border = editor_theme::BorderDisabled;
        labelColor = editor_theme::TextDisabled;
        valueColor = editor_theme::TextDisabled;
    } else {
        fill = ui::InterpolateColor(fill, editor_theme::ControlHover, hoverAmount);
        fill = ui::InterpolateColor(fill, editor_theme::ControlPressed, pressAmount);
        border = ui::InterpolateColor(border, editor_theme::BorderHover, hoverAmount);
    }

    RECT shape = client;
    const int inset = ScaleForWindow(window_, 1);
    ::InflateRect(&shape, -inset, -inset);
    const int radius = ScaleForWindow(window_, theme::CornerMedium);
    ui::Canvas canvas(dc);
    if (canvas.Valid()) {
        canvas.DrawRoundedRectangle(
            shape,
            static_cast<float>(radius),
            fill,
            border,
            static_cast<float>(std::max(1, ScaleForWindow(window_, 1))));
    } else {
        DrawRoundedRectangleFallback(dc, shape, radius, fill, border);
    }

    const int centerY = (client.top + client.bottom) / 2;
    const int horizontalPadding = ScaleForWindow(window_, 10);
    RECT labelBounds{
        client.left + horizontalPadding,
        client.top,
        client.left + ScaleForWindow(window_, 42),
        client.bottom};
    const std::wstring_view visibleLabel =
        presentation_ == EditorSliderPresentation::QualityPercent
        ? L"画质"
        : L"倍速";
    DrawTextBlock(
        dc,
        labelBounds,
        visibleLabel,
        labelFont_,
        labelColor,
        DT_LEFT);

    const RECT track = TrackBounds();
    const int trackHeight = std::max(2, ScaleForWindow(window_, 4));
    RECT trackShape{
        track.left,
        centerY - trackHeight / 2,
        track.right,
        centerY - trackHeight / 2 + trackHeight};
    const int thumbX = ThumbCenterX();
    RECT activeTrack = trackShape;
    activeTrack.right = std::max(static_cast<int>(activeTrack.left), thumbX);
    const COLORREF inactiveTrackColor = enabled_
        ? editor_theme::TimelineTrack
        : editor_theme::ControlDisabled;
    const COLORREF activeTrackColor = enabled_
        ? ui::InterpolateColor(
            editor_theme::SelectionBorder,
            editor_theme::Focus,
            focusAmount * 0.55F)
        : editor_theme::TextDisabled;
    if (canvas.Valid()) {
        canvas.FillRoundedRectangle(
            trackShape,
            static_cast<float>(trackHeight / 2),
            inactiveTrackColor);
        canvas.FillRoundedRectangle(
            activeTrack,
            static_cast<float>(trackHeight / 2),
            activeTrackColor);
    } else {
        DrawRoundedRectangleFallback(
            dc,
            trackShape,
            trackHeight / 2,
            inactiveTrackColor,
            inactiveTrackColor);
        if (activeTrack.right > activeTrack.left) {
            DrawRoundedRectangleFallback(
                dc,
                activeTrack,
                trackHeight / 2,
                activeTrackColor,
                activeTrackColor);
        }
    }

    const float thumbGrowth = hoverAmount * 1.0F + pressAmount * 0.5F;
    const float thumbRadius = ScaleForWindow(window_, 5.0F + thumbGrowth);
    const COLORREF thumbColor = enabled_
        ? ui::InterpolateColor(
            editor_theme::TimelinePlayhead,
            editor_theme::White,
            hoverAmount)
        : editor_theme::TextDisabled;
    if (canvas.Valid()) {
        canvas.FillEllipse(
            static_cast<float>(thumbX),
            static_cast<float>(centerY),
            thumbRadius,
            thumbRadius,
            thumbColor);
    } else {
        const HBRUSH thumbBrush = ::CreateSolidBrush(thumbColor);
        if (thumbBrush != nullptr) {
            const HGDIOBJ previousBrush = ::SelectObject(dc, thumbBrush);
            const HGDIOBJ previousPen = ::SelectObject(dc, ::GetStockObject(NULL_PEN));
            ::Ellipse(
                dc,
                thumbX - static_cast<int>(std::lround(thumbRadius)),
                centerY - static_cast<int>(std::lround(thumbRadius)),
                thumbX + static_cast<int>(std::lround(thumbRadius)) + 1,
                centerY + static_cast<int>(std::lround(thumbRadius)) + 1);
            ::SelectObject(dc, previousPen);
            ::SelectObject(dc, previousBrush);
            ::DeleteObject(thumbBrush);
        }
    }

    const std::wstring valueText =
        presentation_ == EditorSliderPresentation::QualityPercent
        ? std::format(L"{}%", valueTenths_)
        : std::format(L"{:.1f}×", valueTenths_ / 10.0);
    RECT valueBounds{
        track.right + ScaleForWindow(window_, 7),
        client.top,
        client.right - horizontalPadding,
        client.bottom};
    DrawTextBlock(
        dc,
        valueBounds,
        valueText,
        valueFont_,
        valueColor,
        DT_RIGHT);

    if (focusAmount > 0.001F && enabled_) {
        RECT focus = shape;
        ::InflateRect(
            &focus,
            -ScaleForWindow(window_, 2),
            -ScaleForWindow(window_, 2));
        const COLORREF focusColor = ui::InterpolateColor(
            fill,
            editor_theme::Focus,
            focusAmount);
        if (canvas.Valid()) {
            canvas.StrokeRoundedRectangle(
                focus,
                static_cast<float>(std::max(1, radius - ScaleForWindow(window_, 2))),
                focusColor,
                static_cast<float>(std::max(1, ScaleForWindow(window_, 2))));
        } else {
            StrokeRoundedRectangleFallback(
                dc,
                focus,
                std::max(1, radius - ScaleForWindow(window_, 2)),
                focusColor,
                std::max(1, ScaleForWindow(window_, 2)));
        }
    }
}

void EditorSpeedControl::RecreateFonts() noexcept {
    DeleteFonts();
    labelFont_ = CreateUiFont(window_, 13, FW_NORMAL, theme::FontFamily);
    valueFont_ = CreateUiFont(window_, 13, FW_SEMIBOLD, theme::LatinFontFamily);
}

void EditorSpeedControl::DeleteFonts() noexcept {
    if (labelFont_ != nullptr) {
        ::DeleteObject(labelFont_);
        labelFont_ = nullptr;
    }
    if (valueFont_ != nullptr) {
        ::DeleteObject(valueFont_);
        valueFont_ = nullptr;
    }
}

bool EditorSpeedControl::EnsureBackBuffer(
    const HDC targetDc,
    const int width,
    const int height) noexcept {
    if (targetDc == nullptr || width <= 0 || height <= 0) {
        return false;
    }
    if (backBufferDc_ != nullptr && backBufferBitmap_ != nullptr &&
        backBufferWidth_ == width && backBufferHeight_ == height) {
        return true;
    }
    const HDC replacementDc = ::CreateCompatibleDC(targetDc);
    const HBITMAP replacementBitmap = replacementDc != nullptr
        ? ::CreateCompatibleBitmap(targetDc, width, height)
        : nullptr;
    if (replacementDc == nullptr || replacementBitmap == nullptr) {
        if (replacementBitmap != nullptr) {
            ::DeleteObject(replacementBitmap);
        }
        if (replacementDc != nullptr) {
            ::DeleteDC(replacementDc);
        }
        return false;
    }
    const HGDIOBJ replacementPrevious = ::SelectObject(
        replacementDc,
        replacementBitmap);
    if (replacementPrevious == nullptr || replacementPrevious == HGDI_ERROR) {
        ::DeleteObject(replacementBitmap);
        ::DeleteDC(replacementDc);
        return false;
    }
    ReleaseBackBuffer();
    backBufferDc_ = replacementDc;
    backBufferBitmap_ = replacementBitmap;
    backBufferPreviousBitmap_ = replacementPrevious;
    backBufferWidth_ = width;
    backBufferHeight_ = height;
    return true;
}

void EditorSpeedControl::ReleaseBackBuffer() noexcept {
    if (backBufferDc_ != nullptr && backBufferPreviousBitmap_ != nullptr &&
        backBufferPreviousBitmap_ != HGDI_ERROR) {
        ::SelectObject(backBufferDc_, backBufferPreviousBitmap_);
    }
    if (backBufferBitmap_ != nullptr) {
        ::DeleteObject(backBufferBitmap_);
    }
    if (backBufferDc_ != nullptr) {
        ::DeleteDC(backBufferDc_);
    }
    backBufferDc_ = nullptr;
    backBufferBitmap_ = nullptr;
    backBufferPreviousBitmap_ = nullptr;
    backBufferWidth_ = 0;
    backBufferHeight_ = 0;
}

RECT EditorSpeedControl::TrackBounds() const noexcept {
    RECT client{};
    if (window_ == nullptr || ::GetClientRect(window_, &client) == FALSE) {
        return {0, 0, 1, 1};
    }
    const int left = static_cast<int>(client.left) + ScaleForWindow(window_, 50);
    const int right = std::max(
        left + 1,
        static_cast<int>(client.right) - ScaleForWindow(window_, 48));
    return {left, client.top, right, client.bottom};
}

int EditorSpeedControl::ValueFromClientX(const int x) const noexcept {
    const RECT track = TrackBounds();
    const int width = std::max(1, static_cast<int>(track.right - track.left));
    const double ratio = std::clamp(
        static_cast<double>(x - track.left) / static_cast<double>(width),
        0.0,
        1.0);
    const int step = ValueStep();
    const int stepCount = std::max(1, (MaximumValue() - MinimumValue()) / step);
    return MinimumValue() + static_cast<int>(std::lround(
        ratio * static_cast<double>(stepCount))) * step;
}

int EditorSpeedControl::ThumbCenterX() const noexcept {
    const RECT track = TrackBounds();
    const double ratio = static_cast<double>(valueTenths_ - MinimumValue()) /
        static_cast<double>(MaximumValue() - MinimumValue());
    return track.left + static_cast<int>(std::lround(
        ratio * static_cast<double>(track.right - track.left)));
}

bool EditorSpeedControl::ApplyUserValue(const int value) {
    const int clamped = SnapValue(value);
    if (clamped == valueTenths_) {
        return false;
    }
    valueTenths_ = clamped;
    UpdateAccessibleText();
    ::InvalidateRect(window_, nullptr, FALSE);
    if (changedCallback_) {
        changedCallback_(valueTenths_, EditorSliderInteractionPhase::Preview);
    }
    return true;
}

void EditorSpeedControl::CommitInteraction() {
    if (!interactionChanged_) {
        return;
    }
    interactionChanged_ = false;
    if (changedCallback_) {
        changedCallback_(valueTenths_, EditorSliderInteractionPhase::Committed);
    }
}

void EditorSpeedControl::UpdateAccessibleText() noexcept {
    if (window_ == nullptr) {
        return;
    }
    const std::wstring accessibleName =
        presentation_ == EditorSliderPresentation::QualityPercent
        ? std::format(
            L"预览与输出画质，{}%。方向键按 {}% 调整",
            valueTenths_,
            media::ExportQuality::StepPercent)
        : std::format(
            L"播放与输出倍速，{:.1f} 倍。方向键按 0.1 倍调整",
            valueTenths_ / 10.0);
    ::SetWindowTextW(window_, accessibleName.c_str());
    ::NotifyWinEvent(
        EVENT_OBJECT_NAMECHANGE,
        window_,
        OBJID_CLIENT,
        CHILDID_SELF);
}

int EditorSpeedControl::MinimumValue() const noexcept {
    return presentation_ == EditorSliderPresentation::QualityPercent
        ? media::ExportQuality::MinimumPercent
        : MinimumSpeedTenths;
}

int EditorSpeedControl::MaximumValue() const noexcept {
    return presentation_ == EditorSliderPresentation::QualityPercent
        ? media::ExportQuality::MaximumPercent
        : MaximumSpeedTenths;
}

int EditorSpeedControl::ValueStep() const noexcept {
    return presentation_ == EditorSliderPresentation::QualityPercent
        ? media::ExportQuality::StepPercent
        : 1;
}

int EditorSpeedControl::SnapValue(const int value) const noexcept {
    const int minimum = MinimumValue();
    const int maximum = MaximumValue();
    const int step = ValueStep();
    const int clamped = std::clamp(value, minimum, maximum);
    const int snapped = minimum +
        ((clamped - minimum + step / 2) / step) * step;
    return std::clamp(snapped, minimum, maximum);
}

void EditorSpeedControl::SetHoverState(const bool hovered) noexcept {
    if (hovered_ == hovered) {
        return;
    }
    hovered_ = hovered;
    SetMotionTarget(
        hoverMotion_,
        hovered_,
        kHoverEnterDuration,
        kHoverExitDuration,
        animationsEnabled_);
    UpdateMotionTimer();
    if (window_ != nullptr) {
        ::InvalidateRect(window_, nullptr, FALSE);
    }
}

void EditorSpeedControl::SetPressState(const bool pressed) noexcept {
    SetMotionTarget(
        pressMotion_,
        pressed,
        kPressEnterDuration,
        kPressExitDuration,
        animationsEnabled_,
        ui::MotionEasing::EaseOutQuint);
    UpdateMotionTimer();
    if (window_ != nullptr) {
        ::InvalidateRect(window_, nullptr, FALSE);
    }
}

void EditorSpeedControl::SetFocusState(const bool focused) noexcept {
    SetMotionTarget(
        focusMotion_,
        focused,
        kFocusEnterDuration,
        kFocusExitDuration,
        animationsEnabled_,
        ui::MotionEasing::EaseOutQuint);
    UpdateMotionTimer();
    if (window_ != nullptr) {
        ::InvalidateRect(window_, nullptr, FALSE);
    }
}

void EditorSpeedControl::CancelInput(const bool commit) noexcept {
    const bool hadInput = dragging_ || keyboardAdjustmentKey_ != 0;
    dragging_ = false;
    keyboardAdjustmentKey_ = 0;
    SetPressState(false);
    if (::GetCapture() == window_) {
        ::ReleaseCapture();
    }
    if (commit && hadInput) {
        try {
            CommitInteraction();
        } catch (...) {
        }
    } else if (!commit) {
        interactionChanged_ = false;
    }
}

void EditorSpeedControl::UpdateMotionTimer() noexcept {
    if (window_ == nullptr) {
        return;
    }
    const bool active = hoverMotion_.IsActive() || pressMotion_.IsActive() ||
        focusMotion_.IsActive();
    if (active && !timerArmed_) {
        timerArmed_ = ::SetTimer(
            window_,
            kMotionTimerId,
            kMotionFrameMilliseconds,
            nullptr) != 0;
        if (!timerArmed_) {
            JumpMotionToTargets();
            ::InvalidateRect(window_, nullptr, FALSE);
        }
    } else if (!active && timerArmed_) {
        ::KillTimer(window_, kMotionTimerId);
        timerArmed_ = false;
    }
}

bool EditorSpeedControl::AdvanceMotion() noexcept {
    if (!animationsEnabled_) {
        JumpMotionToTargets();
        return false;
    }
    const auto now = ui::MotionState::Clock::now();
    const bool hover = hoverMotion_.Advance(now);
    const bool press = pressMotion_.Advance(now);
    const bool focus = focusMotion_.Advance(now);
    return hover || press || focus;
}

void EditorSpeedControl::JumpMotionToTargets() noexcept {
    hoverMotion_.JumpTo(hoverMotion_.Target());
    pressMotion_.JumpTo(pressMotion_.Target());
    focusMotion_.JumpTo(focusMotion_.Target());
}

}  // namespace qrec
