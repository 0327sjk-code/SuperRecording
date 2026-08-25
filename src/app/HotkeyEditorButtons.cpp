#include "app/HotkeyEditorButtons.h"

#include "editor/EditorTheme.h"
#include "ui/AntiAliasedDrawing.h"
#include "ui/Motion.h"

#include <commctrl.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <string_view>

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "user32.lib")

namespace qrec {
namespace {

constexpr UINT_PTR kSubclassId = 0x7A11;
constexpr UINT_PTR kMotionTimerId = 0x7A12;
constexpr UINT kMotionFrameMilliseconds = 16;
constexpr auto kHoverEnter = std::chrono::milliseconds(160);
constexpr auto kHoverExit = std::chrono::milliseconds(120);
constexpr auto kPressEnter = std::chrono::milliseconds(90);
constexpr auto kPressExit = std::chrono::milliseconds(130);
constexpr auto kFocusEnter = std::chrono::milliseconds(170);
constexpr auto kFocusExit = std::chrono::milliseconds(120);

enum class ButtonRole : unsigned char {
    Ghost,
    Secondary,
    Primary,
};

struct ButtonState final {
    HWND handle{};
    ButtonRole role{ButtonRole::Secondary};
    ui::MotionState hover{};
    ui::MotionState press{};
    ui::MotionState focus{};
    bool hovered{};
    bool trackingMouse{};
};

[[nodiscard]] int ScaleForWindow(const HWND window, const int value) noexcept {
    const UINT dpi = window != nullptr
        ? ::GetDpiForWindow(window)
        : USER_DEFAULT_SCREEN_DPI;
    return ::MulDiv(value, static_cast<int>(dpi), USER_DEFAULT_SCREEN_DPI);
}

void SetMotionTarget(
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

[[nodiscard]] bool AdvanceMotion(ButtonState& state) noexcept {
    if (!ui::ClientAreaAnimationsEnabled()) {
        state.hover.JumpTo(state.hover.Target());
        state.press.JumpTo(state.press.Target());
        state.focus.JumpTo(state.focus.Target());
        return false;
    }
    const auto now = ui::MotionState::Clock::now();
    const bool hoverActive = state.hover.Advance(now);
    const bool pressActive = state.press.Advance(now);
    const bool focusActive = state.focus.Advance(now);
    return hoverActive || pressActive || focusActive;
}

[[nodiscard]] bool HasActiveMotion(const ButtonState& state) noexcept {
    return state.hover.IsActive() || state.press.IsActive() || state.focus.IsActive();
}

void FillSolidRectangle(const HDC dc, const RECT& bounds, const COLORREF color) {
    const HBRUSH brush = ::CreateSolidBrush(color);
    if (brush != nullptr) {
        ::FillRect(dc, &bounds, brush);
        ::DeleteObject(brush);
    }
}

void DrawFallbackRoundedRectangle(
    const HDC dc,
    const RECT& bounds,
    const int diameter,
    const COLORREF fill,
    const COLORREF border) {
    const HBRUSH brush = ::CreateSolidBrush(fill);
    const HPEN pen = ::CreatePen(PS_SOLID, 1, border);
    if (brush != nullptr && pen != nullptr) {
        const HGDIOBJ previousBrush = ::SelectObject(dc, brush);
        const HGDIOBJ previousPen = ::SelectObject(dc, pen);
        ::RoundRect(dc, bounds.left, bounds.top, bounds.right, bounds.bottom,
            diameter, diameter);
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

void DrawTextBlock(
    const HDC dc,
    const RECT& bounds,
    const std::wstring_view text,
    const HFONT font,
    const COLORREF color) {
    if (dc == nullptr || font == nullptr || text.empty()) {
        return;
    }
    const HGDIOBJ previousFont = ::SelectObject(dc, font);
    const int previousMode = ::SetBkMode(dc, TRANSPARENT);
    const COLORREF previousColor = ::SetTextColor(dc, color);
    RECT textBounds = bounds;
    ::DrawTextW(dc, text.data(), static_cast<int>(text.size()), &textBounds,
        DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
    ::SetTextColor(dc, previousColor);
    ::SetBkMode(dc, previousMode);
    ::SelectObject(dc, previousFont);
}

}  // namespace

class HotkeyEditorButtons::Impl final {
public:
    ~Impl() { Shutdown(); }

    [[nodiscard]] bool Initialize(
        const HINSTANCE instance,
        const HWND parent,
        const HFONT regularFont,
        const HFONT strongFont,
        const int defaultButtonId,
        const int cancelButtonId,
        const int saveButtonId) {
        Shutdown();
        if (instance == nullptr || parent == nullptr || ::IsWindow(parent) == FALSE) {
            return false;
        }
        instance_ = instance;
        parent_ = parent;
        regularFont_ = regularFont;
        strongFont_ = strongFont;
        states_[0].handle = CreateButton(
            L"恢复默认 F3", defaultButtonId, ButtonRole::Ghost, states_[0]);
        states_[1].handle = CreateButton(
            L"取消", cancelButtonId, ButtonRole::Secondary, states_[1]);
        states_[2].handle = CreateButton(
            L"保存", saveButtonId, ButtonRole::Primary, states_[2]);
        if (DefaultButton() == nullptr || CancelButton() == nullptr || SaveButton() == nullptr) {
            Shutdown();
            return false;
        }
        ApplyFonts();
        return true;
    }

    void Shutdown() noexcept {
        for (ButtonState& state : states_) {
            if (state.handle != nullptr && ::IsWindow(state.handle) != FALSE) {
                ::KillTimer(state.handle, kMotionTimerId);
                ::RemoveWindowSubclass(state.handle, &Impl::SubclassProc, kSubclassId);
                ::DestroyWindow(state.handle);
            }
            state = ButtonState{};
        }
        instance_ = nullptr;
        parent_ = nullptr;
        regularFont_ = nullptr;
        strongFont_ = nullptr;
    }

    void SetFonts(const HFONT regularFont, const HFONT strongFont) noexcept {
        regularFont_ = regularFont;
        strongFont_ = strongFont;
        ApplyFonts();
    }

    void Layout(
        const RECT& defaultBounds,
        const RECT& cancelBounds,
        const RECT& saveBounds) const noexcept {
        Position(DefaultButton(), defaultBounds);
        Position(CancelButton(), cancelBounds);
        Position(SaveButton(), saveBounds);
    }

    [[nodiscard]] bool Draw(const DRAWITEMSTRUCT* item) const {
        if (item == nullptr || item->CtlType != ODT_BUTTON) {
            return false;
        }
        const ButtonState* state = FindState(item->hwndItem);
        if (state == nullptr) {
            return false;
        }
        RECT bounds = item->rcItem;
        FillSolidRectangle(item->hDC, bounds, editor_theme::Canvas);
        const bool enabled = (item->itemState & ODS_DISABLED) == 0;
        COLORREF fill = editor_theme::Control;
        COLORREF hover = editor_theme::ControlHover;
        COLORREF pressed = editor_theme::ControlPressed;
        COLORREF border = editor_theme::Border;
        COLORREF text = editor_theme::TextPrimary;
        if (state->role == ButtonRole::Primary) {
            fill = editor_theme::Save;
            hover = editor_theme::SaveHover;
            pressed = editor_theme::SavePressed;
            border = editor_theme::Save;
            text = editor_theme::White;
        } else if (state->role == ButtonRole::Ghost) {
            fill = editor_theme::Canvas;
            hover = editor_theme::Control;
            pressed = editor_theme::ControlPressed;
            text = editor_theme::TextSecondary;
        }
        if (!enabled) {
            fill = editor_theme::ControlDisabled;
            hover = fill;
            pressed = fill;
            border = editor_theme::BorderDisabled;
            text = editor_theme::TextDisabled;
        }
        COLORREF animatedFill = ui::InterpolateColor(fill, hover, state->hover.Value());
        animatedFill = ui::InterpolateColor(animatedFill, pressed, state->press.Value());
        const COLORREF animatedBorder = enabled
            ? ui::InterpolateColor(border, editor_theme::BorderHover, state->hover.Value())
            : border;

        ui::Canvas canvas(item->hDC);
        if (canvas.Valid()) {
            canvas.DrawRoundedRectangle(bounds,
                static_cast<float>(ScaleForWindow(parent_, 10)),
                animatedFill, animatedBorder, 1.0F);
            const float focusAmount = std::max(
                state->focus.Value(),
                (item->itemState & ODS_FOCUS) != 0 ? 1.0F : 0.0F);
            if (focusAmount > 0.001F) {
                canvas.StrokeRoundedRectangle(bounds,
                    static_cast<float>(ScaleForWindow(parent_, 10)),
                    editor_theme::Focus,
                    static_cast<float>(ScaleForWindow(parent_, 2)),
                    ui::MotionAlpha(focusAmount));
            }
        } else {
            DrawFallbackRoundedRectangle(item->hDC, bounds,
                ScaleForWindow(parent_, 20), animatedFill, animatedBorder);
        }

        std::array<wchar_t, 128> label{};
        const int length = ::GetWindowTextW(
            item->hwndItem, label.data(), static_cast<int>(label.size()));
        const HFONT font = state->role == ButtonRole::Primary
            ? strongFont_
            : regularFont_;
        DrawTextBlock(item->hDC, bounds,
            std::wstring_view(label.data(), static_cast<std::size_t>(std::max(0, length))),
            font, text);
        return true;
    }

    [[nodiscard]] HWND DefaultButton() const noexcept { return states_[0].handle; }
    [[nodiscard]] HWND CancelButton() const noexcept { return states_[1].handle; }
    [[nodiscard]] HWND SaveButton() const noexcept { return states_[2].handle; }

    void SetSaveEnabled(const bool enabled) const noexcept {
        if (SaveButton() != nullptr) {
            ::EnableWindow(SaveButton(), enabled ? TRUE : FALSE);
        }
    }

    void FocusFirst() const noexcept {
        if (DefaultButton() != nullptr) {
            ::SetFocus(DefaultButton());
        }
    }

    void FocusSave() const noexcept {
        if (SaveButton() != nullptr) {
            ::SetFocus(SaveButton());
        }
    }

private:
    [[nodiscard]] HWND CreateButton(
        const wchar_t* text,
        const int id,
        const ButtonRole role,
        ButtonState& state) {
        const HWND button = ::CreateWindowExW(
            0, WC_BUTTONW, text,
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
            0, 0, 1, 1, parent_,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), instance_, nullptr);
        if (button == nullptr) {
            return nullptr;
        }
        state.handle = button;
        state.role = role;
        if (::SetWindowSubclass(button, &Impl::SubclassProc, kSubclassId,
                reinterpret_cast<DWORD_PTR>(this)) == FALSE) {
            ::DestroyWindow(button);
            state.handle = nullptr;
            return nullptr;
        }
        return button;
    }

    void ApplyFonts() const noexcept {
        for (const ButtonState& state : states_) {
            if (state.handle != nullptr) {
                const HFONT font = state.role == ButtonRole::Primary
                    ? strongFont_
                    : regularFont_;
                ::SendMessageW(state.handle, WM_SETFONT,
                    reinterpret_cast<WPARAM>(font), TRUE);
            }
        }
    }

    static LRESULT CALLBACK SubclassProc(
        const HWND control,
        const UINT message,
        const WPARAM wParam,
        const LPARAM lParam,
        const UINT_PTR subclassId,
        const DWORD_PTR referenceData) noexcept {
        auto* self = reinterpret_cast<Impl*>(referenceData);
        if (self == nullptr) {
            return ::DefSubclassProc(control, message, wParam, lParam);
        }
        try {
            return self->HandleMessage(control, message, wParam, lParam, subclassId);
        } catch (...) {
            return 0;
        }
    }

    LRESULT HandleMessage(
        const HWND control,
        const UINT message,
        const WPARAM wParam,
        const LPARAM lParam,
        const UINT_PTR subclassId) {
        ButtonState* state = FindState(control);
        if (state == nullptr) {
            return ::DefSubclassProc(control, message, wParam, lParam);
        }
        switch (message) {
        case WM_MOUSEMOVE:
            HandleMouseMove(*state);
            break;
        case WM_MOUSELEAVE:
            state->trackingMouse = false;
            state->hovered = false;
            SetMotionTarget(state->hover, false, kHoverEnter, kHoverExit);
            UpdateTimer(*state);
            ::InvalidateRect(control, nullptr, FALSE);
            break;
        case WM_LBUTTONDOWN:
            SetMotionTarget(state->press, true, kPressEnter, kPressExit,
                ui::MotionEasing::EaseOutQuint);
            UpdateTimer(*state);
            ::InvalidateRect(control, nullptr, FALSE);
            break;
        case WM_LBUTTONUP:
        case WM_CAPTURECHANGED:
        case WM_CANCELMODE:
            SetMotionTarget(state->press, false, kPressEnter, kPressExit);
            UpdateTimer(*state);
            ::InvalidateRect(control, nullptr, FALSE);
            break;
        case WM_SETFOCUS:
            SetMotionTarget(state->focus, true, kFocusEnter, kFocusExit,
                ui::MotionEasing::EaseOutQuint);
            UpdateTimer(*state);
            ::InvalidateRect(control, nullptr, FALSE);
            break;
        case WM_KILLFOCUS:
            SetMotionTarget(state->focus, false, kFocusEnter, kFocusExit);
            UpdateTimer(*state);
            ::InvalidateRect(control, nullptr, FALSE);
            break;
        case WM_KEYDOWN:
            if (HandleKeyDown(control, wParam)) {
                return 0;
            }
            break;
        case WM_TIMER:
            if (wParam == kMotionTimerId) {
                const bool active = AdvanceMotion(*state);
                ::InvalidateRect(control, nullptr, FALSE);
                if (!active) {
                    ::KillTimer(control, kMotionTimerId);
                }
                return 0;
            }
            break;
        case WM_ERASEBKGND:
            return 1;
        case WM_NCDESTROY:
            ::KillTimer(control, kMotionTimerId);
            ::RemoveWindowSubclass(control, &Impl::SubclassProc, subclassId);
            state->handle = nullptr;
            break;
        default:
            break;
        }
        return ::DefSubclassProc(control, message, wParam, lParam);
    }

    void HandleMouseMove(ButtonState& state) const {
        if (!state.hovered) {
            state.hovered = true;
            SetMotionTarget(state.hover, true, kHoverEnter, kHoverExit);
            UpdateTimer(state);
            ::InvalidateRect(state.handle, nullptr, FALSE);
        }
        if (!state.trackingMouse) {
            TRACKMOUSEEVENT tracking{};
            tracking.cbSize = sizeof(tracking);
            tracking.dwFlags = TME_LEAVE;
            tracking.hwndTrack = state.handle;
            state.trackingMouse = ::TrackMouseEvent(&tracking) != FALSE;
        }
    }

    [[nodiscard]] bool HandleKeyDown(const HWND control, const WPARAM key) const noexcept {
        if (key == VK_ESCAPE) {
            ::SendMessageW(parent_, WM_CLOSE, 0, 0);
            return true;
        }
        if (key == VK_TAB) {
            FocusAdjacent(control, (::GetKeyState(VK_SHIFT) & 0x8000) == 0 ? 1 : -1);
            return true;
        }
        if (key == VK_RETURN) {
            ::SendMessageW(control, BM_CLICK, 0, 0);
            return true;
        }
        return false;
    }

    void UpdateTimer(ButtonState& state) const noexcept {
        if (state.handle == nullptr) {
            return;
        }
        if (HasActiveMotion(state)) {
            if (::SetTimer(state.handle, kMotionTimerId,
                    kMotionFrameMilliseconds, nullptr) == 0) {
                state.hover.JumpTo(state.hover.Target());
                state.press.JumpTo(state.press.Target());
                state.focus.JumpTo(state.focus.Target());
                ::InvalidateRect(state.handle, nullptr, FALSE);
            }
        } else {
            ::KillTimer(state.handle, kMotionTimerId);
        }
    }

    void FocusAdjacent(const HWND current, const int direction) const noexcept {
        const std::array<HWND, 3> order{DefaultButton(), CancelButton(), SaveButton()};
        int index = 0;
        for (int candidate = 0; candidate < static_cast<int>(order.size()); ++candidate) {
            if (order[static_cast<std::size_t>(candidate)] == current) {
                index = candidate;
                break;
            }
        }
        for (int attempt = 0; attempt < static_cast<int>(order.size()); ++attempt) {
            index = (index + direction + static_cast<int>(order.size())) %
                static_cast<int>(order.size());
            const HWND target = order[static_cast<std::size_t>(index)];
            if (target != nullptr && ::IsWindowEnabled(target) != FALSE) {
                ::SetFocus(target);
                return;
            }
        }
    }

    [[nodiscard]] ButtonState* FindState(const HWND handle) noexcept {
        for (ButtonState& state : states_) {
            if (state.handle == handle) {
                return &state;
            }
        }
        return nullptr;
    }

    [[nodiscard]] const ButtonState* FindState(const HWND handle) const noexcept {
        for (const ButtonState& state : states_) {
            if (state.handle == handle) {
                return &state;
            }
        }
        return nullptr;
    }

    static void Position(const HWND button, const RECT& bounds) noexcept {
        if (button != nullptr) {
            ::SetWindowPos(button, nullptr, bounds.left, bounds.top,
                bounds.right - bounds.left, bounds.bottom - bounds.top,
                SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOCOPYBITS);
        }
    }

    HINSTANCE instance_{};
    HWND parent_{};
    std::array<ButtonState, 3> states_{};
    HFONT regularFont_{};
    HFONT strongFont_{};
};

HotkeyEditorButtons::HotkeyEditorButtons() : impl_(std::make_unique<Impl>()) {}
HotkeyEditorButtons::~HotkeyEditorButtons() = default;

bool HotkeyEditorButtons::Initialize(
    const HINSTANCE instance,
    const HWND parent,
    const HFONT regularFont,
    const HFONT strongFont,
    const int defaultButtonId,
    const int cancelButtonId,
    const int saveButtonId) {
    return impl_->Initialize(instance, parent, regularFont, strongFont,
        defaultButtonId, cancelButtonId, saveButtonId);
}

void HotkeyEditorButtons::Shutdown() noexcept { impl_->Shutdown(); }

void HotkeyEditorButtons::SetFonts(
    const HFONT regularFont,
    const HFONT strongFont) const noexcept {
    impl_->SetFonts(regularFont, strongFont);
}

void HotkeyEditorButtons::Layout(
    const RECT& defaultBounds,
    const RECT& cancelBounds,
    const RECT& saveBounds) const noexcept {
    impl_->Layout(defaultBounds, cancelBounds, saveBounds);
}

bool HotkeyEditorButtons::Draw(const DRAWITEMSTRUCT* item) const {
    return impl_->Draw(item);
}

HWND HotkeyEditorButtons::DefaultButton() const noexcept { return impl_->DefaultButton(); }
HWND HotkeyEditorButtons::CancelButton() const noexcept { return impl_->CancelButton(); }
HWND HotkeyEditorButtons::SaveButton() const noexcept { return impl_->SaveButton(); }
void HotkeyEditorButtons::SetSaveEnabled(const bool enabled) const noexcept {
    impl_->SetSaveEnabled(enabled);
}
void HotkeyEditorButtons::FocusFirst() const noexcept { impl_->FocusFirst(); }
void HotkeyEditorButtons::FocusSave() const noexcept { impl_->FocusSave(); }

}  // namespace qrec
