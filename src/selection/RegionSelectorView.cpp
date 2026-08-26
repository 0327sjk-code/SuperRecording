#include "RegionSelectorView.h"

#include <algorithm>
#include <array>
#include <cwchar>

#include "../ui/AntiAliasedDrawing.h"
#include "../ui/Theme.h"

namespace qrec::selection::view {
namespace {

[[nodiscard]] int ScaleForDpi(int value, UINT dpi) noexcept {
    return MulDiv(value, static_cast<int>(dpi), USER_DEFAULT_SCREEN_DPI);
}

void FillSolidRectangle(HDC device, const RECT& bounds, COLORREF color) {
    const HBRUSH brush = CreateSolidBrush(color);
    if (brush != nullptr) {
        FillRect(device, &bounds, brush);
        DeleteObject(brush);
    }
}

void FillRoundedRectangle(HDC device, const RECT& bounds, int radius, COLORREF color) {
    ui::Canvas canvas(device);
    if (canvas.Valid()) {
        canvas.FillRoundedRectangle(bounds, static_cast<float>(radius), color);
        return;
    }
    const HBRUSH brush = CreateSolidBrush(color);
    const HPEN pen = CreatePen(PS_SOLID, 1, color);
    if (brush == nullptr || pen == nullptr) {
        if (pen != nullptr) {
            DeleteObject(pen);
        }
        if (brush != nullptr) {
            DeleteObject(brush);
        }
        return;
    }

    const HGDIOBJ oldBrush = SelectObject(device, brush);
    const HGDIOBJ oldPen = SelectObject(device, pen);
    const int diameter = std::max(2, radius * 2);
    RoundRect(device, bounds.left, bounds.top, bounds.right, bounds.bottom, diameter, diameter);
    SelectObject(device, oldPen);
    SelectObject(device, oldBrush);
    DeleteObject(pen);
    DeleteObject(brush);
}

void DrawRectangleOutline(HDC device, const RECT& bounds, int width, COLORREF color) {
    const HPEN pen = CreatePen(PS_INSIDEFRAME, std::max(1, width), color);
    if (pen == nullptr) {
        return;
    }

    const HGDIOBJ oldPen = SelectObject(device, pen);
    const HGDIOBJ oldBrush = SelectObject(device, GetStockObject(HOLLOW_BRUSH));
    Rectangle(device, bounds.left, bounds.top, bounds.right, bounds.bottom);
    SelectObject(device, oldBrush);
    SelectObject(device, oldPen);
    DeleteObject(pen);
}

void DrawCircle(HDC device, POINT center, int radius, COLORREF color) {
    ui::Canvas canvas(device);
    if (canvas.Valid()) {
        canvas.FillEllipse(
            static_cast<float>(center.x),
            static_cast<float>(center.y),
            static_cast<float>(radius),
            static_cast<float>(radius),
            color);
        return;
    }
    const HBRUSH brush = CreateSolidBrush(color);
    if (brush == nullptr) {
        return;
    }

    const HGDIOBJ oldBrush = SelectObject(device, brush);
    const HGDIOBJ oldPen = SelectObject(device, GetStockObject(NULL_PEN));
    Ellipse(
        device,
        center.x - radius,
        center.y - radius,
        center.x + radius + 1,
        center.y + radius + 1);
    SelectObject(device, oldPen);
    SelectObject(device, oldBrush);
    DeleteObject(brush);
}

[[nodiscard]] HFONT CreateUiFont(UINT dpi, int size, int weight) {
    return CreateFontW(
        -ScaleForDpi(size, dpi),
        0,
        0,
        0,
        weight,
        FALSE,
        FALSE,
        FALSE,
        DEFAULT_CHARSET,
        OUT_TT_ONLY_PRECIS,
        CLIP_DEFAULT_PRECIS,
        theme::FontQuality,
        VARIABLE_PITCH | FF_SWISS,
        theme::FontFamily);
}

[[nodiscard]] RECT ActiveMonitorBounds(HWND window, const RECT& fallback) noexcept {
    POINT cursor{};
    if (!GetCursorPos(&cursor)) {
        return fallback;
    }

    const HMONITOR monitor = MonitorFromPoint(cursor, MONITOR_DEFAULTTONEAREST);
    MONITORINFO information{};
    information.cbSize = sizeof(information);
    if (monitor == nullptr || !GetMonitorInfoW(monitor, &information)) {
        return fallback;
    }

    POINT corners[2]{
        {information.rcMonitor.left, information.rcMonitor.top},
        {information.rcMonitor.right, information.rcMonitor.bottom},
    };
    SetLastError(ERROR_SUCCESS);
    if (MapWindowPoints(nullptr, window, corners, 2) == 0 && GetLastError() != ERROR_SUCCESS) {
        return fallback;
    }
    return RECT{corners[0].x, corners[0].y, corners[1].x, corners[1].y};
}

void DrawInstruction(
    HWND window,
    HDC device,
    const RECT& client,
    UINT dpi,
    const SelectionUiState& state) {
    const HFONT titleFont = CreateUiFont(dpi, 14, FW_BOLD);
    const HFONT detailFont = CreateUiFont(dpi, 13, FW_NORMAL);
    if (titleFont == nullptr || detailFont == nullptr) {
        if (detailFont != nullptr) {
            DeleteObject(detailFont);
        }
        if (titleFont != nullptr) {
            DeleteObject(titleFont);
        }
        return;
    }

    const wchar_t* title = state.adjusting
        ? L"调整录制区域"
        : L"拖动框选录制区域";
    const wchar_t* detail = state.adjusting
        ? (state.selectionValid
              ? L"拖动边缘或区域   Enter 开始   Esc 取消"
              : L"选区不能跨越显示器，请调整后再开始")
        : L"单击全屏   Esc 取消";
    const HGDIOBJ oldFont = SelectObject(device, titleFont);
    SIZE titleSize{};
    GetTextExtentPoint32W(device, title, static_cast<int>(wcslen(title)), &titleSize);
    SelectObject(device, detailFont);
    SIZE detailSize{};
    GetTextExtentPoint32W(device, detail, static_cast<int>(wcslen(detail)), &detailSize);

    const int horizontalPadding = ScaleForDpi(14, dpi);
    const int dotDiameter = ScaleForDpi(8, dpi);
    const int dotGap = ScaleForDpi(9, dpi);
    const int separatorGap = ScaleForDpi(12, dpi);
    const int separatorWidth = std::max(1, ScaleForDpi(1, dpi));
    const int width = horizontalPadding * 2 + dotDiameter + dotGap + titleSize.cx +
        separatorGap * 2 + separatorWidth + detailSize.cx;
    const int height = ScaleForDpi(42, dpi);

    RECT monitorBounds = ActiveMonitorBounds(window, client);
    monitorBounds.left = std::clamp(monitorBounds.left, client.left, client.right);
    monitorBounds.top = std::clamp(monitorBounds.top, client.top, client.bottom);
    monitorBounds.right = std::clamp(monitorBounds.right, client.left, client.right);
    monitorBounds.bottom = std::clamp(monitorBounds.bottom, client.top, client.bottom);
    const int monitorWidth = monitorBounds.right - monitorBounds.left;
    LONG left = monitorBounds.left + (monitorWidth - width) / 2;
    left = std::clamp<LONG>(
        left,
        monitorBounds.left + ScaleForDpi(12, dpi),
        std::max(
            monitorBounds.left + ScaleForDpi(12, dpi),
            monitorBounds.right - width - ScaleForDpi(12, dpi)));
    const int top = monitorBounds.top + ScaleForDpi(24, dpi);
    const RECT panel{left, top, left + width, top + height};
    FillRoundedRectangle(device, panel, ScaleForDpi(theme::CornerMedium, dpi), theme::OverlayPanel);

    const POINT dotCenter{
        panel.left + horizontalPadding + dotDiameter / 2,
        (panel.top + panel.bottom) / 2};
    DrawCircle(device, dotCenter, dotDiameter / 2, theme::Primary);

    SetBkMode(device, TRANSPARENT);
    SelectObject(device, titleFont);
    SetTextColor(device, theme::Background);
    RECT titleBounds{
        dotCenter.x + dotDiameter / 2 + dotGap,
        panel.top,
        dotCenter.x + dotDiameter / 2 + dotGap + titleSize.cx,
        panel.bottom};
    DrawTextW(
        device,
        title,
        -1,
        &titleBounds,
        DT_SINGLELINE | DT_VCENTER | DT_LEFT | DT_NOPREFIX);

    const int separatorX = titleBounds.right + separatorGap;
    RECT separator{
        separatorX,
        panel.top + ScaleForDpi(11, dpi),
        separatorX + separatorWidth,
        panel.bottom - ScaleForDpi(11, dpi)};
    FillSolidRectangle(device, separator, theme::BorderStrong);

    SelectObject(device, detailFont);
    SetTextColor(device, theme::OverlayPanelMuted);
    RECT detailBounds{
        separator.right + separatorGap,
        panel.top,
        panel.right - horizontalPadding,
        panel.bottom};
    DrawTextW(
        device,
        detail,
        -1,
        &detailBounds,
        DT_SINGLELINE | DT_VCENTER | DT_LEFT | DT_NOPREFIX | DT_END_ELLIPSIS);

    SelectObject(device, oldFont);
    DeleteObject(detailFont);
    DeleteObject(titleFont);
}

void DrawSizeBadge(
    HDC device,
    const RECT& selection,
    const RECT& client,
    UINT dpi,
    bool adjusting) {
    wchar_t dimensions[64]{};
    swprintf_s(
        dimensions,
        L"%ld × %ld",
        selection.right - selection.left,
        selection.bottom - selection.top);

    const HFONT font = CreateUiFont(dpi, 13, FW_BOLD);
    if (font == nullptr) {
        return;
    }
    const HGDIOBJ oldFont = SelectObject(device, font);
    SIZE textSize{};
    GetTextExtentPoint32W(device, dimensions, static_cast<int>(wcslen(dimensions)), &textSize);

    const int horizontalPadding = ScaleForDpi(10, dpi);
    const int verticalPadding = ScaleForDpi(5, dpi);
    const int gap = ScaleForDpi(8, dpi);
    const int width = textSize.cx + horizontalPadding * 2;
    const int height = textSize.cy + verticalPadding * 2;
    RECT badge{};
    if (adjusting && selection.right - selection.left >= width + gap * 2 &&
        selection.bottom - selection.top >= height + gap * 2) {
        badge = RECT{
            selection.left + gap,
            selection.top + gap,
            selection.left + gap + width,
            selection.top + gap + height};
    } else {
        badge = RECT{
            selection.left,
            selection.bottom + gap,
            selection.left + width,
            selection.bottom + gap + height};
        if (badge.bottom > client.bottom - ScaleForDpi(8, dpi)) {
            badge.bottom = selection.top - gap;
            badge.top = badge.bottom - height;
        }
        if (badge.top < client.top + ScaleForDpi(8, dpi)) {
            badge.top = selection.top + gap;
            badge.bottom = badge.top + height;
        }
    }

    badge.left = std::clamp(
        badge.left,
        client.left + ScaleForDpi(8, dpi),
        std::max(
            client.left + ScaleForDpi(8, dpi),
            client.right - width - ScaleForDpi(8, dpi)));
    badge.right = badge.left + width;
    FillRoundedRectangle(device, badge, ScaleForDpi(theme::CornerSmall, dpi), theme::OverlayPanel);

    SetBkMode(device, TRANSPARENT);
    SetTextColor(device, theme::Background);
    RECT textBounds = badge;
    DrawTextW(
        device,
        dimensions,
        -1,
        &textBounds,
        DT_SINGLELINE | DT_VCENTER | DT_CENTER | DT_NOPREFIX);

    SelectObject(device, oldFont);
    DeleteObject(font);
}

void DrawHandles(
    HDC device,
    const RECT& selection,
    const RECT& client,
    UINT dpi,
    bool selectionValid) {
    const int radius = std::max(4, ScaleForDpi(5, dpi));
    const int innerRadius = std::max(2, radius - std::max(2, ScaleForDpi(2, dpi)));
    const LONG right = std::max(selection.left, selection.right - 1);
    const LONG bottom = std::max(selection.top, selection.bottom - 1);
    const std::array<POINT, 8> handles{{
        {selection.left, selection.top},
        {(selection.left + right) / 2, selection.top},
        {right, selection.top},
        {selection.left, (selection.top + bottom) / 2},
        {right, (selection.top + bottom) / 2},
        {selection.left, bottom},
        {(selection.left + right) / 2, bottom},
        {right, bottom},
    }};

    for (POINT handle : handles) {
        handle.x = std::clamp<LONG>(
            handle.x,
            client.left + radius,
            std::max<LONG>(client.left + radius, client.right - radius - 1));
        handle.y = std::clamp<LONG>(
            handle.y,
            client.top + radius,
            std::max<LONG>(client.top + radius, client.bottom - radius - 1));
        DrawCircle(device, handle, radius, selectionValid ? theme::Primary : theme::Danger);
        DrawCircle(device, handle, innerRadius, theme::Background);
    }
}

void DrawControlButton(
    HDC device,
    const RECT& bounds,
    const wchar_t* label,
    UINT dpi,
    bool primary,
    bool enabled,
    bool hovered,
    bool pressed) {
    constexpr COLORREF kNeutral = RGB(48, 48, 46);
    constexpr COLORREF kNeutralHover = RGB(58, 58, 55);
    constexpr COLORREF kNeutralPressed = RGB(39, 39, 37);
    constexpr COLORREF kDisabled = RGB(56, 56, 53);
    constexpr COLORREF kDisabledText = RGB(139, 139, 134);

    COLORREF fill = primary ? theme::Primary : kNeutral;
    if (!enabled) {
        fill = kDisabled;
    } else if (pressed) {
        fill = primary ? theme::PrimaryPressed : kNeutralPressed;
    } else if (hovered) {
        fill = primary ? theme::PrimaryHover : kNeutralHover;
    }

    ui::Canvas canvas(device);
    if (canvas.Valid()) {
        canvas.DrawRoundedRectangle(
            bounds,
            static_cast<float>(ScaleForDpi(theme::CornerSmall, dpi)),
            fill,
            primary || !enabled ? fill : theme::BorderStrong,
            static_cast<float>(std::max(1, ScaleForDpi(1, dpi))));
    } else {
        FillRoundedRectangle(
            device,
            bounds,
            ScaleForDpi(theme::CornerSmall, dpi),
            fill);
    }

    const HFONT font = CreateUiFont(dpi, 13, FW_BOLD);
    if (font == nullptr) {
        return;
    }
    const HGDIOBJ oldFont = SelectObject(device, font);
    SetBkMode(device, TRANSPARENT);
    SetTextColor(device, enabled ? theme::Background : kDisabledText);
    RECT textBounds = bounds;
    DrawTextW(
        device,
        label,
        -1,
        &textBounds,
        DT_SINGLELINE | DT_VCENTER | DT_CENTER | DT_NOPREFIX);
    SelectObject(device, oldFont);
    DeleteObject(font);
}

void DrawControlBar(
    HDC device,
    const RECT& client,
    const RECT& selection,
    UINT dpi,
    const SelectionUiState& state) {
    if (!state.adjusting || IsEmpty(selection)) {
        return;
    }

    const ControlLayout layout = CalculateControlLayout(
        client,
        selection,
        state.activeMonitorBounds,
        dpi);
    FillRoundedRectangle(
        device,
        layout.panel,
        ScaleForDpi(theme::CornerMedium, dpi),
        theme::OverlayPanel);
    DrawControlButton(
        device,
        layout.startButton,
        L"开始录制",
        dpi,
        true,
        state.selectionValid,
        state.hoveredButton == ControlButton::Start,
        state.pressedButton == ControlButton::Start &&
            state.hoveredButton == ControlButton::Start);
    DrawControlButton(
        device,
        layout.cancelButton,
        L"取消",
        dpi,
        false,
        true,
        state.hoveredButton == ControlButton::Cancel,
        state.pressedButton == ControlButton::Cancel &&
            state.hoveredButton == ControlButton::Cancel);
}

void RenderFrame(
    HWND window,
    HDC device,
    const RECT& client,
    const RECT& selection,
    UINT dpi,
    const SelectionUiState& state) {
    FillSolidRectangle(device, client, theme::OverlayMask);

    if (!IsEmpty(selection)) {
        FillSolidRectangle(device, selection, TransparentColor);
        DrawRectangleOutline(
            device,
            selection,
            std::max(2, ScaleForDpi(2, dpi)),
            state.selectionValid ? theme::Primary : theme::Danger);
        DrawHandles(device, selection, client, dpi, state.selectionValid);
        DrawSizeBadge(device, selection, client, dpi, state.adjusting);
    }

    DrawInstruction(window, device, client, dpi, state);
    DrawControlBar(device, client, selection, dpi, state);
}

}  // namespace

FrameBuffer::~FrameBuffer() {
    Reset();
}

bool FrameBuffer::Ensure(const HDC referenceDevice, const SIZE size) noexcept {
    if (referenceDevice == nullptr || size.cx <= 0 || size.cy <= 0) {
        return false;
    }
    if (device_ != nullptr && bitmap_ != nullptr &&
        size_.cx == size.cx && size_.cy == size.cy) {
        return true;
    }

    Reset();
    device_ = CreateCompatibleDC(referenceDevice);
    bitmap_ = CreateCompatibleBitmap(referenceDevice, size.cx, size.cy);
    if (device_ == nullptr || bitmap_ == nullptr) {
        Reset();
        return false;
    }

    previousBitmap_ = SelectObject(device_, bitmap_);
    if (previousBitmap_ == nullptr || previousBitmap_ == HGDI_ERROR) {
        previousBitmap_ = nullptr;
        Reset();
        return false;
    }
    size_ = size;
    return true;
}

void FrameBuffer::Reset() noexcept {
    if (device_ != nullptr && previousBitmap_ != nullptr) {
        SelectObject(device_, previousBitmap_);
    }
    previousBitmap_ = nullptr;
    if (bitmap_ != nullptr) {
        DeleteObject(bitmap_);
        bitmap_ = nullptr;
    }
    if (device_ != nullptr) {
        DeleteDC(device_);
        device_ = nullptr;
    }
    size_ = SIZE{};
}

bool IsEmpty(const RECT& rectangle) noexcept {
    return rectangle.right <= rectangle.left || rectangle.bottom <= rectangle.top;
}

ControlLayout CalculateControlLayout(
    const RECT& client,
    const RECT& selection,
    const RECT& activeMonitorBounds,
    const UINT dpi) noexcept {
    const int outerPadding = ScaleForDpi(6, dpi);
    const int buttonGap = ScaleForDpi(6, dpi);
    const int buttonHeight = ScaleForDpi(36, dpi);
    const int preferredStartWidth = ScaleForDpi(92, dpi);
    const int preferredCancelWidth = ScaleForDpi(64, dpi);
    const int preferredPanelWidth =
        outerPadding * 2 + preferredStartWidth + buttonGap + preferredCancelWidth;
    const int panelHeight = outerPadding * 2 + buttonHeight;
    const int selectionGap = ScaleForDpi(9, dpi);
    const int monitorMargin = ScaleForDpi(8, dpi);

    RECT monitor = activeMonitorBounds;
    monitor.left = std::clamp(monitor.left, client.left, client.right);
    monitor.top = std::clamp(monitor.top, client.top, client.bottom);
    monitor.right = std::clamp(monitor.right, client.left, client.right);
    monitor.bottom = std::clamp(monitor.bottom, client.top, client.bottom);
    if (monitor.right <= monitor.left || monitor.bottom <= monitor.top) {
        monitor = client;
    }

    const int availablePanelWidth = static_cast<int>(std::max<LONG>(
        1L,
        monitor.right - monitor.left - monitorMargin * 2));
    const int panelWidth = std::min(preferredPanelWidth, availablePanelWidth);
    const int availableButtonWidth = std::max(
        2,
        panelWidth - outerPadding * 2 - buttonGap);
    const int cancelWidth = std::min(
        preferredCancelWidth,
        std::max(1, availableButtonWidth * 2 / 5));
    const int startWidth = std::max(1, availableButtonWidth - cancelWidth);

    LONG left = selection.right - panelWidth;
    LONG top = selection.bottom + selectionGap;
    if (top + panelHeight > monitor.bottom - monitorMargin) {
        top = selection.top - selectionGap - panelHeight;
    }
    if (top < monitor.top + monitorMargin) {
        // Extremely short selections and small rotated displays use an
        // in-frame placement rather than allowing the confirmation controls
        // to leave the active monitor.
        top = selection.bottom - panelHeight - selectionGap;
    }

    const LONG minimumLeft = monitor.left + monitorMargin;
    const LONG maximumLeft = std::max<LONG>(
        minimumLeft,
        monitor.right - panelWidth - monitorMargin);
    left = std::clamp(left, minimumLeft, maximumLeft);
    const LONG minimumTop = monitor.top + monitorMargin;
    const LONG maximumTop = std::max<LONG>(
        minimumTop,
        monitor.bottom - panelHeight - monitorMargin);
    top = std::clamp(top, minimumTop, maximumTop);

    ControlLayout layout;
    layout.panel = RECT{left, top, left + panelWidth, top + panelHeight};
    layout.startButton = RECT{
        layout.panel.left + outerPadding,
        layout.panel.top + outerPadding,
        layout.panel.left + outerPadding + startWidth,
        layout.panel.bottom - outerPadding};
    layout.cancelButton = RECT{
        layout.startButton.right + buttonGap,
        layout.startButton.top,
        layout.startButton.right + buttonGap + cancelWidth,
        layout.startButton.bottom};
    return layout;
}

void Paint(
    HWND window,
    const RECT& selection,
    const UINT dpi,
    const SelectionUiState& state,
    FrameBuffer& frameBuffer) {
    PAINTSTRUCT paint{};
    const HDC device = BeginPaint(window, &paint);
    if (device == nullptr) {
        return;
    }

    RECT client{};
    GetClientRect(window, &client);
    const SIZE size{
        std::max<LONG>(1, client.right - client.left),
        std::max<LONG>(1, client.bottom - client.top)};
    if (frameBuffer.Ensure(device, size)) {
        RenderFrame(window, frameBuffer.Device(), client, selection, dpi, state);
        BitBlt(device, 0, 0, size.cx, size.cy, frameBuffer.Device(), 0, 0, SRCCOPY);
    } else {
        // Preserve usability even if a very large virtual desktop cannot
        // allocate its backing bitmap. This fallback is intentionally rare.
        RenderFrame(window, device, client, selection, dpi, state);
    }
    EndPaint(window, &paint);
}

}  // namespace qrec::selection::view
