#include "editor/EditorAudioToggle.h"

#include "editor/EditorTheme.h"
#include "ui/AntiAliasedDrawing.h"
#include "ui/Theme.h"

#include <commctrl.h>
#include <windowsx.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <utility>

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "user32.lib")

namespace qrec {
namespace {

constexpr UINT_PTR kSubclassId = 0x6A31;
constexpr UINT_PTR kMotionTimerId = 0x6A32;
constexpr UINT kMotionFrameMilliseconds = 16;
constexpr auto kHoverEnterDuration = std::chrono::milliseconds(160);
constexpr auto kHoverExitDuration = std::chrono::milliseconds(120);
constexpr auto kPressEnterDuration = std::chrono::milliseconds(100);
constexpr auto kPressExitDuration = std::chrono::milliseconds(140);
constexpr auto kFocusEnterDuration = std::chrono::milliseconds(180);
constexpr auto kFocusExitDuration = std::chrono::milliseconds(120);
constexpr auto kSelectionDuration = std::chrono::milliseconds(190);

constexpr wchar_t kLabel[] = L"声音";
constexpr wchar_t kEnabledStatus[] = L"开启";
constexpr wchar_t kDisabledStatus[] = L"关闭";
constexpr wchar_t kAccessibleEnabled[] =
    L"电脑声音，已开启。录屏将包含电脑正在播放的声音";
constexpr wchar_t kAccessibleDisabled[] =
    L"电脑声音，已关闭。录屏不会包含电脑正在播放的声音";

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

void FillSolidRectangle(const HDC dc, const RECT& bounds, const COLORREF color) noexcept {
    if (dc == nullptr) {
        return;
    }
    const HBRUSH brush = ::CreateSolidBrush(color);
    if (brush != nullptr) {
        ::FillRect(dc, &bounds, brush);
        ::DeleteObject(brush);
    }
}

void DrawFallbackRoundedRectangle(
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

void DrawRoundedRectangle(
    ui::Canvas& canvas,
    const HDC dc,
    const RECT& bounds,
    const int radius,
    const COLORREF fill,
    const COLORREF border,
    const int borderWidth = 1) noexcept {
    if (canvas.Valid()) {
        canvas.DrawRoundedRectangle(
            bounds,
            static_cast<float>(radius),
            fill,
            border,
            static_cast<float>(std::max(1, borderWidth)));
        return;
    }
    DrawFallbackRoundedRectangle(dc, bounds, radius, fill, border, borderWidth);
}

void DrawTextBlock(
    const HDC dc,
    const RECT& bounds,
    const std::wstring_view text,
    const HFONT font,
    const COLORREF color,
    const UINT alignment) noexcept {
    if (dc == nullptr || font == nullptr || text.empty() ||
        bounds.right <= bounds.left || bounds.bottom <= bounds.top) {
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
        alignment | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
    ::SetTextColor(dc, previousColor);
    ::SetBkMode(dc, previousMode);
    ::SelectObject(dc, previousFont);
}

[[nodiscard]] HFONT CreateUiFont(
    const HWND window,
    const int heightDip,
    const int weight) noexcept {
    LOGFONTW descriptor{};
    descriptor.lfHeight = -ScaleForWindow(window, heightDip);
    descriptor.lfWeight = weight;
    descriptor.lfQuality = theme::FontQuality;
    descriptor.lfOutPrecision = OUT_TT_ONLY_PRECIS;
    descriptor.lfPitchAndFamily = VARIABLE_PITCH | FF_SWISS;
    wcscpy_s(descriptor.lfFaceName, theme::FontFamily);
    return ::CreateFontIndirectW(&descriptor);
}

[[nodiscard]] bool IsPointInsideClient(const HWND window, const LPARAM lParam) noexcept {
    RECT client{};
    if (window == nullptr || ::GetClientRect(window, &client) == FALSE) {
        return false;
    }
    const POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
    return ::PtInRect(&client, point) != FALSE;
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

EditorAudioToggle::~EditorAudioToggle() {
    Destroy();
}

bool EditorAudioToggle::Create(
    const HWND parent,
    const int controlId,
    HINSTANCE instance,
    ChangedCallback changed) {
    const bool desiredEnabled = enabled_;
    const bool desiredChecked = checked_;
    Destroy();
    enabled_ = desiredEnabled;
    checked_ = desiredChecked;
    changedCallback_ = std::move(changed);

    if (parent == nullptr || ::IsWindow(parent) == FALSE) {
        return false;
    }
    if (instance == nullptr) {
        instance = ::GetModuleHandleW(nullptr);
    }
    if (instance == nullptr) {
        return false;
    }

    parent_ = parent;
    instance_ = instance;
    controlId_ = controlId;
    animationsEnabled_ = ui::ClientAreaAnimationsEnabled();
    checkedMotion_.JumpTo(checked_ ? 1.0F : 0.0F);

    window_ = ::CreateWindowExW(
        0,
        WC_BUTTONW,
        checked_ ? kAccessibleEnabled : kAccessibleDisabled,
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_CLIPSIBLINGS |
            BS_AUTOCHECKBOX | BS_NOTIFY,
        0,
        0,
        1,
        1,
        parent_,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(controlId_)),
        instance_,
        nullptr);
    if (window_ == nullptr) {
        parent_ = nullptr;
        instance_ = nullptr;
        controlId_ = 0;
        return false;
    }
    if (::SetWindowSubclass(
            window_,
            &EditorAudioToggle::SubclassProc,
            kSubclassId,
            reinterpret_cast<DWORD_PTR>(this)) == FALSE) {
        ::DestroyWindow(window_);
        window_ = nullptr;
        parent_ = nullptr;
        instance_ = nullptr;
        controlId_ = 0;
        return false;
    }

    RecreateFonts();
    ::SendMessageW(
        window_,
        BM_SETCHECK,
        checked_ ? BST_CHECKED : BST_UNCHECKED,
        0);
    ::EnableWindow(window_, enabled_ ? TRUE : FALSE);
    UpdateAccessibleText();
    ::InvalidateRect(window_, nullptr, FALSE);
    return true;
}

void EditorAudioToggle::Destroy() noexcept {
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
    keyboardActivationKey_ = 0;
    hovered_ = false;
    trackingMouse_ = false;
    mousePressed_ = false;
    timerArmed_ = false;
    hoverMotion_.JumpTo(0.0F);
    pressMotion_.JumpTo(0.0F);
    focusMotion_.JumpTo(0.0F);
    checkedMotion_.JumpTo(checked_ ? 1.0F : 0.0F);
}

void EditorAudioToggle::SetEnabled(const bool enabled) noexcept {
    enabled_ = enabled;
    if (window_ != nullptr && ::IsWindow(window_) != FALSE) {
        ::EnableWindow(window_, enabled ? TRUE : FALSE);
        ::InvalidateRect(window_, nullptr, FALSE);
        ::NotifyWinEvent(
            EVENT_OBJECT_STATECHANGE,
            window_,
            OBJID_CLIENT,
            CHILDID_SELF);
    }
}

void EditorAudioToggle::SetChecked(const bool checked) noexcept {
    if (window_ != nullptr && ::IsWindow(window_) != FALSE) {
        ::SendMessageW(
            window_,
            BM_SETCHECK,
            checked ? BST_CHECKED : BST_UNCHECKED,
            0);
        return;
    }
    checked_ = checked;
    checkedMotion_.JumpTo(checked ? 1.0F : 0.0F);
}

void EditorAudioToggle::SetStatusText(const std::wstring_view statusText) {
    if (std::wstring_view(statusText_) == statusText) {
        return;
    }
    statusText_.assign(statusText);
    UpdateAccessibleText();
    if (window_ != nullptr) {
        ::InvalidateRect(window_, nullptr, FALSE);
    }
}

void EditorAudioToggle::SetChangedCallback(ChangedCallback changed) {
    changedCallback_ = std::move(changed);
}

LRESULT CALLBACK EditorAudioToggle::SubclassProc(
    const HWND control,
    const UINT message,
    const WPARAM wParam,
    const LPARAM lParam,
    const UINT_PTR subclassId,
    const DWORD_PTR referenceData) noexcept {
    auto* self = reinterpret_cast<EditorAudioToggle*>(referenceData);
    if (self == nullptr) {
        return ::DefSubclassProc(control, message, wParam, lParam);
    }
    try {
        return self->HandleMessage(control, message, wParam, lParam, subclassId);
    } catch (...) {
        return ::DefSubclassProc(control, message, wParam, lParam);
    }
}

LRESULT EditorAudioToggle::HandleMessage(
    const HWND control,
    const UINT message,
    const WPARAM wParam,
    const LPARAM lParam,
    const UINT_PTR subclassId) {
    switch (message) {
    case WM_PAINT:
        PaintWindow();
        return 0;
    case WM_PRINTCLIENT: {
        RECT client{};
        ::GetClientRect(control, &client);
        DrawContent(reinterpret_cast<HDC>(wParam), client);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_SIZE:
        ReleaseBackBuffer();
        ::InvalidateRect(control, nullptr, FALSE);
        return 0;
#ifdef WM_DPICHANGED_AFTERPARENT
    case WM_DPICHANGED_AFTERPARENT:
#endif
    case WM_DPICHANGED:
        ReleaseBackBuffer();
        RecreateFonts();
        ::InvalidateRect(control, nullptr, FALSE);
        return 0;
    case WM_SETTINGCHANGE:
        animationsEnabled_ = ui::RefreshClientAreaAnimationsEnabled();
        if (!animationsEnabled_) {
            JumpMotionToTargets();
        }
        UpdateMotionTimer();
        ::InvalidateRect(control, nullptr, FALSE);
        break;
    case WM_TIMER:
        if (wParam == kMotionTimerId) {
            const bool active = AdvanceMotion();
            ::InvalidateRect(control, nullptr, FALSE);
            if (!active) {
                ::KillTimer(control, kMotionTimerId);
                timerArmed_ = false;
            }
            return 0;
        }
        break;
    case WM_MOUSEMOVE: {
        if (!enabled_) {
            return 0;
        }
        const bool inside = IsPointInsideClient(control, lParam);
        SetHoverState(inside);
        if (mousePressed_) {
            SetPressState(inside);
        }
        if (!trackingMouse_) {
            TRACKMOUSEEVENT tracking{};
            tracking.cbSize = sizeof(tracking);
            tracking.dwFlags = TME_LEAVE;
            tracking.hwndTrack = control;
            trackingMouse_ = ::TrackMouseEvent(&tracking) != FALSE;
        }
        return 0;
    }
    case WM_MOUSELEAVE:
        trackingMouse_ = false;
        SetHoverState(false);
        if (!mousePressed_) {
            SetPressState(false);
        }
        return 0;
    case WM_LBUTTONDOWN:
    case WM_LBUTTONDBLCLK:
        if (!enabled_) {
            return 0;
        }
        ::SetFocus(control);
        ::SetCapture(control);
        mousePressed_ = true;
        SetHoverState(true);
        SetPressState(true);
        return 0;
    case WM_LBUTTONUP: {
        if (!mousePressed_) {
            return 0;
        }
        const bool activate = enabled_ && IsPointInsideClient(control, lParam);
        mousePressed_ = false;
        if (::GetCapture() == control) {
            ::ReleaseCapture();
        }
        SetPressState(false);
        if (activate) {
            ActivateFromUser();
        }
        return 0;
    }
    case WM_CAPTURECHANGED:
    case WM_CANCELMODE:
        mousePressed_ = false;
        SetPressState(false);
        return 0;
    case WM_SETFOCUS:
        SetFocusState(true);
        return 0;
    case WM_KILLFOCUS:
        keyboardActivationKey_ = 0;
        SetPressState(false);
        SetFocusState(false);
        return 0;
    case WM_GETDLGCODE: {
        const LRESULT base = ::DefSubclassProc(control, message, wParam, lParam);
        if (wParam == VK_SPACE || wParam == VK_RETURN) {
            return base | DLGC_WANTMESSAGE;
        }
        return base;
    }
    case WM_KEYDOWN:
        if (wParam == VK_SPACE || wParam == VK_RETURN) {
            if (enabled_ && (lParam & (1LL << 30)) == 0 && keyboardActivationKey_ == 0) {
                keyboardActivationKey_ = static_cast<UINT>(wParam);
                SetPressState(true);
            }
            return 0;
        }
        break;
    case WM_KEYUP:
        if (wParam == VK_SPACE || wParam == VK_RETURN) {
            const bool activate = enabled_ && keyboardActivationKey_ == wParam;
            keyboardActivationKey_ = 0;
            SetPressState(false);
            if (activate) {
                ActivateFromUser();
            }
            return 0;
        }
        break;
    case WM_CHAR:
        if (wParam == VK_SPACE || wParam == VK_RETURN) {
            return 0;
        }
        break;
    case BM_CLICK:
        if (enabled_) {
            ActivateFromUser();
        }
        return 0;
    case BM_SETCHECK: {
        const bool checked = wParam == BST_CHECKED;
        const LRESULT result = ::DefSubclassProc(
            control,
            message,
            checked ? BST_CHECKED : BST_UNCHECKED,
            lParam);
        ApplyCheckedState(checked, true);
        return result;
    }
    case WM_ENABLE:
        enabled_ = wParam != FALSE;
        if (!enabled_) {
            CancelInput();
            hoverMotion_.JumpTo(0.0F);
            pressMotion_.JumpTo(0.0F);
            focusMotion_.JumpTo(0.0F);
        }
        UpdateMotionTimer();
        ::InvalidateRect(control, nullptr, FALSE);
        break;
    case WM_NCDESTROY:
        ::KillTimer(control, kMotionTimerId);
        timerArmed_ = false;
        ReleaseBackBuffer();
        DeleteFonts();
        ::RemoveWindowSubclass(control, &EditorAudioToggle::SubclassProc, subclassId);
        window_ = nullptr;
        parent_ = nullptr;
        instance_ = nullptr;
        keyboardActivationKey_ = 0;
        hovered_ = false;
        trackingMouse_ = false;
        mousePressed_ = false;
        return ::DefSubclassProc(control, message, wParam, lParam);
    default:
        break;
    }
    return ::DefSubclassProc(control, message, wParam, lParam);
}

void EditorAudioToggle::ActivateFromUser() {
    if (!enabled_ || window_ == nullptr) {
        return;
    }
    const bool nextChecked = !checked_;
    ::SendMessageW(
        window_,
        BM_SETCHECK,
        nextChecked ? BST_CHECKED : BST_UNCHECKED,
        0);

    ChangedCallback callback;
    try {
        callback = changedCallback_;
    } catch (...) {
        return;
    }
    if (callback) {
        try {
            callback(nextChecked);
        } catch (...) {
            // Application callbacks must never unwind through the Win32 window procedure.
        }
    }
}

void EditorAudioToggle::ApplyCheckedState(
    const bool checked,
    const bool animate) noexcept {
    const bool changed = checked_ != checked;
    checked_ = checked;
    (void)checkedMotion_.SetTarget(
        checked ? 1.0F : 0.0F,
        kSelectionDuration,
        ui::MotionEasing::EaseOutQuint,
        animate && animationsEnabled_);
    UpdateMotionTimer();
    if (window_ != nullptr) {
        try {
            UpdateAccessibleText();
        } catch (...) {
            // Visual and native checked state remain valid if text allocation fails.
        }
        ::InvalidateRect(window_, nullptr, FALSE);
        if (changed) {
            ::NotifyWinEvent(
                EVENT_OBJECT_STATECHANGE,
                window_,
                OBJID_CLIENT,
                CHILDID_SELF);
        }
    }
}

void EditorAudioToggle::UpdateAccessibleText() {
    if (window_ == nullptr) {
        return;
    }
    std::wstring accessibleText = checked_
        ? kAccessibleEnabled
        : kAccessibleDisabled;
    if (!statusText_.empty()) {
        accessibleText.append(L"。状态：");
        accessibleText.append(statusText_);
    }
    ::SetWindowTextW(window_, accessibleText.c_str());
    ::NotifyWinEvent(
        EVENT_OBJECT_NAMECHANGE,
        window_,
        OBJID_CLIENT,
        CHILDID_SELF);
}

void EditorAudioToggle::RecreateFonts() noexcept {
    DeleteFonts();
    if (window_ == nullptr) {
        return;
    }
    labelFont_ = CreateUiFont(window_, 14, FW_NORMAL);
    const HFONT nativeFont = labelFont_ != nullptr
        ? labelFont_
        : static_cast<HFONT>(::GetStockObject(DEFAULT_GUI_FONT));
    ::SendMessageW(window_, WM_SETFONT, reinterpret_cast<WPARAM>(nativeFont), FALSE);
}

void EditorAudioToggle::DeleteFonts() noexcept {
    if (labelFont_ != nullptr) {
        ::DeleteObject(labelFont_);
        labelFont_ = nullptr;
    }
}

void EditorAudioToggle::PaintWindow() {
    PAINTSTRUCT paint{};
    const HDC targetDc = ::BeginPaint(window_, &paint);
    RECT client{};
    ::GetClientRect(window_, &client);
    const int width = std::max(1, static_cast<int>(client.right - client.left));
    const int height = std::max(1, static_cast<int>(client.bottom - client.top));
    if (!EnsureBackBuffer(targetDc, width, height)) {
        DrawContent(targetDc, client);
        ::EndPaint(window_, &paint);
        return;
    }

    DrawContent(backBufferDc_, client);
    RECT blitBounds = paint.rcPaint;
    if (::IsRectEmpty(&blitBounds) != FALSE) {
        blitBounds = client;
    }
    const int blitWidth = std::max(0, static_cast<int>(blitBounds.right - blitBounds.left));
    const int blitHeight = std::max(0, static_cast<int>(blitBounds.bottom - blitBounds.top));
    if (blitWidth > 0 && blitHeight > 0) {
        ::BitBlt(
            targetDc,
            blitBounds.left,
            blitBounds.top,
            blitWidth,
            blitHeight,
            backBufferDc_,
            blitBounds.left,
            blitBounds.top,
            SRCCOPY);
    }
    ::EndPaint(window_, &paint);
}

void EditorAudioToggle::DrawContent(const HDC dc, const RECT& client) {
    if (dc == nullptr) {
        return;
    }
    FillSolidRectangle(dc, client, editor_theme::Panel);
    if (client.right <= client.left || client.bottom <= client.top) {
        return;
    }

    ui::Canvas canvas(dc);
    RECT shape = client;
    const int outerInset = std::max(1, ScaleForWindow(window_, 1));
    ::InflateRect(&shape, -outerInset, -outerInset);
    const int radius = ScaleForWindow(window_, theme::CornerMedium);

    COLORREF fill = editor_theme::Control;
    COLORREF border = editor_theme::Border;
    COLORREF labelColor = editor_theme::TextPrimary;
    if (!enabled_) {
        fill = editor_theme::ControlDisabled;
        border = editor_theme::BorderDisabled;
        labelColor = editor_theme::TextDisabled;
    } else {
        fill = ui::InterpolateColor(
            fill,
            editor_theme::ControlHover,
            hoverMotion_.Value());
        fill = ui::InterpolateColor(
            fill,
            editor_theme::ControlPressed,
            pressMotion_.Value());
        border = ui::InterpolateColor(
            border,
            editor_theme::BorderHover,
            hoverMotion_.Value());
        border = ui::InterpolateColor(
            border,
            editor_theme::BorderStrong,
            pressMotion_.Value());
    }
    DrawRoundedRectangle(canvas, dc, shape, radius, fill, border);

    const int switchWidth = ScaleForWindow(window_, 40);
    const int switchHeight = ScaleForWindow(window_, 22);
    const int rightPadding = ScaleForWindow(window_, 8);
    const int switchRight = std::max(shape.left, shape.right - rightPadding);
    const int switchLeft = std::max(
        static_cast<int>(shape.left),
        switchRight - switchWidth);
    const int switchTop = client.top +
        (static_cast<int>(client.bottom - client.top) - switchHeight) / 2;
    RECT switchBounds{
        switchLeft,
        switchTop,
        switchRight,
        switchTop + switchHeight};

    const float checkedAmount = checkedMotion_.Value();
    COLORREF trackFill = ui::InterpolateColor(
        editor_theme::ControlPressed,
        editor_theme::Selection,
        checkedAmount);
    COLORREF trackHover = ui::InterpolateColor(
        editor_theme::Control,
        editor_theme::SelectionHover,
        checkedAmount);
    COLORREF trackPressed = ui::InterpolateColor(
        editor_theme::ControlPressed,
        editor_theme::SelectionPressed,
        checkedAmount);
    COLORREF trackBorder = ui::InterpolateColor(
        editor_theme::BorderStrong,
        editor_theme::SelectionBorder,
        checkedAmount);
    COLORREF knobColor = ui::InterpolateColor(
        editor_theme::TextSecondary,
        editor_theme::TextPrimary,
        checkedAmount);
    if (!enabled_) {
        trackFill = editor_theme::ControlDisabled;
        trackHover = trackFill;
        trackPressed = trackFill;
        trackBorder = editor_theme::BorderDisabled;
        knobColor = editor_theme::TextDisabled;
    }
    trackFill = ui::InterpolateColor(trackFill, trackHover, hoverMotion_.Value());
    trackFill = ui::InterpolateColor(trackFill, trackPressed, pressMotion_.Value());
    DrawRoundedRectangle(
        canvas,
        dc,
        switchBounds,
        switchHeight / 2,
        trackFill,
        trackBorder);

    const float knobRadius = ScaleForWindow(window_, 8.0F);
    const float knobInset = ScaleForWindow(window_, 3.0F);
    const float offCenter = static_cast<float>(switchBounds.left) + knobInset + knobRadius;
    const float onCenter = static_cast<float>(switchBounds.right) - knobInset - knobRadius;
    const float knobCenterX = offCenter + (onCenter - offCenter) * checkedAmount;
    const float knobCenterY =
        (static_cast<float>(switchBounds.top) + static_cast<float>(switchBounds.bottom)) * 0.5F;
    if (canvas.Valid()) {
        canvas.FillEllipse(
            knobCenterX,
            knobCenterY,
            knobRadius,
            knobRadius,
            knobColor);
    } else {
        const HBRUSH knobBrush = ::CreateSolidBrush(knobColor);
        const HPEN knobPen = ::CreatePen(PS_SOLID, 1, knobColor);
        if (knobBrush != nullptr && knobPen != nullptr) {
            const HGDIOBJ previousBrush = ::SelectObject(dc, knobBrush);
            const HGDIOBJ previousPen = ::SelectObject(dc, knobPen);
            ::Ellipse(
                dc,
                static_cast<int>(std::lround(knobCenterX - knobRadius)),
                static_cast<int>(std::lround(knobCenterY - knobRadius)),
                static_cast<int>(std::lround(knobCenterX + knobRadius)),
                static_cast<int>(std::lround(knobCenterY + knobRadius)));
            ::SelectObject(dc, previousPen);
            ::SelectObject(dc, previousBrush);
        }
        if (knobPen != nullptr) {
            ::DeleteObject(knobPen);
        }
        if (knobBrush != nullptr) {
            ::DeleteObject(knobBrush);
        }
    }

    const int leftPadding = ScaleForWindow(window_, 10);
    const int switchGap = ScaleForWindow(window_, 6);
    const int textRight = std::max(shape.left, switchBounds.left - switchGap);
    const int labelLeft = std::min(
        textRight,
        static_cast<int>(shape.left) + leftPadding);
    RECT labelBounds{labelLeft, client.top, textRight, client.bottom};
    const HFONT labelFont = labelFont_ != nullptr
        ? labelFont_
        : static_cast<HFONT>(::GetStockObject(DEFAULT_GUI_FONT));
    DrawTextBlock(dc, labelBounds, kLabel, labelFont, labelColor, DT_LEFT);

    const float focusAmount = enabled_ ? focusMotion_.Value() : 0.0F;
    if (focusAmount > 0.001F) {
        if (canvas.Valid()) {
            canvas.StrokeRoundedRectangle(
                shape,
                static_cast<float>(radius),
                editor_theme::Focus,
                static_cast<float>(std::max(1, ScaleForWindow(window_, 2))),
                ui::MotionAlpha(focusAmount));
        } else if (focusAmount >= 0.5F) {
            DrawFallbackRoundedRectangle(
                dc,
                shape,
                radius,
                fill,
                editor_theme::Focus,
                std::max(1, ScaleForWindow(window_, 2)));
        }
    }
}

bool EditorAudioToggle::EnsureBackBuffer(
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
    ReleaseBackBuffer();

    const HDC candidateDc = ::CreateCompatibleDC(targetDc);
    if (candidateDc == nullptr) {
        return false;
    }
    const HBITMAP candidateBitmap = ::CreateCompatibleBitmap(targetDc, width, height);
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
    backBufferWidth_ = width;
    backBufferHeight_ = height;
    return true;
}

void EditorAudioToggle::ReleaseBackBuffer() noexcept {
    if (backBufferDc_ != nullptr && backBufferPreviousBitmap_ != nullptr &&
        backBufferPreviousBitmap_ != HGDI_ERROR) {
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
    backBufferWidth_ = 0;
    backBufferHeight_ = 0;
}

void EditorAudioToggle::SetHoverState(const bool hovered) noexcept {
    if (hovered_ == hovered) {
        return;
    }
    hovered_ = hovered;
    SetMotionTarget(
        hoverMotion_,
        hovered,
        kHoverEnterDuration,
        kHoverExitDuration,
        animationsEnabled_);
    UpdateMotionTimer();
    if (window_ != nullptr) {
        ::InvalidateRect(window_, nullptr, FALSE);
    }
}

void EditorAudioToggle::SetPressState(const bool pressed) noexcept {
    SetMotionTarget(
        pressMotion_,
        pressed,
        kPressEnterDuration,
        kPressExitDuration,
        animationsEnabled_,
        pressed ? ui::MotionEasing::EaseOutQuint : ui::MotionEasing::EaseOutQuart);
    UpdateMotionTimer();
    if (window_ != nullptr) {
        ::InvalidateRect(window_, nullptr, FALSE);
    }
}

void EditorAudioToggle::SetFocusState(const bool focused) noexcept {
    SetMotionTarget(
        focusMotion_,
        focused,
        kFocusEnterDuration,
        kFocusExitDuration,
        animationsEnabled_,
        focused ? ui::MotionEasing::EaseOutQuint : ui::MotionEasing::EaseOutQuart);
    UpdateMotionTimer();
    if (window_ != nullptr) {
        ::InvalidateRect(window_, nullptr, FALSE);
    }
}

void EditorAudioToggle::CancelInput() noexcept {
    keyboardActivationKey_ = 0;
    mousePressed_ = false;
    hovered_ = false;
    if (window_ != nullptr && ::GetCapture() == window_) {
        ::ReleaseCapture();
    }
    if (trackingMouse_ && window_ != nullptr) {
        TRACKMOUSEEVENT tracking{};
        tracking.cbSize = sizeof(tracking);
        tracking.dwFlags = TME_CANCEL | TME_LEAVE;
        tracking.hwndTrack = window_;
        (void)::TrackMouseEvent(&tracking);
    }
    trackingMouse_ = false;
}

void EditorAudioToggle::UpdateMotionTimer() noexcept {
    if (window_ == nullptr) {
        return;
    }
    if (HasActiveMotion()) {
        if (!timerArmed_) {
            timerArmed_ = ::SetTimer(
                window_,
                kMotionTimerId,
                kMotionFrameMilliseconds,
                nullptr) != 0;
            if (!timerArmed_) {
                JumpMotionToTargets();
                ::InvalidateRect(window_, nullptr, FALSE);
            }
        }
    } else if (timerArmed_) {
        ::KillTimer(window_, kMotionTimerId);
        timerArmed_ = false;
    }
}

bool EditorAudioToggle::AdvanceMotion() noexcept {
    if (!animationsEnabled_) {
        JumpMotionToTargets();
        return false;
    }
    const auto now = ui::MotionState::Clock::now();
    const bool hoverActive = hoverMotion_.Advance(now);
    const bool pressActive = pressMotion_.Advance(now);
    const bool focusActive = focusMotion_.Advance(now);
    const bool checkedActive = checkedMotion_.Advance(now);
    return hoverActive || pressActive || focusActive || checkedActive;
}

bool EditorAudioToggle::HasActiveMotion() const noexcept {
    return hoverMotion_.IsActive() || pressMotion_.IsActive() ||
        focusMotion_.IsActive() || checkedMotion_.IsActive();
}

void EditorAudioToggle::JumpMotionToTargets() noexcept {
    hoverMotion_.JumpTo(hoverMotion_.Target());
    pressMotion_.JumpTo(pressMotion_.Target());
    focusMotion_.JumpTo(focusMotion_.Target());
    checkedMotion_.JumpTo(checkedMotion_.Target());
}

}  // namespace qrec
