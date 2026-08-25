#pragma once

#include <windows.h>

#include <chrono>

namespace qrec::overlay::view {

struct VisualState final {
    UINT dpi{USER_DEFAULT_SCREEN_DPI};
    bool paused{};
    bool stopping{};
    float dragHoverAmount{};
    float dragPressAmount{};
    float pauseHoverAmount{};
    float pausePressAmount{};
    float pauseStateAmount{};
    float stopHoverAmount{};
    float stopPressAmount{};
    float stopStateAmount{};
    HFONT bodyFont{};
    HFONT actionFont{};
    HFONT timerFont{};
    std::chrono::milliseconds elapsed{};
};

[[nodiscard]] SIZE DesiredSize(UINT dpi) noexcept;
[[nodiscard]] RECT StatusBounds(UINT dpi) noexcept;
[[nodiscard]] RECT DragBounds(UINT dpi) noexcept;
[[nodiscard]] RECT PauseBounds(UINT dpi) noexcept;
[[nodiscard]] RECT StopBounds(UINT dpi) noexcept;
void ApplyRoundedWindowRegion(HWND window, UINT dpi);
void Paint(HWND window, const VisualState& state);

}  // namespace qrec::overlay::view
