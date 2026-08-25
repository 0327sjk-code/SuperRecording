#include "RecordingOverlayView.h"

#include <algorithm>
#include <array>
#include <cwchar>
#include <objidl.h>

#include "../ui/AntiAliasedDrawing.h"
#include "../ui/Motion.h"
#include "../ui/Theme.h"

namespace qrec::overlay::view {
namespace {

constexpr int kBaseWidth = 250;
constexpr int kBaseHeight = 40;
constexpr int kBaseDragWidth = 22;
constexpr int kBaseStatusLeft = 26;
constexpr int kBaseStatusRight = 119;
constexpr int kBasePauseLeft = 122;
constexpr int kBasePauseRight = 181;
constexpr int kBaseStopLeft = 184;
constexpr int kBaseStopRight = 246;
constexpr int kBaseControlTop = 4;
constexpr int kBaseControlBottom = 36;
constexpr int kBaseTextBaseline = 25;

struct ButtonPalette final {
    COLORREF background{};
    COLORREF foreground{};
    COLORREF border{};
    bool bordered{};
};

[[nodiscard]] int ScaleForDpi(int value, UINT dpi) noexcept {
    return MulDiv(value, static_cast<int>(dpi), USER_DEFAULT_SCREEN_DPI);
}

[[nodiscard]] float ScaleForDpi(float value, UINT dpi) noexcept {
    return value * static_cast<float>(dpi) / static_cast<float>(USER_DEFAULT_SCREEN_DPI);
}

[[nodiscard]] ButtonPalette PausePalette(const VisualState& state) noexcept {
    if (state.stopping) {
        return ButtonPalette{
            theme::SurfaceDisabled,
            theme::InkDisabled,
            theme::SurfaceDisabled,
            false};
    }
    const COLORREF baseBackground = ui::InterpolateColor(
        theme::SurfaceMuted,
        theme::PrimarySubtle,
        state.pauseStateAmount);
    const COLORREF hoverBackground = ui::InterpolateColor(
        theme::SurfaceHover,
        theme::PrimarySubtleHover,
        state.pauseStateAmount);
    const COLORREF pressBackground = ui::InterpolateColor(
        theme::SurfacePressed,
        theme::PrimarySubtlePressed,
        state.pauseStateAmount);
    COLORREF background = ui::InterpolateColor(
        baseBackground,
        hoverBackground,
        state.pauseHoverAmount);
    background = ui::InterpolateColor(
        background,
        pressBackground,
        state.pausePressAmount);
    COLORREF foreground = ui::InterpolateColor(
        theme::Ink,
        theme::PrimaryPressed,
        state.pauseStateAmount);
    foreground = ui::InterpolateColor(
        foreground,
        theme::PrimaryHover,
        state.pauseHoverAmount);
    COLORREF border = ui::InterpolateColor(
        theme::BorderSubtle,
        theme::PrimarySubtleHover,
        state.pauseStateAmount);
    border = ui::InterpolateColor(
        border,
        theme::BorderHover,
        state.pauseHoverAmount);
    border = ui::InterpolateColor(
        border,
        theme::Primary,
        state.pausePressAmount);
    return ButtonPalette{background, foreground, border, true};
}

[[nodiscard]] ButtonPalette StopPalette(const VisualState& state) noexcept {
    COLORREF background = ui::InterpolateColor(
        theme::Danger,
        theme::DangerHover,
        state.stopHoverAmount);
    background = ui::InterpolateColor(
        background,
        theme::DangerPressed,
        state.stopPressAmount);
    background = ui::InterpolateColor(
        background,
        theme::DangerSubtle,
        state.stopStateAmount);
    const COLORREF foreground = ui::InterpolateColor(
        theme::White,
        theme::DangerMuted,
        state.stopStateAmount);
    return ButtonPalette{background, foreground, background, false};
}

void DrawGrip(ui::Canvas& canvas, const VisualState& state, int height) {
    COLORREF color = ui::InterpolateColor(
        theme::Muted,
        theme::Ink,
        state.dragHoverAmount);
    color = ui::InterpolateColor(color, theme::Primary, state.dragPressAmount);
    const float radius = std::max(1.0F, ScaleForDpi(1.05F, state.dpi));
    const float firstX = ScaleForDpi(8.0F, state.dpi);
    const float secondX = ScaleForDpi(14.0F, state.dpi);
    const float centerY = static_cast<float>(height) * 0.5F;
    const float rowGap = ScaleForDpi(5.0F, state.dpi);
    for (int row = -1; row <= 1; ++row) {
        const float y = centerY + static_cast<float>(row) * rowGap;
        canvas.FillEllipse(firstX, y, radius, radius, color);
        canvas.FillEllipse(secondX, y, radius, radius, color);
    }
    const float dividerX = ScaleForDpi(21.5F, state.dpi);
    canvas.DrawLine(
        dividerX,
        ScaleForDpi(10.0F, state.dpi),
        dividerX,
        static_cast<float>(height) - ScaleForDpi(10.0F, state.dpi),
        theme::BorderSubtle,
        std::max(1.0F, ScaleForDpi(1.0F, state.dpi)));
}

void DrawPauseGlyph(
    ui::Canvas& canvas,
    POINT center,
    const VisualState& state,
    COLORREF color) {
    if (state.paused) {
        const int halfHeight = ScaleForDpi(4, state.dpi);
        const int left = center.x - ScaleForDpi(3, state.dpi);
        const std::array<POINT, 3> triangle{{
            {left, center.y - halfHeight},
            {left, center.y + halfHeight},
            {center.x + ScaleForDpi(4, state.dpi), center.y},
        }};
        canvas.FillPolygon(triangle, color);
        return;
    }

    const float halfHeight = ScaleForDpi(4.0F, state.dpi);
    const float barOffset = ScaleForDpi(2.5F, state.dpi);
    const float barWidth = std::max(1.0F, ScaleForDpi(1.5F, state.dpi));
    canvas.DrawLine(
        static_cast<float>(center.x) - barOffset,
        static_cast<float>(center.y) - halfHeight,
        static_cast<float>(center.x) - barOffset,
        static_cast<float>(center.y) + halfHeight,
        color,
        barWidth);
    canvas.DrawLine(
        static_cast<float>(center.x) + barOffset,
        static_cast<float>(center.y) - halfHeight,
        static_cast<float>(center.x) + barOffset,
        static_cast<float>(center.y) + halfHeight,
        color,
        barWidth);
}

void DrawStopGlyph(
    ui::Canvas& canvas,
    POINT center,
    const VisualState& state,
    COLORREF color) {
    const int halfSize = ScaleForDpi(4, state.dpi);
    const RECT glyphBounds{
        center.x - halfSize,
        center.y - halfSize,
        center.x + halfSize,
        center.y + halfSize};
    canvas.FillRoundedRectangle(
        glyphBounds,
        ScaleForDpi(1.75F, state.dpi),
        color);
}

void DrawButton(
    ui::Canvas& canvas,
    const RECT& bounds,
    UINT dpi,
    const ButtonPalette& palette) {
    const float radius = ScaleForDpi(static_cast<float>(theme::CornerSmall), dpi);
    canvas.FillRoundedRectangle(bounds, radius, palette.background);
    if (palette.bordered) {
        canvas.StrokeRoundedRectangle(
            bounds,
            radius,
            palette.border,
            std::max(1.0F, ScaleForDpi(1.0F, dpi)));
    }
}

void DrawVectorLayer(
    HDC device,
    HWND window,
    const RECT& client,
    const VisualState& state) {
    ui::Canvas canvas(device);
    if (!canvas.Valid()) {
        return;
    }

    const float outerRadius = ScaleForDpi(
        static_cast<float>(theme::CornerMedium), state.dpi);
    canvas.FillRoundedRectangle(client, outerRadius, theme::Background);

    const ButtonPalette pausePalette = PausePalette(state);
    const ButtonPalette stopPalette = StopPalette(state);
    const RECT pauseBounds = PauseBounds(state.dpi);
    const RECT stopBounds = StopBounds(state.dpi);
    DrawButton(canvas, pauseBounds, state.dpi, pausePalette);
    DrawButton(canvas, stopBounds, state.dpi, stopPalette);

    const int height = client.bottom - client.top;
    DrawGrip(canvas, state, height);

    const RECT statusBounds = StatusBounds(state.dpi);
    COLORREF statusColor = ui::InterpolateColor(
        theme::Danger,
        theme::Primary,
        state.pauseStateAmount);
    statusColor = ui::InterpolateColor(
        statusColor,
        theme::InkDisabled,
        state.stopStateAmount);
    const float statusRadius = std::max(2.25F, ScaleForDpi(2.5F, state.dpi));
    canvas.FillEllipse(
        static_cast<float>(statusBounds.left) + statusRadius,
        static_cast<float>(height) * 0.5F,
        statusRadius,
        statusRadius,
        statusColor);

    const POINT pauseCenter{
        pauseBounds.left + ScaleForDpi(13, state.dpi),
        (pauseBounds.top + pauseBounds.bottom) / 2};
    DrawPauseGlyph(canvas, pauseCenter, state, pausePalette.foreground);

    const POINT stopCenter{
        stopBounds.left + ScaleForDpi(13, state.dpi),
        (stopBounds.top + stopBounds.bottom) / 2};
    DrawStopGlyph(canvas, stopCenter, state, stopPalette.foreground);

    canvas.StrokeRoundedRectangle(
        client,
        outerRadius,
        theme::BorderHover,
        std::max(1.0F, ScaleForDpi(1.0F, state.dpi)));
    if (GetFocus() == window) {
        RECT focusBounds = client;
        InflateRect(
            &focusBounds,
            -ScaleForDpi(2, state.dpi),
            -ScaleForDpi(2, state.dpi));
        canvas.StrokeRoundedRectangle(
            focusBounds,
            std::max(1.0F, outerRadius - ScaleForDpi(2.0F, state.dpi)),
            theme::Focus,
            std::max(1.5F, ScaleForDpi(1.5F, state.dpi)));
    }
}

[[nodiscard]] std::array<wchar_t, 32> FormatElapsed(
    std::chrono::milliseconds elapsed) {
    std::array<wchar_t, 32> text{};
    const auto totalSeconds = std::chrono::duration_cast<std::chrono::seconds>(elapsed).count();
    const long long hours = totalSeconds / 3600;
    const long long minutes = (totalSeconds / 60) % 60;
    const long long seconds = totalSeconds % 60;
    swprintf_s(text.data(), text.size(), L"%02lld:%02lld:%02lld", hours, minutes, seconds);
    return text;
}

void DrawTextAtSharedBaseline(
    HDC device,
    HFONT font,
    const wchar_t* text,
    RECT bounds,
    UINT dpi,
    COLORREF color,
    UINT extraFlags = 0) {
    const HGDIOBJ selectedFont = font != nullptr
        ? static_cast<HGDIOBJ>(font)
        : GetStockObject(DEFAULT_GUI_FONT);
    const HGDIOBJ previousFont = SelectObject(device, selectedFont);
    TEXTMETRICW metrics{};
    GetTextMetricsW(device, &metrics);
    const int baseline = ScaleForDpi(kBaseTextBaseline, dpi);
    bounds.top = baseline - metrics.tmAscent;
    bounds.bottom = bounds.top + metrics.tmHeight + ScaleForDpi(1, dpi);
    SetTextColor(device, color);
    DrawTextW(
        device,
        text,
        -1,
        &bounds,
        DT_SINGLELINE | DT_TOP | DT_LEFT | DT_NOPREFIX | extraFlags);
    SelectObject(device, previousFont);
}

void DrawStatusText(HDC device, const VisualState& state) {
    const RECT statusBounds = StatusBounds(state.dpi);
    const int height = statusBounds.bottom - statusBounds.top;
    const int dotWidth = ScaleForDpi(10, state.dpi);
    const int labelWidth = ScaleForDpi(25, state.dpi);
    RECT labelBounds{
        statusBounds.left + dotWidth,
        statusBounds.top,
        statusBounds.left + dotWidth + labelWidth,
        statusBounds.bottom};
    const wchar_t* label = state.stopping
        ? L"结束"
        : (state.paused ? L"暂停" : L"录制");
    const COLORREF labelColor = state.stopping ? theme::InkDisabled : theme::Ink;
    DrawTextAtSharedBaseline(
        device,
        state.bodyFont,
        label,
        labelBounds,
        state.dpi,
        labelColor);

    const auto elapsedText = FormatElapsed(state.elapsed);
    RECT timerBounds{
        labelBounds.right + ScaleForDpi(3, state.dpi),
        statusBounds.top,
        statusBounds.right,
        statusBounds.top + height};
    DrawTextAtSharedBaseline(
        device,
        state.timerFont,
        elapsedText.data(),
        timerBounds,
        state.dpi,
        state.stopping ? theme::InkDisabled : theme::Ink,
        DT_END_ELLIPSIS);
}

void DrawPauseText(HDC device, const VisualState& state) {
    const ButtonPalette palette = PausePalette(state);
    RECT bounds = PauseBounds(state.dpi);
    bounds.left += ScaleForDpi(24, state.dpi);
    bounds.right -= ScaleForDpi(4, state.dpi);
    DrawTextAtSharedBaseline(
        device,
        state.actionFont,
        state.paused ? L"继续" : L"暂停",
        bounds,
        state.dpi,
        palette.foreground,
        DT_END_ELLIPSIS);
}

void DrawStopText(HDC device, const VisualState& state) {
    const ButtonPalette palette = StopPalette(state);
    RECT bounds = StopBounds(state.dpi);
    bounds.left += ScaleForDpi(24, state.dpi);
    bounds.right -= ScaleForDpi(4, state.dpi);
    DrawTextAtSharedBaseline(
        device,
        state.actionFont,
        L"结束",
        bounds,
        state.dpi,
        palette.foreground,
        DT_END_ELLIPSIS);
}

}  // namespace

SIZE DesiredSize(UINT dpi) noexcept {
    return SIZE{ScaleForDpi(kBaseWidth, dpi), ScaleForDpi(kBaseHeight, dpi)};
}

RECT StatusBounds(UINT dpi) noexcept {
    const SIZE size = DesiredSize(dpi);
    return RECT{
        ScaleForDpi(kBaseStatusLeft, dpi),
        0,
        ScaleForDpi(kBaseStatusRight, dpi),
        size.cy};
}

RECT DragBounds(UINT dpi) noexcept {
    const SIZE size = DesiredSize(dpi);
    return RECT{0, 0, ScaleForDpi(kBaseDragWidth, dpi), size.cy};
}

RECT PauseBounds(UINT dpi) noexcept {
    return RECT{
        ScaleForDpi(kBasePauseLeft, dpi),
        ScaleForDpi(kBaseControlTop, dpi),
        ScaleForDpi(kBasePauseRight, dpi),
        ScaleForDpi(kBaseControlBottom, dpi)};
}

RECT StopBounds(UINT dpi) noexcept {
    return RECT{
        ScaleForDpi(kBaseStopLeft, dpi),
        ScaleForDpi(kBaseControlTop, dpi),
        ScaleForDpi(kBaseStopRight, dpi),
        ScaleForDpi(kBaseControlBottom, dpi)};
}

void ApplyRoundedWindowRegion(HWND window, UINT dpi) {
    if (window == nullptr) {
        return;
    }
    const SIZE size = DesiredSize(dpi);
    const int diameter = ScaleForDpi(theme::CornerMedium * 2, dpi);
    const HRGN region = CreateRoundRectRgn(
        0,
        0,
        size.cx + 1,
        size.cy + 1,
        diameter,
        diameter);
    if (region != nullptr && SetWindowRgn(window, region, TRUE) == 0) {
        DeleteObject(region);
    }
}

void Paint(HWND window, const VisualState& state) {
    PAINTSTRUCT paint{};
    const HDC target = BeginPaint(window, &paint);
    if (target == nullptr) {
        return;
    }

    RECT client{};
    GetClientRect(window, &client);
    const int width = client.right - client.left;
    const int height = client.bottom - client.top;
    const HDC buffer = CreateCompatibleDC(target);
    const HBITMAP bitmap = CreateCompatibleBitmap(target, width, height);
    if (buffer == nullptr || bitmap == nullptr) {
        if (bitmap != nullptr) {
            DeleteObject(bitmap);
        }
        if (buffer != nullptr) {
            DeleteDC(buffer);
        }
        EndPaint(window, &paint);
        return;
    }

    const HGDIOBJ oldBitmap = SelectObject(buffer, bitmap);
    const HBRUSH background = CreateSolidBrush(theme::Background);
    if (background != nullptr) {
        FillRect(buffer, &client, background);
        DeleteObject(background);
    }
    DrawVectorLayer(buffer, window, client, state);

    SetBkMode(buffer, TRANSPARENT);
    DrawStatusText(buffer, state);
    DrawPauseText(buffer, state);
    DrawStopText(buffer, state);

    BitBlt(target, 0, 0, width, height, buffer, 0, 0, SRCCOPY);
    SelectObject(buffer, oldBitmap);
    DeleteObject(bitmap);
    DeleteDC(buffer);
    EndPaint(window, &paint);
}

}  // namespace qrec::overlay::view
