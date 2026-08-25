#pragma once

#include <windows.h>

#include <algorithm>
#include <array>
#include <cstddef>

#include "../common/Types.h"

namespace qrec::overlay::region_geometry {

inline constexpr std::size_t LayerCount = 4;

[[nodiscard]] inline bool IsNonEmpty(const RECT& rectangle) noexcept {
    return rectangle.right > rectangle.left && rectangle.bottom > rectangle.top;
}

[[nodiscard]] inline RECT Intersect(const RECT& first, const RECT& second) noexcept {
    const RECT intersection{
        std::max(first.left, second.left),
        std::max(first.top, second.top),
        std::min(first.right, second.right),
        std::min(first.bottom, second.bottom)};
    return IsNonEmpty(intersection) ? intersection : RECT{};
}

[[nodiscard]] inline RECT VirtualDesktopBounds() noexcept {
    const int left = GetSystemMetrics(SM_XVIRTUALSCREEN);
    const int top = GetSystemMetrics(SM_YVIRTUALSCREEN);
    const int width = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    const int height = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    if (width <= 0 || height <= 0) {
        return RECT{};
    }
    return RECT{left, top, left + width, top + height};
}

// Win32 rectangles are half-open. Top and bottom own the full desktop width;
// left and right own only the clear region's vertical span. The four windows
// therefore cover every outside pixel exactly once, including negative-origin
// multi-monitor desktops, without touching a selected pixel.
[[nodiscard]] inline bool BuildDimmingBounds(
    const RECT& virtualDesktop,
    const IntRect& recordingRegion,
    std::array<RECT, LayerCount>& bounds) noexcept {
    bounds.fill(RECT{});
    if (!IsNonEmpty(virtualDesktop) ||
        recordingRegion.right <= recordingRegion.left ||
        recordingRegion.bottom <= recordingRegion.top) {
        return false;
    }

    const RECT requestedClearArea{
        recordingRegion.left,
        recordingRegion.top,
        recordingRegion.right,
        recordingRegion.bottom};
    const RECT clearArea = Intersect(virtualDesktop, requestedClearArea);
    if (!IsNonEmpty(clearArea)) {
        bounds[0] = virtualDesktop;
        return true;
    }

    bounds = {{
        RECT{
            virtualDesktop.left,
            virtualDesktop.top,
            virtualDesktop.right,
            clearArea.top},
        RECT{
            virtualDesktop.left,
            clearArea.bottom,
            virtualDesktop.right,
            virtualDesktop.bottom},
        RECT{
            virtualDesktop.left,
            clearArea.top,
            clearArea.left,
            clearArea.bottom},
        RECT{
            clearArea.right,
            clearArea.top,
            virtualDesktop.right,
            clearArea.bottom},
    }};
    return true;
}

[[nodiscard]] inline int ScaleDip(const int value, const UINT dpi) noexcept {
    const UINT effectiveDpi = dpi == 0 ? USER_DEFAULT_SCREEN_DPI : dpi;
    return MulDiv(value, static_cast<int>(effectiveDpi), USER_DEFAULT_SCREEN_DPI);
}

[[nodiscard]] inline std::array<RECT, LayerCount> BuildEdgeBounds(
    const IntRect& recordingRegion,
    const UINT dpi) noexcept {
    constexpr int baseThickness = 2;
    const int width = recordingRegion.Width();
    const int height = recordingRegion.Height();
    const int maximumThickness = std::max(1, std::min(width, height) / 2);
    const int thickness = std::clamp(
        ScaleDip(baseThickness, dpi),
        1,
        maximumThickness);

    return {{
        RECT{
            recordingRegion.left,
            recordingRegion.top,
            recordingRegion.right,
            recordingRegion.top + thickness},
        RECT{
            recordingRegion.left,
            recordingRegion.bottom - thickness,
            recordingRegion.right,
            recordingRegion.bottom},
        RECT{
            recordingRegion.left,
            recordingRegion.top,
            recordingRegion.left + thickness,
            recordingRegion.bottom},
        RECT{
            recordingRegion.right - thickness,
            recordingRegion.top,
            recordingRegion.right,
            recordingRegion.bottom},
    }};
}

}  // namespace qrec::overlay::region_geometry
