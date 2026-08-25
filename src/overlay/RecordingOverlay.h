#pragma once

#include <windows.h>

#include <chrono>
#include <functional>

#include "../common/Types.h"
#include "../ui/Motion.h"

namespace qrec::overlay {

struct RecordingOverlayCallbacks final {
    std::function<void(bool paused)> pauseChanged;
    std::function<void()> stopRequested;
};

// Main-thread, non-activating recording controller. The window is marked with
// WDA_EXCLUDEFROMCAPTURE so it stays visible locally without entering captures.
class RecordingOverlay final {
public:
    RecordingOverlay() = default;
    ~RecordingOverlay();

    RecordingOverlay(const RecordingOverlay&) = delete;
    RecordingOverlay& operator=(const RecordingOverlay&) = delete;

    [[nodiscard]] bool Create(
        HWND owner,
        const IntRect& recordingRegion,
        RecordingOverlayCallbacks callbacks,
        std::chrono::milliseconds initialElapsed = std::chrono::milliseconds::zero());
    void Destroy();

    void SetPaused(bool paused);
    void SetElapsed(std::chrono::milliseconds elapsed);
    void SetRegion(const IntRect& recordingRegion);
    void SetStopping(bool stopping);

    [[nodiscard]] HWND WindowHandle() const noexcept { return window_; }
    [[nodiscard]] bool IsCreated() const noexcept { return window_ != nullptr; }
    [[nodiscard]] bool IsPaused() const noexcept { return paused_; }
    [[nodiscard]] bool IsCaptureExcluded() const noexcept { return captureExcluded_; }
    [[nodiscard]] std::chrono::milliseconds Elapsed() const noexcept;

private:
    static constexpr UINT_PTR RefreshTimerId = 1;
    static constexpr UINT RefreshIntervalMilliseconds = 100;
    static constexpr UINT_PTR MotionTimerId = 2;
    static constexpr UINT MotionFrameMilliseconds = 16;

    enum class HitTarget {
        None,
        DragHandle,
        Pause,
        Stop,
    };

    static LRESULT CALLBACK WindowProcedure(HWND window, UINT message, WPARAM wParam, LPARAM lParam);

    [[nodiscard]] bool CreateWindowForRegion(HWND owner, const IntRect& recordingRegion);
    void Reposition(bool preserveManualPosition);
    void UpdateDpi(UINT dpi);
    void RecreateFonts();
    void UpdateRoundedRegion();
    void Paint();
    void BeginDrag(POINT clientPoint);
    void ContinueDrag(POINT screenPoint);
    void EndDrag();
    void Activate(HitTarget target);
    void TrackMouseLeave();
    void SetHoveredTarget(HitTarget target) noexcept;
    void SetPressedTarget(HitTarget target) noexcept;
    void UpdateMotionTimer() noexcept;
    [[nodiscard]] bool AdvanceMotion() noexcept;
    void ResetMotion() noexcept;
    void InvalidateTarget(HitTarget target) const noexcept;
    void InvalidateMotionRegions() const noexcept;
    void InvalidateStatusRegion() const noexcept;

    [[nodiscard]] HitTarget HitTest(POINT clientPoint) const noexcept;
    [[nodiscard]] RECT PauseBounds() const noexcept;
    [[nodiscard]] RECT StopBounds() const noexcept;
    [[nodiscard]] RECT DragBounds() const noexcept;
    [[nodiscard]] SIZE DesiredSize() const noexcept;

    HWND window_{};
    HWND owner_{};
    IntRect recordingRegion_{};
    RecordingOverlayCallbacks callbacks_;
    UINT dpi_{96};
    bool paused_{};
    bool stopping_{};
    bool captureExcluded_{};
    bool trackingMouse_{};
    bool dragging_{};
    bool manuallyPositioned_{};
    HitTarget hovered_{HitTarget::None};
    HitTarget pressed_{HitTarget::None};
    ui::MotionState dragHoverMotion_{};
    ui::MotionState dragPressMotion_{};
    ui::MotionState pauseHoverMotion_{};
    ui::MotionState pausePressMotion_{};
    ui::MotionState pauseStateMotion_{};
    ui::MotionState stopHoverMotion_{};
    ui::MotionState stopPressMotion_{};
    ui::MotionState stopStateMotion_{};
    POINT dragStartScreen_{};
    RECT dragStartWindow_{};
    HFONT bodyFont_{};
    HFONT actionFont_{};
    HFONT timerFont_{};
    std::chrono::milliseconds accumulatedElapsed_{};
    std::chrono::steady_clock::time_point segmentStarted_{};
};

}  // namespace qrec::overlay
