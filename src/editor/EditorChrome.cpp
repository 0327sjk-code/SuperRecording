#include "editor/EditorChrome.h"

#include "editor/EditorAudioToggle.h"
#include "editor/EditorSpeedControl.h"
#include "editor/EditorTheme.h"

#include "ui/AntiAliasedDrawing.h"
#include "ui/Motion.h"
#include "ui/Theme.h"

#include <commctrl.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <chrono>
#include <new>
#include <string>
#include <utility>

namespace qrec {
namespace {

constexpr UINT_PTR kButtonSubclassId = 0x7101;
constexpr UINT_PTR kButtonMotionTimerId = 0x7102;
constexpr UINT kMotionFrameMilliseconds = 16;
constexpr int kHeaderHeight = 68;

constexpr auto kHoverEnterDuration = std::chrono::milliseconds(160);
constexpr auto kHoverExitDuration = std::chrono::milliseconds(120);
constexpr auto kPressEnterDuration = std::chrono::milliseconds(100);
constexpr auto kPressExitDuration = std::chrono::milliseconds(140);
constexpr auto kFocusEnterDuration = std::chrono::milliseconds(180);
constexpr auto kFocusExitDuration = std::chrono::milliseconds(120);

struct ButtonMotionState final {
    ui::MotionState hover{};
    ui::MotionState press{};
    ui::MotionState focus{};
    std::wstring cachedLabel;
    SIZE cachedLabelSize{};
    HFONT cachedLabelFont{};
    UINT cachedLabelDpi{};
    bool hovered{};
    bool trackingMouse{};
    bool timerArmed{};
    bool labelMetricsValid{};
};

void RequestButtonRedraw(const HWND control) noexcept {
    if (control == nullptr) {
        return;
    }
    ::InvalidateRect(control, nullptr, FALSE);
}

void SetMotionTarget(
    ui::MotionState& motion,
    const bool active,
    const std::chrono::milliseconds enterDuration,
    const std::chrono::milliseconds exitDuration,
    const ui::MotionEasing easing,
    const bool animationsEnabled) noexcept {
    (void)motion.SetTarget(
        active ? 1.0F : 0.0F,
        active ? enterDuration : exitDuration,
        easing,
        animationsEnabled);
}

void SetMotionTarget(
    ui::MotionState& motion,
    const bool active,
    const std::chrono::milliseconds enterDuration,
    const std::chrono::milliseconds exitDuration,
    const ui::MotionEasing easing = ui::MotionEasing::EaseOutQuart) noexcept {
    SetMotionTarget(
        motion,
        active,
        enterDuration,
        exitDuration,
        easing,
        ui::ClientAreaAnimationsEnabled());
}

[[nodiscard]] bool AdvanceButtonMotion(
    ButtonMotionState& state,
    const bool animationsEnabled) noexcept {
    if (!animationsEnabled) {
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

[[nodiscard]] bool AdvanceButtonMotion(ButtonMotionState& state) noexcept {
    return AdvanceButtonMotion(state, ui::ClientAreaAnimationsEnabled());
}

[[nodiscard]] bool HasActiveButtonMotion(const ButtonMotionState& state) noexcept {
    return state.hover.IsActive() || state.press.IsActive() ||
        state.focus.IsActive();
}

void UpdateButtonMotionTimer(const HWND control, ButtonMotionState& state) noexcept {
    if (control == nullptr) {
        return;
    }
    if (HasActiveButtonMotion(state)) {
        if (!state.timerArmed &&
            ::SetTimer(control, kButtonMotionTimerId, kMotionFrameMilliseconds, nullptr) == 0) {
            state.hover.JumpTo(state.hover.Target());
            state.press.JumpTo(state.press.Target());
            state.focus.JumpTo(state.focus.Target());
            state.timerArmed = false;
            RequestButtonRedraw(control);
        } else {
            state.timerArmed = true;
        }
    } else if (state.timerArmed) {
        ::KillTimer(control, kButtonMotionTimerId);
        state.timerArmed = false;
    }
}

int ScaleForWindow(const HWND window, const int value) noexcept {
    const UINT dpi = window != nullptr ? ::GetDpiForWindow(window) : USER_DEFAULT_SCREEN_DPI;
    return ::MulDiv(value, static_cast<int>(dpi), USER_DEFAULT_SCREEN_DPI);
}

float ScaleForWindow(const HWND window, const float value) noexcept {
    const UINT dpi = window != nullptr ? ::GetDpiForWindow(window) : USER_DEFAULT_SCREEN_DPI;
    return value * static_cast<float>(dpi) / static_cast<float>(USER_DEFAULT_SCREEN_DPI);
}

void FillSolidRectangle(const HDC dc, const RECT& rectangle, const COLORREF color) {
    const HBRUSH brush = ::CreateSolidBrush(color);
    ::FillRect(dc, &rectangle, brush);
    ::DeleteObject(brush);
}

void DrawRoundedRectangle(
    const HDC dc,
    const RECT& rectangle,
    const int radius,
    const COLORREF fill,
    const COLORREF border,
    const int borderWidth = 1) {
    ui::Canvas canvas(dc);
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

HFONT CreateUiFont(
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

void SetRect(RECT& rectangle, const int x, const int y, const int width, const int height) noexcept {
    rectangle = RECT{x, y, x + std::max(1, width), y + std::max(1, height)};
}

enum class SegmentSide : unsigned char {
    Left,
    Right,
};

void AddSegmentPath(
    Gdiplus::GraphicsPath& path,
    const Gdiplus::RectF& rectangle,
    const float requestedRadius,
    const SegmentSide side) {
    const float radius = std::clamp(
        requestedRadius,
        0.0F,
        std::min(rectangle.Width, rectangle.Height) * 0.5F);
    const float diameter = radius * 2.0F;
    const float right = rectangle.GetRight();
    const float bottom = rectangle.GetBottom();

    path.StartFigure();
    if (radius <= 0.0F) {
        path.AddRectangle(rectangle);
    } else if (side == SegmentSide::Left) {
        path.AddArc(
            rectangle.X,
            rectangle.Y,
            diameter,
            diameter,
            180.0F,
            90.0F);
        path.AddLine(rectangle.X + radius, rectangle.Y, right, rectangle.Y);
        path.AddLine(right, rectangle.Y, right, bottom);
        path.AddLine(right, bottom, rectangle.X + radius, bottom);
        path.AddArc(
            rectangle.X,
            bottom - diameter,
            diameter,
            diameter,
            90.0F,
            90.0F);
    } else {
        path.AddLine(rectangle.X, rectangle.Y, right - radius, rectangle.Y);
        path.AddArc(
            right - diameter,
            rectangle.Y,
            diameter,
            diameter,
            270.0F,
            90.0F);
        path.AddLine(right, rectangle.Y + radius, right, bottom - radius);
        path.AddArc(
            right - diameter,
            bottom - diameter,
            diameter,
            diameter,
            0.0F,
            90.0F);
        path.AddLine(right - radius, bottom, rectangle.X, bottom);
    }
    path.CloseFigure();
}

void DrawSegmentedRectangle(
    const HDC dc,
    const RECT& bounds,
    const int radius,
    const COLORREF fill,
    const COLORREF border,
    const COLORREF divider,
    const int borderWidth,
    const SegmentSide side) {
    if (!ui::SharedGdiPlusRuntime().Ready()) {
        DrawRoundedRectangle(dc, bounds, radius, fill, border, borderWidth);
        return;
    }

    Gdiplus::Graphics graphics(dc);
    if (graphics.GetLastStatus() != Gdiplus::Ok) {
        DrawRoundedRectangle(dc, bounds, radius, fill, border, borderWidth);
        return;
    }
    graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    graphics.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);
    graphics.SetCompositingQuality(Gdiplus::CompositingQualityHighQuality);

    const float width = static_cast<float>(std::max<LONG>(1, bounds.right - bounds.left));
    const float height = static_cast<float>(std::max<LONG>(1, bounds.bottom - bounds.top));
    const Gdiplus::RectF fillRectangle{
        static_cast<float>(bounds.left),
        static_cast<float>(bounds.top),
        width,
        height};
    Gdiplus::GraphicsPath fillPath;
    AddSegmentPath(fillPath, fillRectangle, static_cast<float>(radius), side);
    Gdiplus::SolidBrush fillBrush(ui::ToGdiPlusColor(fill));
    graphics.FillPath(&fillBrush, &fillPath);

    const float strokeWidth = static_cast<float>(std::max(1, borderWidth));
    const float inset = strokeWidth * 0.5F;
    const Gdiplus::RectF outlineRectangle{
        static_cast<float>(bounds.left) + inset,
        static_cast<float>(bounds.top) + inset,
        std::max(1.0F, width - strokeWidth),
        std::max(1.0F, height - strokeWidth)};
    const float outlineRadius = std::max(0.0F, static_cast<float>(radius) - inset);
    const float diameter = outlineRadius * 2.0F;
    const float right = outlineRectangle.GetRight();
    const float bottom = outlineRectangle.GetBottom();
    Gdiplus::GraphicsPath outline;
    if (side == SegmentSide::Left) {
        outline.StartFigure();
        outline.AddLine(
            static_cast<float>(bounds.right),
            outlineRectangle.Y,
            outlineRectangle.X + outlineRadius,
            outlineRectangle.Y);
        if (outlineRadius > 0.0F) {
            outline.AddArc(
                outlineRectangle.X,
                outlineRectangle.Y,
                diameter,
                diameter,
                270.0F,
                -90.0F);
            outline.AddLine(
                outlineRectangle.X,
                outlineRectangle.Y + outlineRadius,
                outlineRectangle.X,
                bottom - outlineRadius);
            outline.AddArc(
                outlineRectangle.X,
                bottom - diameter,
                diameter,
                diameter,
                180.0F,
                -90.0F);
        } else {
            outline.AddLine(
                outlineRectangle.X,
                outlineRectangle.Y,
                outlineRectangle.X,
                bottom);
        }
        outline.AddLine(
            outlineRectangle.X + outlineRadius,
            bottom,
            static_cast<float>(bounds.right),
            bottom);
    } else {
        outline.StartFigure();
        outline.AddLine(
            static_cast<float>(bounds.left),
            outlineRectangle.Y,
            right - outlineRadius,
            outlineRectangle.Y);
        if (outlineRadius > 0.0F) {
            outline.AddArc(
                right - diameter,
                outlineRectangle.Y,
                diameter,
                diameter,
                270.0F,
                90.0F);
            outline.AddLine(
                right,
                outlineRectangle.Y + outlineRadius,
                right,
                bottom - outlineRadius);
            outline.AddArc(
                right - diameter,
                bottom - diameter,
                diameter,
                diameter,
                0.0F,
                90.0F);
        } else {
            outline.AddLine(right, outlineRectangle.Y, right, bottom);
        }
        outline.AddLine(
            right - outlineRadius,
            bottom,
            static_cast<float>(bounds.left),
            bottom);
    }

    Gdiplus::Pen borderPen(ui::ToGdiPlusColor(border), strokeWidth);
    borderPen.SetLineJoin(Gdiplus::LineJoinRound);
    graphics.DrawPath(&borderPen, &outline);
    if (side == SegmentSide::Right) {
        Gdiplus::Pen dividerPen(ui::ToGdiPlusColor(divider), strokeWidth);
        graphics.DrawLine(
            &dividerPen,
            static_cast<float>(bounds.left) + inset,
            outlineRectangle.Y,
            static_cast<float>(bounds.left) + inset,
            bottom);
    }
}

void DrawSegmentFocusRing(
    const HDC dc,
    const RECT& bounds,
    const int radius,
    const COLORREF color,
    const int width,
    const SegmentSide side) {
    if (!ui::SharedGdiPlusRuntime().Ready()) {
        const HPEN pen = ::CreatePen(PS_SOLID, std::max(1, width), color);
        const HGDIOBJ previousPen = ::SelectObject(dc, pen);
        const HGDIOBJ previousBrush = ::SelectObject(dc, ::GetStockObject(HOLLOW_BRUSH));
        ::Rectangle(dc, bounds.left, bounds.top, bounds.right, bounds.bottom);
        ::SelectObject(dc, previousBrush);
        ::SelectObject(dc, previousPen);
        ::DeleteObject(pen);
        return;
    }
    Gdiplus::Graphics graphics(dc);
    graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    graphics.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);
    const float strokeWidth = static_cast<float>(std::max(1, width));
    const float inset = strokeWidth * 0.5F;
    const Gdiplus::RectF rectangle{
        static_cast<float>(bounds.left) + inset,
        static_cast<float>(bounds.top) + inset,
        std::max(1.0F, static_cast<float>(bounds.right - bounds.left) - strokeWidth),
        std::max(1.0F, static_cast<float>(bounds.bottom - bounds.top) - strokeWidth)};
    Gdiplus::GraphicsPath path;
    AddSegmentPath(
        path,
        rectangle,
        std::max(0.0F, static_cast<float>(radius) - inset),
        side);
    Gdiplus::Pen pen(ui::ToGdiPlusColor(color), strokeWidth);
    pen.SetLineJoin(Gdiplus::LineJoinRound);
    graphics.DrawPath(&pen, &path);
}

}  // namespace

EditorChrome::~EditorChrome() {
    Destroy();
}

int EditorChrome::Scale(const HWND window, const int value) noexcept {
    return ScaleForWindow(window, value);
}

bool EditorChrome::Initialize(const HWND window) {
    Destroy();
    workspaceBrush_ = ::CreateSolidBrush(editor_theme::Canvas);
    panelBrush_ = ::CreateSolidBrush(editor_theme::Panel);
    videoBrush_ = ::CreateSolidBrush(editor_theme::VideoStage);
    RecreateFonts(window);
    if (workspaceBrush_ == nullptr || panelBrush_ == nullptr || videoBrush_ == nullptr ||
        regularFont_ == nullptr || strongFont_ == nullptr || titleFont_ == nullptr ||
        captionFont_ == nullptr || timeFont_ == nullptr) {
        Destroy();
        return false;
    }
    return true;
}

void EditorChrome::RecreateFonts(const HWND window) {
    DeleteFonts();
    regularFont_ = CreateUiFont(window, 14, FW_NORMAL, theme::FontFamily);
    strongFont_ = CreateUiFont(window, 14, FW_SEMIBOLD, theme::FontFamily);
    titleFont_ = CreateUiFont(window, 16, FW_SEMIBOLD, theme::TitleFontFamily);
    captionFont_ = CreateUiFont(window, 13, FW_NORMAL, theme::FontFamily);
    timeFont_ = CreateUiFont(window, 14, FW_NORMAL, theme::LatinFontFamily);
}

void EditorChrome::DeleteFonts() noexcept {
    const std::array fonts{regularFont_, strongFont_, titleFont_, captionFont_, timeFont_};
    for (const HFONT font : fonts) {
        if (font != nullptr) {
            ::DeleteObject(font);
        }
    }
    regularFont_ = nullptr;
    strongFont_ = nullptr;
    titleFont_ = nullptr;
    captionFont_ = nullptr;
    timeFont_ = nullptr;
}

bool EditorChrome::EnsureBackBuffer(
    const HDC target,
    const int width,
    const int height) noexcept {
    if (target == nullptr || width <= 0 || height <= 0) {
        return false;
    }
    if (backBufferDc_ != nullptr && backBufferBitmap_ != nullptr &&
        backBufferWidth_ == width && backBufferHeight_ == height) {
        return true;
    }

    const HDC replacementDc = ::CreateCompatibleDC(target);
    if (replacementDc == nullptr) {
        return false;
    }
    const HBITMAP replacementBitmap = ::CreateCompatibleBitmap(target, width, height);
    if (replacementBitmap == nullptr) {
        ::DeleteDC(replacementDc);
        return false;
    }
    const HGDIOBJ replacementPreviousBitmap = ::SelectObject(
        replacementDc,
        replacementBitmap);
    if (replacementPreviousBitmap == nullptr ||
        replacementPreviousBitmap == HGDI_ERROR) {
        ::DeleteObject(replacementBitmap);
        ::DeleteDC(replacementDc);
        return false;
    }

    DeleteBackBuffer();
    backBufferDc_ = replacementDc;
    backBufferBitmap_ = replacementBitmap;
    backBufferPreviousBitmap_ = replacementPreviousBitmap;
    backBufferWidth_ = width;
    backBufferHeight_ = height;
    return true;
}

void EditorChrome::DeleteBackBuffer() noexcept {
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

void EditorChrome::Destroy() noexcept {
    DeleteBackBuffer();
    DeleteFonts();
    if (workspaceBrush_ != nullptr) {
        ::DeleteObject(workspaceBrush_);
        workspaceBrush_ = nullptr;
    }
    if (panelBrush_ != nullptr) {
        ::DeleteObject(panelBrush_);
        panelBrush_ = nullptr;
    }
    if (videoBrush_ != nullptr) {
        ::DeleteObject(videoBrush_);
        videoBrush_ = nullptr;
    }
}

void EditorChrome::AttachButton(const HWND button) {
    if (button == nullptr) {
        return;
    }
    DWORD_PTR existingData{};
    if (::GetWindowSubclass(
            button,
            &EditorChrome::ButtonSubclassProc,
            kButtonSubclassId,
            &existingData) != FALSE) {
        return;
    }
    auto* state = new (std::nothrow) ButtonMotionState{};
    if (state == nullptr) {
        return;
    }
    if (::SetWindowSubclass(
            button,
            &EditorChrome::ButtonSubclassProc,
            kButtonSubclassId,
            reinterpret_cast<DWORD_PTR>(state)) == FALSE) {
        delete state;
    }
}

LRESULT CALLBACK EditorChrome::ButtonSubclassProc(
    const HWND control,
    const UINT message,
    const WPARAM wParam,
    const LPARAM lParam,
    const UINT_PTR subclassId,
    const DWORD_PTR referenceData) {
    auto* state = reinterpret_cast<ButtonMotionState*>(referenceData);
    if (state != nullptr) {
        switch (message) {
        case WM_MOUSEMOVE: {
            if (!state->hovered) {
                state->hovered = true;
                SetMotionTarget(
                    state->hover,
                    true,
                    kHoverEnterDuration,
                    kHoverExitDuration);
                UpdateButtonMotionTimer(control, *state);
                ::InvalidateRect(control, nullptr, FALSE);
            }
            if (!state->trackingMouse) {
                TRACKMOUSEEVENT tracking{};
                tracking.cbSize = sizeof(tracking);
                tracking.dwFlags = TME_LEAVE;
                tracking.hwndTrack = control;
                state->trackingMouse = ::TrackMouseEvent(&tracking) != FALSE;
                if (!state->trackingMouse && state->hovered) {
                    state->hovered = false;
                    SetMotionTarget(
                        state->hover,
                        false,
                        kHoverEnterDuration,
                        kHoverExitDuration);
                    UpdateButtonMotionTimer(control, *state);
                    ::InvalidateRect(control, nullptr, FALSE);
                }
            }
            break;
        }
        case WM_MOUSELEAVE:
            state->trackingMouse = false;
            if (state->hovered) {
                state->hovered = false;
                SetMotionTarget(
                    state->hover,
                    false,
                    kHoverEnterDuration,
                    kHoverExitDuration);
                UpdateButtonMotionTimer(control, *state);
                ::InvalidateRect(control, nullptr, FALSE);
            }
            break;
        case WM_LBUTTONDOWN:
            // Press state is committed to the next coalesced paint frame; input
            // remains non-blocking even if DWM/GDI is temporarily saturated.
            state->press.JumpTo(1.0F);
            UpdateButtonMotionTimer(control, *state);
            RequestButtonRedraw(control);
            break;
        case WM_LBUTTONUP:
        case WM_CAPTURECHANGED:
        case WM_CANCELMODE:
            state->press.JumpTo(0.0F);
            UpdateButtonMotionTimer(control, *state);
            RequestButtonRedraw(control);
            break;
        case WM_KEYDOWN:
            if (wParam == VK_SPACE &&
                (static_cast<LPARAM>(lParam) & (1LL << 30)) == 0) {
                state->press.JumpTo(1.0F);
                UpdateButtonMotionTimer(control, *state);
                RequestButtonRedraw(control);
            }
            break;
        case WM_KEYUP:
            if (wParam == VK_SPACE) {
                state->press.JumpTo(0.0F);
                UpdateButtonMotionTimer(control, *state);
                RequestButtonRedraw(control);
            }
            break;
        case BM_SETSTATE:
            state->press.JumpTo(wParam != FALSE ? 1.0F : 0.0F);
            UpdateButtonMotionTimer(control, *state);
            RequestButtonRedraw(control);
            break;
        case WM_SETFOCUS:
            SetMotionTarget(
                state->focus,
                true,
                kFocusEnterDuration,
                kFocusExitDuration,
                ui::MotionEasing::EaseOutQuint);
            UpdateButtonMotionTimer(control, *state);
            ::InvalidateRect(control, nullptr, FALSE);
            break;
        case WM_KILLFOCUS:
            SetMotionTarget(
                state->focus,
                false,
                kFocusEnterDuration,
                kFocusExitDuration);
            UpdateButtonMotionTimer(control, *state);
            ::InvalidateRect(control, nullptr, FALSE);
            break;
        case WM_ENABLE:
            if (wParam == FALSE) {
                state->hovered = false;
                state->trackingMouse = false;
                SetMotionTarget(
                    state->hover,
                    false,
                    kHoverEnterDuration,
                    kHoverExitDuration);
                SetMotionTarget(
                    state->press,
                    false,
                    kPressEnterDuration,
                    kPressExitDuration);
            }
            UpdateButtonMotionTimer(control, *state);
            ::InvalidateRect(control, nullptr, FALSE);
            break;
        case WM_TIMER:
            if (wParam == kButtonMotionTimerId) {
                const bool active = AdvanceButtonMotion(*state);
                RequestButtonRedraw(control);
                if (!active) {
                    ::KillTimer(control, kButtonMotionTimerId);
                    state->timerArmed = false;
                }
                return 0;
            }
            break;
        case WM_SETTEXT:
        case WM_SETFONT:
        case WM_DPICHANGED_AFTERPARENT:
            state->labelMetricsValid = false;
            break;
        case WM_NCDESTROY:
            ::KillTimer(control, kButtonMotionTimerId);
            state->timerArmed = false;
            ::RemoveWindowSubclass(control, &EditorChrome::ButtonSubclassProc, subclassId);
            delete state;
            break;
        default:
            break;
        }
    }
    return ::DefSubclassProc(control, message, wParam, lParam);
}

EditorChromeLayout EditorChrome::CalculateLayout(
    const HWND window,
    const int width,
    const int height,
    const RecordingResult& recording) const {
    EditorChromeLayout layout{};
    const bool compact = width < Scale(window, 970);
    const int margin = Scale(window, 28);
    const int headerHeight = Scale(window, kHeaderHeight);
    const int panelHeight = Scale(window, compact ? 278 : 224);
    const int previewGap = Scale(window, 16);
    const int previewTop = headerHeight + Scale(window, 16);
    layout.editorPanelTop = std::max(
        previewTop + Scale(window, 190),
        height - panelHeight);

    SetRect(
        layout.headerTitle,
        margin,
        Scale(window, 10),
        width - margin * 2,
        Scale(window, 25));
    SetRect(
        layout.headerSubtitle,
        margin,
        Scale(window, 36),
        width - margin * 2,
        Scale(window, 20));

    const int previewAreaWidth = std::max(1, width - margin * 2);
    const int previewAreaHeight = std::max(
        Scale(window, 150),
        layout.editorPanelTop - previewTop - previewGap);
    SetRect(
        layout.previewStage,
        margin,
        previewTop,
        previewAreaWidth,
        previewAreaHeight);
    int previewWidth = previewAreaWidth;
    int previewHeight = previewAreaHeight;
    const double videoAspect = recording.height > 0
        ? static_cast<double>(recording.width) / static_cast<double>(recording.height)
        : 16.0 / 9.0;
    if (static_cast<double>(previewWidth) / static_cast<double>(previewHeight) > videoAspect) {
        previewWidth = std::max(1, static_cast<int>(std::lround(
            static_cast<double>(previewHeight) * videoAspect)));
    } else {
        previewHeight = std::max(1, static_cast<int>(std::lround(
            static_cast<double>(previewWidth) / videoAspect)));
    }
    SetRect(
        layout.preview,
        (width - previewWidth) / 2,
        previewTop + (previewAreaHeight - previewHeight) / 2,
        previewWidth,
        previewHeight);
    layout.previewRadius = Scale(window, 4);

    const int contentWidth = std::max(1, width - margin * 2);
    const int rangeY = layout.editorPanelTop + Scale(window, 16);
    const int compactButtonWidth = Scale(window, 32);
    const int compactButtonGap = Scale(window, 6);
    const int speedGap = Scale(window, 10);
    const int speedWidth = Scale(window, EditorSpeedControl::PreferredWidthDip);
    int rangeToolbarRight = width - margin;
    rangeToolbarRight -= compactButtonWidth;
    SetRect(
        layout.trimEndButton,
        rangeToolbarRight,
        rangeY,
        compactButtonWidth,
        Scale(window, EditorSpeedControl::HeightDip));
    rangeToolbarRight -= compactButtonGap + compactButtonWidth;
    SetRect(
        layout.trimStartButton,
        rangeToolbarRight,
        rangeY,
        compactButtonWidth,
        Scale(window, EditorSpeedControl::HeightDip));
    rangeToolbarRight -= speedGap + speedWidth;
    SetRect(
        layout.speedControl,
        rangeToolbarRight,
        rangeY,
        speedWidth,
        Scale(window, EditorSpeedControl::HeightDip));
    SetRect(
        layout.rangeLabel,
        margin,
        rangeY,
        std::max(
            1,
            static_cast<int>(layout.speedControl.left) - margin - Scale(window, 12)),
        Scale(window, 30));
    const int timelineY = rangeY + Scale(window, 30);
    SetRect(layout.timeline, margin, timelineY, contentWidth, Scale(window, 78));

    const int buttonHeight = Scale(window, theme::ControlHeight);
    const int gap = Scale(window, 8);
    const int playWidth = Scale(window, 90);
    const int timeWidth = Scale(window, 188);
    const int labelWidth = Scale(window, 68);
    const int segmentWidth = Scale(window, 60);
    const int saveWidth = Scale(window, 144);
    const int copyWidth = Scale(window, 150);
    const int firstRowY = timelineY + Scale(window, 90);
    SetRect(layout.playButton, margin, firstRowY, playWidth, buttonHeight);
    SetRect(
        layout.timeLabel,
        margin + playWidth + gap,
        firstRowY,
        timeWidth,
        buttonHeight);

    const int actionY = compact ? firstRowY + Scale(window, 54) : firstRowY;
    int right = width - margin;
    right -= saveWidth;
    SetRect(layout.saveButton, right, actionY, saveWidth, buttonHeight);
    right -= gap + copyWidth;
    SetRect(layout.copyButton, right, actionY, copyWidth, buttonHeight);
    right -= Scale(window, 14) + segmentWidth;
    SetRect(
        layout.gifButton,
        right,
        actionY + Scale(window, 1),
        segmentWidth,
        Scale(window, 38));
    right -= segmentWidth;
    SetRect(
        layout.mp4Button,
        right,
        actionY + Scale(window, 1),
        segmentWidth,
        Scale(window, 38));
    right -= Scale(window, 8) + labelWidth;
    SetRect(layout.formatLabel, right, actionY, labelWidth, buttonHeight);

    const int audioWidth = Scale(window, EditorAudioToggle::PreferredWidthDip);
    const int minimumAudioLeft = compact
        ? margin
        : layout.timeLabel.right + Scale(window, 12);
    const int maximumAudioRight = compact
        ? minimumAudioLeft + audioWidth
        : layout.formatLabel.left - Scale(window, 8);
    const int resolvedAudioWidth = std::clamp(
        maximumAudioRight - minimumAudioLeft,
        Scale(window, EditorAudioToggle::MinimumWidthDip),
        audioWidth);
    const int audioLeft = compact
        ? minimumAudioLeft
        : maximumAudioRight - resolvedAudioWidth;
    SetRect(
        layout.audioToggle,
        audioLeft,
        actionY,
        resolvedAudioWidth,
        Scale(window, EditorAudioToggle::HeightDip));

    const int statusY = actionY + buttonHeight + Scale(window, 9);
    layout.statusDotCenter.x = margin + Scale(window, 5);
    layout.statusDotCenter.y = statusY + Scale(window, 9);
    SetRect(
        layout.statusLabel,
        margin + Scale(window, 16),
        statusY,
        contentWidth - Scale(window, 16),
        Scale(window, 20));
    return layout;
}

void EditorChrome::ApplyPreviewRegion(
    const HWND preview,
    const EditorChromeLayout& layout) const {
    if (preview == nullptr) {
        return;
    }
    const int width = layout.preview.right - layout.preview.left;
    const int height = layout.preview.bottom - layout.preview.top;
    const HRGN region = ::CreateRoundRectRgn(
        0,
        0,
        width + 1,
        height + 1,
        layout.previewRadius * 2,
        layout.previewRadius * 2);
    if (region != nullptr && ::SetWindowRgn(preview, region, FALSE) == 0) {
        ::DeleteObject(region);
    }
}

COLORREF EditorChrome::StatusColor(const EditorStatusTone tone) const noexcept {
    switch (tone) {
    case EditorStatusTone::Progress:
        return editor_theme::Progress;
    case EditorStatusTone::Success:
        return editor_theme::Success;
    case EditorStatusTone::Error:
        return editor_theme::DangerText;
    case EditorStatusTone::Neutral:
    default:
        return editor_theme::TextSecondary;
    }
}

void EditorChrome::PaintWindow(
    const HWND window,
    const int editorPanelTop,
    const RECT& previewStage,
    const POINT statusDotCenter,
    const EditorStatusTone statusTone) {
    PAINTSTRUCT paint{};
    const HDC target = ::BeginPaint(window, &paint);
    RECT client{};
    ::GetClientRect(window, &client);
    const int width = std::max(1, static_cast<int>(client.right));
    const int height = std::max(1, static_cast<int>(client.bottom));
    if (!EnsureBackBuffer(target, width, height)) {
        ::FillRect(target, &client, workspaceBrush_);
        ::EndPaint(window, &paint);
        return;
    }
    const HDC buffer = backBufferDc_;
    const int savedBufferState = ::SaveDC(buffer);
    if (savedBufferState != 0) {
        static_cast<void>(::IntersectClipRect(
            buffer,
            paint.rcPaint.left,
            paint.rcPaint.top,
            paint.rcPaint.right,
            paint.rcPaint.bottom));
    }
    ::FillRect(buffer, &client, workspaceBrush_);
    RECT header{0, 0, width, Scale(window, kHeaderHeight)};
    ::FillRect(buffer, &header, panelBrush_);
    RECT headerLine{0, header.bottom - 1, width, header.bottom};
    FillSolidRectangle(buffer, headerLine, editor_theme::BorderSubtle);
    if (previewStage.right > previewStage.left && previewStage.bottom > previewStage.top) {
        DrawRoundedRectangle(
            buffer,
            previewStage,
            Scale(window, theme::CornerLarge),
            editor_theme::VideoStage,
            editor_theme::VideoStageBorder);
    }
    if (editorPanelTop > 0) {
        RECT editorPanel{0, editorPanelTop, width, height};
        ::FillRect(buffer, &editorPanel, panelBrush_);
        RECT panelLine{0, editorPanelTop, width, editorPanelTop + 1};
        FillSolidRectangle(buffer, panelLine, editor_theme::BorderSubtle);
    }

    const int dotRadius = Scale(window, 3);
    if (statusDotCenter.y > 0) {
        const COLORREF dotColor = StatusColor(statusTone);
        const HBRUSH dotBrush = ::CreateSolidBrush(dotColor);
        const HPEN dotPen = ::CreatePen(PS_SOLID, 1, dotColor);
        const HGDIOBJ oldBrush = ::SelectObject(buffer, dotBrush);
        const HGDIOBJ oldPen = ::SelectObject(buffer, dotPen);
        ::Ellipse(
            buffer,
            statusDotCenter.x - dotRadius,
            statusDotCenter.y - dotRadius,
            statusDotCenter.x + dotRadius,
            statusDotCenter.y + dotRadius);
        ::SelectObject(buffer, oldPen);
        ::SelectObject(buffer, oldBrush);
        ::DeleteObject(dotPen);
        ::DeleteObject(dotBrush);
    }

    if (savedBufferState != 0) {
        static_cast<void>(::RestoreDC(buffer, savedBufferState));
    }
    const int paintWidth = std::max(
        0,
        static_cast<int>(paint.rcPaint.right - paint.rcPaint.left));
    const int paintHeight = std::max(
        0,
        static_cast<int>(paint.rcPaint.bottom - paint.rcPaint.top));
    if (paintWidth > 0 && paintHeight > 0) {
        static_cast<void>(::BitBlt(
            target,
            paint.rcPaint.left,
            paint.rcPaint.top,
            paintWidth,
            paintHeight,
            buffer,
            paint.rcPaint.left,
            paint.rcPaint.top,
            SRCCOPY));
    }
    ::EndPaint(window, &paint);
}

bool EditorChrome::DrawButton(
    const HWND owner,
    const DRAWITEMSTRUCT* item,
    const EditorButtonPaintState& state) const {
    if (item == nullptr || item->CtlType != ODT_BUTTON || item->hwndItem == nullptr) {
        return false;
    }
    const UINT dpi = owner != nullptr
        ? ::GetDpiForWindow(owner)
        : USER_DEFAULT_SCREEN_DPI;
    const auto scale = [dpi](const int value) noexcept {
        return ::MulDiv(value, static_cast<int>(dpi), USER_DEFAULT_SCREEN_DPI);
    };
    const auto scaleFloat = [dpi](const float value) noexcept {
        return value * static_cast<float>(dpi) /
            static_cast<float>(USER_DEFAULT_SCREEN_DPI);
    };
    const bool isSegment =
        state.role == EditorButtonRole::SegmentLeft ||
        state.role == EditorButtonRole::SegmentRight;
    const bool isTrimBoundary =
        state.role == EditorButtonRole::TrimStart ||
        state.role == EditorButtonRole::TrimEnd;
    const bool isPrimary = state.role == EditorButtonRole::Primary;
    const bool isPlay = state.role == EditorButtonRole::Play;
    const bool isCopy = state.role == EditorButtonRole::Secondary;
    const bool isMp4Segment = state.role == EditorButtonRole::SegmentLeft;
    const COLORREF segmentSelectedFill = isMp4Segment
        ? editor_theme::Mp4Selected
        : editor_theme::Selection;
    const COLORREF segmentSelectedHover = isMp4Segment
        ? editor_theme::Mp4SelectedHover
        : editor_theme::SelectionHover;
    const COLORREF segmentSelectedPressed = isMp4Segment
        ? editor_theme::Mp4SelectedPressed
        : editor_theme::SelectionPressed;
    const COLORREF segmentSelectedBorder = isMp4Segment
        ? editor_theme::Mp4SelectedBorder
        : editor_theme::SelectionBorder;
    const COLORREF segmentSelectedText = isMp4Segment
        ? editor_theme::Mp4SelectedText
        : editor_theme::SelectionText;
    const bool enabled = (item->itemState & ODS_DISABLED) == 0;
    const bool pressed = (item->itemState & ODS_SELECTED) != 0;
    const bool focused = (item->itemState & ODS_FOCUS) != 0;

    DWORD_PTR motionData{};
    ButtonMotionState* motion = nullptr;
    if (::GetWindowSubclass(
            item->hwndItem,
            &EditorChrome::ButtonSubclassProc,
            kButtonSubclassId,
            &motionData) != FALSE) {
        motion = reinterpret_cast<ButtonMotionState*>(motionData);
    }
    const float hoverAmount = motion != nullptr
        ? motion->hover.Value()
        : 0.0F;
    const float pressAmount = motion != nullptr
        ? motion->press.Value()
        : (pressed ? 1.0F : 0.0F);
    const float focusAmount = motion != nullptr
        ? motion->focus.Value()
        : (focused ? 1.0F : 0.0F);
    const float selectionAmount = state.selected ? 1.0F : 0.0F;

    const HDC dc = item->hDC;
    RECT bounds = item->rcItem;
    FillSolidRectangle(dc, bounds, editor_theme::Panel);
    RECT shape = bounds;
    ::InflateRect(&shape, -scale(1), -scale(1));
    const int radius = scale(theme::CornerMedium);
    COLORREF fill = editor_theme::Control;
    COLORREF border = editor_theme::Border;
    COLORREF textColor = editor_theme::TextPrimary;
    if (state.busy) {
        fill = isPrimary
            ? editor_theme::SavePressed
            : editor_theme::SelectionPressed;
        border = isPrimary
            ? editor_theme::SavePressed
            : editor_theme::SelectionBorder;
        textColor = isPrimary
            ? editor_theme::White
            : editor_theme::SelectionText;
    } else if (!enabled) {
        fill = editor_theme::ControlDisabled;
        border = editor_theme::BorderDisabled;
        textColor = editor_theme::TextDisabled;
    } else if (isPrimary) {
        fill = ui::InterpolateColor(
            editor_theme::Save,
            editor_theme::SaveHover,
            hoverAmount);
        fill = ui::InterpolateColor(fill, editor_theme::SavePressed, pressAmount);
        border = fill;
        textColor = editor_theme::White;
    } else {
        const COLORREF baseFill = isSegment
            ? ui::InterpolateColor(
                editor_theme::Control,
                segmentSelectedFill,
                selectionAmount)
            : editor_theme::Control;
        const COLORREF hoverFill = isSegment
            ? ui::InterpolateColor(
                editor_theme::ControlHover,
                segmentSelectedHover,
                selectionAmount)
            : editor_theme::ControlHover;
        const COLORREF pressedFill = isSegment
            ? ui::InterpolateColor(
                editor_theme::ControlPressed,
                segmentSelectedPressed,
                selectionAmount)
            : editor_theme::ControlPressed;
        fill = ui::InterpolateColor(baseFill, hoverFill, hoverAmount);
        fill = ui::InterpolateColor(fill, pressedFill, pressAmount);
        border = ui::InterpolateColor(
            editor_theme::Border,
            editor_theme::BorderHover,
            hoverAmount);
        if (isSegment) {
            border = ui::InterpolateColor(
                border,
                segmentSelectedBorder,
                selectionAmount);
            textColor = ui::InterpolateColor(
                editor_theme::TextPrimary,
                segmentSelectedText,
                selectionAmount);
        }
    }
    if (isSegment) {
        RECT segmentShape = bounds;
        const int inset = scale(1);
        segmentShape.top += inset;
        segmentShape.bottom -= inset;
        const SegmentSide side = state.role == EditorButtonRole::SegmentLeft
            ? SegmentSide::Left
            : SegmentSide::Right;
        if (side == SegmentSide::Left) {
            segmentShape.left += inset;
        } else {
            segmentShape.right -= inset;
        }
        DrawSegmentedRectangle(
            dc,
            segmentShape,
            radius,
            fill,
            border,
            enabled
                ? ui::InterpolateColor(
                    editor_theme::BorderHover,
                    segmentSelectedBorder,
                    selectionAmount)
                : editor_theme::BorderDisabled,
            std::max(1, scale(1)),
            side);
    } else {
        DrawRoundedRectangle(dc, shape, radius, fill, border);
    }

    const HFONT buttonFont = isPrimary ? strongFont_ : regularFont_;
    const HGDIOBJ previousFont = ::SelectObject(dc, buttonFont);
    ::SetBkMode(dc, TRANSPARENT);
    ::SetTextColor(dc, textColor);
    std::wstring fallbackLabel;
    SIZE labelSize{};
    const wchar_t* labelText = L"";
    int labelLength = 0;
    if (motion != nullptr) {
        if (!motion->labelMetricsValid || motion->cachedLabelFont != buttonFont ||
            motion->cachedLabelDpi != dpi) {
            const int requestedLength = std::max(
                0,
                ::GetWindowTextLengthW(item->hwndItem));
            std::wstring refreshedLabel(
                static_cast<std::size_t>(requestedLength) + 1,
                L'\0');
            const int copiedLength = ::GetWindowTextW(
                item->hwndItem,
                refreshedLabel.data(),
                requestedLength + 1);
            refreshedLabel.resize(static_cast<std::size_t>(std::max(0, copiedLength)));
            motion->cachedLabel = std::move(refreshedLabel);
            motion->cachedLabelSize = {};
            static_cast<void>(::GetTextExtentPoint32W(
                dc,
                motion->cachedLabel.data(),
                static_cast<int>(motion->cachedLabel.size()),
                &motion->cachedLabelSize));
            motion->cachedLabelFont = buttonFont;
            motion->cachedLabelDpi = dpi;
            motion->labelMetricsValid = true;
        }
        labelText = motion->cachedLabel.c_str();
        labelLength = static_cast<int>(motion->cachedLabel.size());
        labelSize = motion->cachedLabelSize;
    } else {
        const int requestedLength = std::max(
            0,
            ::GetWindowTextLengthW(item->hwndItem));
        fallbackLabel.resize(static_cast<std::size_t>(requestedLength) + 1, L'\0');
        const int copiedLength = ::GetWindowTextW(
            item->hwndItem,
            fallbackLabel.data(),
            requestedLength + 1);
        fallbackLabel.resize(static_cast<std::size_t>(std::max(0, copiedLength)));
        labelText = fallbackLabel.c_str();
        labelLength = static_cast<int>(fallbackLabel.size());
        static_cast<void>(::GetTextExtentPoint32W(
            dc,
            labelText,
            labelLength,
            &labelSize));
    }

    // Keep the semantic button names ("设为起点/终点") available to
    // accessibility APIs while rendering only the compact bracket glyphs.
    if (isTrimBoundary) {
        labelText = state.role == EditorButtonRole::TrimStart ? L"【" : L"】";
        labelLength = 1;
        labelSize = {};
        static_cast<void>(::GetTextExtentPoint32W(
            dc,
            labelText,
            labelLength,
            &labelSize));
    }

    const bool hasIcon = isPlay || isCopy || isPrimary;
    const int iconWidth = hasIcon ? scale(16) : 0;
    const int iconGap = hasIcon ? scale(8) : 0;
    const int contentWidth = iconWidth + iconGap + labelSize.cx;
    const int contentLeft = bounds.left +
        (static_cast<int>(bounds.right - bounds.left) - contentWidth) / 2;
    const int centerY = (bounds.top + bounds.bottom) / 2;

    if (hasIcon) {
        ui::Canvas iconCanvas(dc);
        const int iconLeft = contentLeft;
        if (isPlay) {
            if (state.playing) {
                RECT leftBar{
                    iconLeft + scale(2), centerY - scale(7),
                    iconLeft + scale(6), centerY + scale(7)};
                RECT rightBar{
                    iconLeft + scale(10), centerY - scale(7),
                    iconLeft + scale(14), centerY + scale(7)};
                iconCanvas.FillRectangle(leftBar, textColor);
                iconCanvas.FillRectangle(rightBar, textColor);
            } else {
                const std::array<POINT, 3> triangle{{
                    {iconLeft + scale(3), centerY - scale(8)},
                    {iconLeft + scale(14), centerY},
                    {iconLeft + scale(3), centerY + scale(8)}}};
                iconCanvas.FillPolygon(triangle, textColor);
            }
        } else if (state.busy) {
            const int dot = scale(2);
            for (int index = 0; index < 3; ++index) {
                const int x = iconLeft + scale(3 + index * 5);
                iconCanvas.FillEllipse(
                    static_cast<float>(x),
                    static_cast<float>(centerY),
                    static_cast<float>(dot),
                    static_cast<float>(dot),
                    textColor);
            }
        } else if (isCopy) {
            const RECT back{
                iconLeft + scale(1), centerY - scale(5),
                iconLeft + scale(11), centerY + scale(7)};
            const RECT front{
                iconLeft + scale(5), centerY - scale(8),
                iconLeft + scale(15), centerY + scale(4)};
            iconCanvas.StrokeRoundedRectangle(
                back,
                static_cast<float>(scale(3)),
                textColor,
                std::max(1.0F, scaleFloat(1.5F)));
            iconCanvas.StrokeRoundedRectangle(
                front,
                static_cast<float>(scale(3)),
                textColor,
                std::max(1.0F, scaleFloat(1.5F)));
        } else {
            const float lineWidth = std::max(1.0F, scaleFloat(1.5F));
            iconCanvas.DrawLine(
                static_cast<float>(iconLeft + scale(8)),
                static_cast<float>(centerY - scale(8)),
                static_cast<float>(iconLeft + scale(8)),
                static_cast<float>(centerY + scale(4)),
                textColor,
                lineWidth);
            iconCanvas.DrawLine(
                static_cast<float>(iconLeft + scale(3)),
                static_cast<float>(centerY - scale(1)),
                static_cast<float>(iconLeft + scale(8)),
                static_cast<float>(centerY + scale(4)),
                textColor,
                lineWidth);
            iconCanvas.DrawLine(
                static_cast<float>(iconLeft + scale(8)),
                static_cast<float>(centerY + scale(4)),
                static_cast<float>(iconLeft + scale(13)),
                static_cast<float>(centerY - scale(1)),
                textColor,
                lineWidth);
            iconCanvas.DrawLine(
                static_cast<float>(iconLeft + scale(2)),
                static_cast<float>(centerY + scale(8)),
                static_cast<float>(iconLeft + scale(14)),
                static_cast<float>(centerY + scale(8)),
                textColor,
                lineWidth);
        }
    }

    if (isSegment && selectionAmount > 0.001F) {
        const int glyphLeft = bounds.left + scale(6);
        ui::Canvas glyphCanvas(dc);
        const float lineWidth = std::max(1.0F, scaleFloat(1.5F));
        const COLORREF glyphColor = ui::InterpolateColor(
            fill,
            segmentSelectedBorder,
            selectionAmount);
        glyphCanvas.DrawLine(
            static_cast<float>(glyphLeft),
            static_cast<float>(centerY),
            static_cast<float>(glyphLeft + scale(3)),
            static_cast<float>(centerY + scale(3)),
            glyphColor,
            lineWidth);
        glyphCanvas.DrawLine(
            static_cast<float>(glyphLeft + scale(3)),
            static_cast<float>(centerY + scale(3)),
            static_cast<float>(glyphLeft + scale(8)),
            static_cast<float>(centerY - scale(4)),
            glyphColor,
            lineWidth);
    }

    RECT textRectangle{
        contentLeft + iconWidth + iconGap,
        bounds.top,
        bounds.right,
        bounds.bottom};
    ::DrawTextW(
        dc,
        labelText,
        labelLength,
        &textRectangle,
        DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

    if (focusAmount > 0.001F && enabled) {
        RECT focus = bounds;
        ::InflateRect(&focus, -scale(3), -scale(3));
        const COLORREF targetFocusColor = isPrimary
            ? editor_theme::White
            : editor_theme::Focus;
        const COLORREF focusColor = ui::InterpolateColor(
            fill,
            targetFocusColor,
            focusAmount);
        const int focusWidth = std::max(1, scale(2));
        if (isSegment) {
            DrawSegmentFocusRing(
                dc,
                focus,
                radius,
                focusColor,
                focusWidth,
                state.role == EditorButtonRole::SegmentLeft
                    ? SegmentSide::Left
                    : SegmentSide::Right);
        } else {
            ui::Canvas focusCanvas(dc);
            focusCanvas.StrokeRoundedRectangle(
                focus,
                static_cast<float>(radius),
                focusColor,
                static_cast<float>(focusWidth));
        }
    }
    ::SelectObject(dc, previousFont);
    return true;
}

}  // namespace qrec
