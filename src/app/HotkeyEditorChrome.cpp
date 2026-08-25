#include "app/HotkeyEditorChrome.h"

#include "app/HotkeyEditorButtons.h"

#include "editor/EditorTheme.h"
#include "ui/AntiAliasedDrawing.h"
#include "ui/Motion.h"
#include "ui/Theme.h"

#include <dwmapi.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <string_view>

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "user32.lib")

namespace qrec {
namespace {

constexpr UINT_PTR kCaptureMotionTimerId = 0x7A13;
constexpr UINT kMotionFrameMilliseconds = 16;

constexpr auto kHoverEnterDuration = std::chrono::milliseconds(160);
constexpr auto kHoverExitDuration = std::chrono::milliseconds(120);

struct DialogLayout final {
    RECT title{};
    RECT subtitle{};
    RECT captureCard{};
    RECT captureLabel{};
    RECT captureValue{};
    RECT captureHint{};
    RECT inlineMessage{};
    RECT separator{};
    RECT defaultButton{};
    RECT cancelButton{};
    RECT saveButton{};
};

[[nodiscard]] int ScaleForWindow(const HWND window, const int value) noexcept {
    const UINT dpi = window != nullptr
        ? ::GetDpiForWindow(window)
        : USER_DEFAULT_SCREEN_DPI;
    return ::MulDiv(value, static_cast<int>(dpi), USER_DEFAULT_SCREEN_DPI);
}

void SetRectangle(
    RECT& rectangle,
    const int left,
    const int top,
    const int width,
    const int height) noexcept {
    rectangle = RECT{left, top, left + std::max(1, width), top + std::max(1, height)};
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
    const COLORREF color,
    const UINT flags) {
    if (dc == nullptr || font == nullptr || text.empty()) {
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
        flags | DT_NOPREFIX);
    ::SetTextColor(dc, previousColor);
    ::SetBkMode(dc, previousMode);
    ::SelectObject(dc, previousFont);
}

[[nodiscard]] HFONT CreateUiFont(
    const HWND window,
    const int pixelHeight,
    const int weight,
    const wchar_t* family) {
    LOGFONTW descriptor{};
    descriptor.lfHeight = -ScaleForWindow(window, pixelHeight);
    descriptor.lfWeight = weight;
    descriptor.lfQuality = theme::FontQuality;
    descriptor.lfOutPrecision = OUT_TT_ONLY_PRECIS;
    descriptor.lfPitchAndFamily = VARIABLE_PITCH | FF_SWISS;
    wcscpy_s(descriptor.lfFaceName, family);
    return ::CreateFontIndirectW(&descriptor);
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

[[nodiscard]] bool ApplyDarkWindowChrome(const HWND window) noexcept {
    constexpr auto immersiveDark = static_cast<DWMWINDOWATTRIBUTE>(20);
    constexpr auto immersiveDarkLegacy = static_cast<DWMWINDOWATTRIBUTE>(19);
    constexpr auto cornerPreference = static_cast<DWMWINDOWATTRIBUTE>(33);
    constexpr auto borderColorAttribute = static_cast<DWMWINDOWATTRIBUTE>(34);
    constexpr auto captionColorAttribute = static_cast<DWMWINDOWATTRIBUTE>(35);
    constexpr auto textColorAttribute = static_cast<DWMWINDOWATTRIBUTE>(36);
    constexpr DWORD roundCorners = 2U;

    const BOOL enabled = TRUE;
    HRESULT immersiveResult = ::DwmSetWindowAttribute(
        window, immersiveDark, &enabled, sizeof(enabled));
    if (FAILED(immersiveResult)) {
        immersiveResult = ::DwmSetWindowAttribute(
            window, immersiveDarkLegacy, &enabled, sizeof(enabled));
    }

    const COLORREF borderColor = editor_theme::BorderSubtle;
    const COLORREF captionColor = editor_theme::TitleBar;
    const COLORREF textColor = editor_theme::TitleBarText;
    static_cast<void>(::DwmSetWindowAttribute(
        window, borderColorAttribute, &borderColor, sizeof(borderColor)));
    static_cast<void>(::DwmSetWindowAttribute(
        window, captionColorAttribute, &captionColor, sizeof(captionColor)));
    static_cast<void>(::DwmSetWindowAttribute(
        window, textColorAttribute, &textColor, sizeof(textColor)));
    const HRESULT cornerResult = ::DwmSetWindowAttribute(
        window, cornerPreference, &roundCorners, sizeof(roundCorners));
    static_cast<void>(::RedrawWindow(
        window, nullptr, nullptr, RDW_FRAME | RDW_INVALIDATE));
    return SUCCEEDED(immersiveResult) && SUCCEEDED(cornerResult);
}

}  // namespace

class HotkeyEditorChrome::Impl final {
public:
    ~Impl() { Shutdown(); }

    [[nodiscard]] bool Initialize(const HINSTANCE instance, const HWND window) {
        Shutdown();
        if (instance == nullptr || window == nullptr || ::IsWindow(window) == FALSE) {
            return false;
        }
        instance_ = instance;
        window_ = window;
        captureHoverMotion_.JumpTo(0.0F);
        if (!RecreateFonts() || !buttons_.Initialize(
                instance_, window_, regularFont_, strongFont_,
                HotkeyEditorChrome::DefaultButtonId,
                HotkeyEditorChrome::CancelButtonId,
                HotkeyEditorChrome::SaveButtonId)) {
            Shutdown();
            return false;
        }
        Layout();
        return true;
    }

    void Shutdown() noexcept {
        if (window_ != nullptr) {
            ::KillTimer(window_, kCaptureMotionTimerId);
        }
        buttons_.Shutdown();
        DeleteFonts();
        layout_ = DialogLayout{};
        captureHoverMotion_.JumpTo(0.0F);
        captureHovered_ = false;
        captureTrackingMouse_ = false;
        useFallbackWindowRegion_ = false;
        instance_ = nullptr;
        window_ = nullptr;
    }

    [[nodiscard]] bool RecreateFonts() {
        DeleteFonts();
        if (window_ == nullptr) {
            return false;
        }
        regularFont_ = CreateUiFont(window_, 14, FW_NORMAL, theme::FontFamily);
        strongFont_ = CreateUiFont(window_, 14, FW_SEMIBOLD, theme::FontFamily);
        titleFont_ = CreateUiFont(window_, 20, FW_SEMIBOLD, theme::TitleFontFamily);
        captionFont_ = CreateUiFont(window_, 12, FW_NORMAL, theme::FontFamily);
        keyFont_ = CreateUiFont(window_, 23, FW_SEMIBOLD, theme::LatinFontFamily);
        buttons_.SetFonts(regularFont_, strongFont_);
        return regularFont_ != nullptr && strongFont_ != nullptr &&
            titleFont_ != nullptr && captionFont_ != nullptr && keyFont_ != nullptr;
    }

    void Layout() {
        if (window_ == nullptr) {
            return;
        }
        RECT client{};
        if (::GetClientRect(window_, &client) == FALSE) {
            return;
        }
        const int width = client.right;
        const int height = client.bottom;
        const int margin = ScaleForWindow(window_, 28);
        const int buttonHeight = ScaleForWindow(window_, 40);
        const int buttonGap = ScaleForWindow(window_, 8);
        const int footerY = height - ScaleForWindow(window_, 24) - buttonHeight;

        SetRectangle(layout_.title, margin, ScaleForWindow(window_, 20),
            width - margin * 2, ScaleForWindow(window_, 29));
        SetRectangle(layout_.subtitle, margin, ScaleForWindow(window_, 53),
            width - margin * 2, ScaleForWindow(window_, 22));
        SetRectangle(layout_.captureCard, margin, ScaleForWindow(window_, 88),
            width - margin * 2, ScaleForWindow(window_, 110));
        SetRectangle(layout_.captureLabel,
            layout_.captureCard.left + ScaleForWindow(window_, 18),
            layout_.captureCard.top + ScaleForWindow(window_, 12),
            layout_.captureCard.right - layout_.captureCard.left - ScaleForWindow(window_, 36),
            ScaleForWindow(window_, 18));
        SetRectangle(layout_.captureValue,
            layout_.captureCard.left + ScaleForWindow(window_, 18),
            layout_.captureCard.top + ScaleForWindow(window_, 36),
            layout_.captureCard.right - layout_.captureCard.left - ScaleForWindow(window_, 36),
            ScaleForWindow(window_, 38));
        SetRectangle(layout_.captureHint,
            layout_.captureCard.left + ScaleForWindow(window_, 18),
            layout_.captureCard.bottom - ScaleForWindow(window_, 29),
            layout_.captureCard.right - layout_.captureCard.left - ScaleForWindow(window_, 36),
            ScaleForWindow(window_, 18));
        SetRectangle(layout_.inlineMessage, margin, ScaleForWindow(window_, 209),
            width - margin * 2, ScaleForWindow(window_, 44));
        SetRectangle(layout_.separator, margin, footerY - ScaleForWindow(window_, 19),
            width - margin * 2, 1);

        const int defaultWidth = ScaleForWindow(window_, 134);
        const int cancelWidth = ScaleForWindow(window_, 82);
        const int saveWidth = ScaleForWindow(window_, 92);
        SetRectangle(layout_.defaultButton, margin, footerY, defaultWidth, buttonHeight);
        SetRectangle(layout_.saveButton,
            width - margin - saveWidth, footerY, saveWidth, buttonHeight);
        SetRectangle(layout_.cancelButton,
            layout_.saveButton.left - buttonGap - cancelWidth,
            footerY, cancelWidth, buttonHeight);

        buttons_.Layout(
            layout_.defaultButton, layout_.cancelButton, layout_.saveButton);
        InvalidateAll();
    }

    void RefreshWindowChrome() {
        if (window_ == nullptr) {
            return;
        }
        const bool previouslyFallback = useFallbackWindowRegion_;
        useFallbackWindowRegion_ = !ApplyDarkWindowChrome(window_);
        if (previouslyFallback && !useFallbackWindowRegion_) {
            static_cast<void>(::SetWindowRgn(window_, nullptr, TRUE));
        }
        UpdateWindowRegion();
        InvalidateAll();
    }

    void UpdateWindowRegion() const noexcept {
        if (window_ == nullptr || !useFallbackWindowRegion_) {
            return;
        }
        RECT bounds{};
        if (::GetWindowRect(window_, &bounds) == FALSE) {
            return;
        }
        const int width = std::max(1, static_cast<int>(bounds.right - bounds.left));
        const int height = std::max(1, static_cast<int>(bounds.bottom - bounds.top));
        const int diameter = ScaleForWindow(window_, 16) * 2;
        const HRGN region = ::CreateRoundRectRgn(
            0, 0, width + 1, height + 1, diameter, diameter);
        if (region != nullptr && ::SetWindowRgn(window_, region, TRUE) == 0) {
            ::DeleteObject(region);
        }
    }

    void Paint(const HotkeyEditorChromeView& view) const {
        if (window_ == nullptr) {
            return;
        }
        PAINTSTRUCT paint{};
        const HDC target = ::BeginPaint(window_, &paint);
        if (target == nullptr) {
            ::EndPaint(window_, &paint);
            return;
        }
        RECT client{};
        ::GetClientRect(window_, &client);
        const int width = std::max(1, static_cast<int>(client.right - client.left));
        const int height = std::max(1, static_cast<int>(client.bottom - client.top));
        const HDC buffer = ::CreateCompatibleDC(target);
        const HBITMAP bitmap = buffer != nullptr
            ? ::CreateCompatibleBitmap(target, width, height)
            : nullptr;
        HGDIOBJ previousBitmap = nullptr;
        HDC drawing = target;
        if (buffer != nullptr && bitmap != nullptr) {
            previousBitmap = ::SelectObject(buffer, bitmap);
            drawing = buffer;
        }

        FillSolidRectangle(drawing, client, editor_theme::Canvas);
        ui::Canvas canvas(drawing);
        const COLORREF cardBase = view.capturing ? RGB(32, 40, 36) : editor_theme::Control;
        const COLORREF cardHover = view.capturing ? RGB(37, 48, 41) : editor_theme::ControlHover;
        const COLORREF cardFill = ui::InterpolateColor(
            cardBase, cardHover, captureHoverMotion_.Value());
        COLORREF cardBorder = editor_theme::Border;
        if (view.tone == HotkeyEditorTone::Error) {
            cardBorder = editor_theme::Danger;
        } else if (view.capturing || view.tone == HotkeyEditorTone::Success) {
            cardBorder = editor_theme::Mp4SelectedBorder;
        }

        if (canvas.Valid()) {
            canvas.DrawRoundedRectangle(
                layout_.captureCard,
                static_cast<float>(ScaleForWindow(window_, 14)),
                cardFill,
                cardBorder,
                static_cast<float>(ScaleForWindow(window_, view.capturing ? 2 : 1)));
            canvas.DrawLine(
                static_cast<float>(layout_.separator.left),
                static_cast<float>(layout_.separator.top),
                static_cast<float>(layout_.separator.right),
                static_cast<float>(layout_.separator.top),
                editor_theme::BorderSubtle,
                1.0F);
            const COLORREF dotColor = view.tone == HotkeyEditorTone::Error
                ? editor_theme::Danger
                : (view.capturing || view.tone == HotkeyEditorTone::Success
                    ? editor_theme::Success
                    : editor_theme::TextSecondary);
            canvas.FillEllipse(
                static_cast<float>(layout_.captureLabel.left + ScaleForWindow(window_, 4)),
                static_cast<float>((layout_.captureLabel.top + layout_.captureLabel.bottom) / 2),
                static_cast<float>(ScaleForWindow(window_, 4)),
                static_cast<float>(ScaleForWindow(window_, 4)),
                dotColor);
        } else {
            DrawFallbackRoundedRectangle(
                drawing, layout_.captureCard, ScaleForWindow(window_, 28),
                cardFill, cardBorder);
        }

        DrawTextBlock(drawing, layout_.title, L"录制快捷键", titleFont_,
            editor_theme::TextPrimary, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        DrawTextBlock(drawing, layout_.subtitle,
            L"按下新的组合键，确认无冲突后保存。", regularFont_,
            editor_theme::TextSecondary, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        RECT captureLabelText = layout_.captureLabel;
        captureLabelText.left += ScaleForWindow(window_, 16);
        DrawTextBlock(drawing, captureLabelText,
            view.capturing ? L"正在捕获" : L"已选择", captionFont_,
            view.capturing ? editor_theme::Success : editor_theme::TextSecondary,
            DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        DrawTextBlock(drawing, layout_.captureValue, view.displayText, keyFont_,
            editor_theme::TextPrimary,
            DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        DrawTextBlock(drawing, layout_.captureHint,
            view.capturing
                ? L"Esc 取消 · 支持 F1–F24（F12 除外）"
                : L"点击此区域可重新捕获",
            captionFont_, editor_theme::TextSecondary,
            DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        const COLORREF messageColor = view.tone == HotkeyEditorTone::Error
            ? editor_theme::DangerText
            : (view.tone == HotkeyEditorTone::Success
                ? editor_theme::Success
                : editor_theme::TextSecondary);
        DrawTextBlock(drawing, layout_.inlineMessage, view.inlineMessage, captionFont_,
            messageColor, DT_LEFT | DT_WORDBREAK | DT_END_ELLIPSIS);

        if (drawing == buffer) {
            ::BitBlt(target, 0, 0, width, height, buffer, 0, 0, SRCCOPY);
            ::SelectObject(buffer, previousBitmap);
        }
        if (bitmap != nullptr) {
            ::DeleteObject(bitmap);
        }
        if (buffer != nullptr) {
            ::DeleteDC(buffer);
        }
        ::EndPaint(window_, &paint);
    }

    [[nodiscard]] bool DrawButton(const DRAWITEMSTRUCT* item) const {
        return buttons_.Draw(item);
    }

    void UpdateCaptureHover(const POINT point) {
        SetCaptureHovered(HitTestCaptureCard(point));
        if (window_ != nullptr && !captureTrackingMouse_) {
            TRACKMOUSEEVENT tracking{};
            tracking.cbSize = sizeof(tracking);
            tracking.dwFlags = TME_LEAVE;
            tracking.hwndTrack = window_;
            captureTrackingMouse_ = ::TrackMouseEvent(&tracking) != FALSE;
        }
    }

    void ClearCaptureHover() {
        captureTrackingMouse_ = false;
        SetCaptureHovered(false);
    }

    [[nodiscard]] bool HandleTimer(const UINT_PTR timerId) {
        if (timerId != kCaptureMotionTimerId || window_ == nullptr) {
            return false;
        }
        const bool active = captureHoverMotion_.Advance();
        InvalidateCaptureCard();
        if (!active) {
            ::KillTimer(window_, kCaptureMotionTimerId);
        }
        return true;
    }

    [[nodiscard]] bool HitTestCaptureCard(const POINT point) const noexcept {
        return window_ != nullptr && ::PtInRect(&layout_.captureCard, point) != FALSE;
    }

    [[nodiscard]] bool SetCaptureCardCursor(const LPARAM hitTestData) const noexcept {
        if (window_ == nullptr || LOWORD(hitTestData) != HTCLIENT) {
            return false;
        }
        POINT cursor{};
        if (::GetCursorPos(&cursor) == FALSE ||
            ::ScreenToClient(window_, &cursor) == FALSE ||
            !HitTestCaptureCard(cursor)) {
            return false;
        }
        ::SetCursor(::LoadCursorW(nullptr, IDC_HAND));
        return true;
    }

    [[nodiscard]] HWND DefaultButton() const noexcept { return buttons_.DefaultButton(); }
    [[nodiscard]] HWND CancelButton() const noexcept { return buttons_.CancelButton(); }
    [[nodiscard]] HWND SaveButton() const noexcept { return buttons_.SaveButton(); }
    void SetSaveEnabled(const bool enabled) const noexcept {
        buttons_.SetSaveEnabled(enabled);
    }
    void FocusFirstButton() const noexcept { buttons_.FocusFirst(); }
    void FocusSaveButton() const noexcept { buttons_.FocusSave(); }

    void InvalidateAll() const noexcept {
        if (window_ != nullptr) {
            ::InvalidateRect(window_, nullptr, FALSE);
        }
    }

    void InvalidateCaptureCard() const noexcept {
        if (window_ != nullptr) {
            ::InvalidateRect(window_, &layout_.captureCard, FALSE);
        }
    }

    void InvalidateInlineMessage() const noexcept {
        if (window_ != nullptr) {
            ::InvalidateRect(window_, &layout_.inlineMessage, FALSE);
        }
    }

private:
    void DeleteFonts() noexcept {
        const std::array fonts{
            regularFont_, strongFont_, titleFont_, captionFont_, keyFont_};
        for (const HFONT font : fonts) {
            if (font != nullptr) {
                ::DeleteObject(font);
            }
        }
        regularFont_ = nullptr;
        strongFont_ = nullptr;
        titleFont_ = nullptr;
        captionFont_ = nullptr;
        keyFont_ = nullptr;
    }

    void SetCaptureHovered(const bool hovered) {
        if (captureHovered_ == hovered) {
            return;
        }
        captureHovered_ = hovered;
        SetMotionTarget(captureHoverMotion_, hovered,
            kHoverEnterDuration, kHoverExitDuration);
        if (captureHoverMotion_.IsActive()) {
            if (::SetTimer(window_, kCaptureMotionTimerId,
                    kMotionFrameMilliseconds, nullptr) == 0) {
                captureHoverMotion_.JumpTo(captureHoverMotion_.Target());
            }
        } else if (window_ != nullptr) {
            ::KillTimer(window_, kCaptureMotionTimerId);
        }
        InvalidateCaptureCard();
    }

    HINSTANCE instance_{};
    HWND window_{};
    HotkeyEditorButtons buttons_{};
    DialogLayout layout_{};
    ui::MotionState captureHoverMotion_{};
    bool captureHovered_{};
    bool captureTrackingMouse_{};
    bool useFallbackWindowRegion_{};
    HFONT regularFont_{};
    HFONT strongFont_{};
    HFONT titleFont_{};
    HFONT captionFont_{};
    HFONT keyFont_{};
};

HotkeyEditorChrome::HotkeyEditorChrome() : impl_(std::make_unique<Impl>()) {}
HotkeyEditorChrome::~HotkeyEditorChrome() = default;

bool HotkeyEditorChrome::Initialize(const HINSTANCE instance, const HWND window) {
    return impl_->Initialize(instance, window);
}

void HotkeyEditorChrome::Shutdown() noexcept { impl_->Shutdown(); }
bool HotkeyEditorChrome::RecreateFonts() { return impl_->RecreateFonts(); }
void HotkeyEditorChrome::Layout() { impl_->Layout(); }
void HotkeyEditorChrome::RefreshWindowChrome() { impl_->RefreshWindowChrome(); }
void HotkeyEditorChrome::UpdateWindowRegion() const noexcept { impl_->UpdateWindowRegion(); }

void HotkeyEditorChrome::Paint(const HotkeyEditorChromeView& view) const {
    impl_->Paint(view);
}

bool HotkeyEditorChrome::DrawButton(const DRAWITEMSTRUCT* item) const {
    return impl_->DrawButton(item);
}

void HotkeyEditorChrome::UpdateCaptureHover(const POINT point) {
    impl_->UpdateCaptureHover(point);
}

void HotkeyEditorChrome::ClearCaptureHover() { impl_->ClearCaptureHover(); }

bool HotkeyEditorChrome::HandleTimer(const UINT_PTR timerId) {
    return impl_->HandleTimer(timerId);
}

bool HotkeyEditorChrome::HitTestCaptureCard(const POINT point) const noexcept {
    return impl_->HitTestCaptureCard(point);
}

bool HotkeyEditorChrome::SetCaptureCardCursor(const LPARAM hitTestData) const noexcept {
    return impl_->SetCaptureCardCursor(hitTestData);
}

HWND HotkeyEditorChrome::DefaultButton() const noexcept { return impl_->DefaultButton(); }
HWND HotkeyEditorChrome::CancelButton() const noexcept { return impl_->CancelButton(); }
HWND HotkeyEditorChrome::SaveButton() const noexcept { return impl_->SaveButton(); }

void HotkeyEditorChrome::SetSaveEnabled(const bool enabled) const noexcept {
    impl_->SetSaveEnabled(enabled);
}

void HotkeyEditorChrome::FocusFirstButton() const noexcept { impl_->FocusFirstButton(); }
void HotkeyEditorChrome::FocusSaveButton() const noexcept { impl_->FocusSaveButton(); }
void HotkeyEditorChrome::InvalidateAll() const noexcept { impl_->InvalidateAll(); }
void HotkeyEditorChrome::InvalidateCaptureCard() const noexcept {
    impl_->InvalidateCaptureCard();
}
void HotkeyEditorChrome::InvalidateInlineMessage() const noexcept {
    impl_->InvalidateInlineMessage();
}

}  // namespace qrec
