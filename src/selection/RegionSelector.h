#pragma once

#include <windows.h>

#include <optional>
#include <vector>

#include "../common/Types.h"
#include "RegionSelectorView.h"

namespace qrec::selection {

// Main-thread modal region picker. The returned rectangle uses virtual-desktop
// screen coordinates and is therefore ready for desktop capture APIs.
class RegionSelector final {
public:
    RegionSelector() = default;
    ~RegionSelector();

    RegionSelector(const RegionSelector&) = delete;
    RegionSelector& operator=(const RegionSelector&) = delete;

    [[nodiscard]] std::optional<IntRect> Select(
        HWND owner = nullptr,
        bool adjustSelectionBeforeRecording = false);

private:
    enum class Phase : unsigned char {
        Selecting,
        Adjusting,
    };

    enum class AdjustmentHit : unsigned char {
        None,
        Move,
        Left,
        Right,
        Top,
        Bottom,
        TopLeft,
        TopRight,
        BottomLeft,
        BottomRight,
    };

    struct MonitorArea final {
        RECT bounds{};
        UINT dpi{96};
    };

    static LRESULT CALLBACK WindowProcedure(HWND window, UINT message, WPARAM wParam, LPARAM lParam);
    static BOOL CALLBACK EnumerateMonitor(HMONITOR monitor, HDC dc, LPRECT bounds, LPARAM context);

    [[nodiscard]] bool CreateSelectionWindow(HWND owner);
    void Complete(const IntRect& region);
    void Cancel();
    void RefreshMonitorAreas();
    void UpdatePointer(POINT clientPoint);
    void UpdateSelection(POINT clientPoint);
    void UpdateAdjustedSelection(POINT clientPoint);
    void FinishInitialSelection(const IntRect& region);
    void EnterAdjustment();
    void BeginAdjustment(POINT clientPoint);
    void EndPointerInteraction(POINT clientPoint);
    void InvalidateFrame();
    void Paint();

    [[nodiscard]] POINT ClampToClient(POINT point) const noexcept;
    [[nodiscard]] RECT NormalizeSelection(POINT first, POINT second) const noexcept;
    [[nodiscard]] RECT SnapSelection(RECT selection, POINT pointer) const noexcept;
    [[nodiscard]] RECT SnapMovedSelection(RECT selection, POINT pointer) const noexcept;
    [[nodiscard]] RECT SnapAdjustedSelection(RECT selection, POINT pointer) const noexcept;
    [[nodiscard]] RECT MakeInitialSelectionEven(RECT selection, POINT pointer) const noexcept;
    [[nodiscard]] RECT MakeAdjustedSelectionEven(RECT selection) const noexcept;
    [[nodiscard]] bool IsSelectionValid() const noexcept;
    [[nodiscard]] bool IsContainedInSingleMonitor(const RECT& clientRect) const noexcept;
    [[nodiscard]] AdjustmentHit HitTestAdjustment(POINT clientPoint) const noexcept;
    [[nodiscard]] view::ControlButton HitTestControl(POINT clientPoint) const noexcept;
    [[nodiscard]] HCURSOR CursorForPoint(POINT clientPoint) const noexcept;
    [[nodiscard]] RECT MonitorBoundsAt(POINT screenPoint) const noexcept;
    [[nodiscard]] RECT ActiveMonitorClientBounds() const noexcept;
    [[nodiscard]] UINT DpiAt(POINT screenPoint) const noexcept;
    [[nodiscard]] IntRect ToScreenRect(const RECT& clientRect) const noexcept;
    [[nodiscard]] RECT ToClientRect(const RECT& screenRect) const noexcept;

    HWND window_{};
    HWND owner_{};
    RECT virtualDesktop_{};
    std::vector<MonitorArea> monitors_;
    POINT dragOrigin_{};
    POINT pointer_{};
    RECT selection_{};
    RECT adjustmentOriginSelection_{};
    POINT adjustmentOrigin_{};
    Phase phase_{Phase::Selecting};
    AdjustmentHit activeAdjustment_{AdjustmentHit::None};
    view::ControlButton hoveredControl_{view::ControlButton::None};
    view::ControlButton pressedControl_{view::ControlButton::None};
    bool mouseDown_{};
    bool hasDragged_{};
    bool adjustSelectionBeforeRecording_{};
    bool completed_{};
    std::optional<IntRect> result_;
    view::FrameBuffer frameBuffer_;
};

}  // namespace qrec::selection
