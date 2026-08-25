#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>

#include <chrono>
#include <cstdint>
#include <optional>

#include "editor/TrimTimelineGeometry.h"
#include "ui/Motion.h"

namespace qrec {

inline constexpr UINT TimelineRangeChanged = 0x5101;
inline constexpr UINT TimelineSeekRequested = 0x5102;
inline constexpr UINT TimelineInteractionMessage = WM_APP + 0x241;

enum class TimelineInteractionPhase : std::uint8_t {
    Preview,
    Committed,
};

struct TimelineNotification final {
    NMHDR header{};
    TimelineInteractionPhase phase{TimelineInteractionPhase::Preview};
    std::chrono::milliseconds trimStart{};
    std::chrono::milliseconds trimEnd{};
    std::chrono::milliseconds seekPosition{};
};

class TrimTimeline final {
public:
    TrimTimeline() = default;
    ~TrimTimeline();

    TrimTimeline(const TrimTimeline&) = delete;
    TrimTimeline& operator=(const TrimTimeline&) = delete;

    [[nodiscard]] bool Create(HWND parent, int controlId, HINSTANCE instance);
    void Destroy() noexcept;
    void SetRange(
        std::chrono::milliseconds duration,
        std::chrono::milliseconds trimStart,
        std::chrono::milliseconds trimEnd);
    void SetPlayhead(std::chrono::milliseconds position);
    void SetEnabled(bool enabled);
    [[nodiscard]] bool ConsumePendingNotification(
        TimelineNotification* notification) noexcept;

    [[nodiscard]] HWND WindowHandle() const noexcept { return window_; }
    [[nodiscard]] std::chrono::milliseconds TrimStart() const noexcept;
    [[nodiscard]] std::chrono::milliseconds TrimEnd() const noexcept;

private:
    using ActivePart = timeline_detail::HitTarget;

    static LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam);
    LRESULT HandleMessage(UINT message, WPARAM wParam, LPARAM lParam);
    void Paint();
    void UpdateFromPointer(int x, bool notify);
    void QueueNotification(
        UINT code,
        TimelineInteractionPhase phase,
        std::chrono::milliseconds seekPosition = {});
    void PostPendingNotificationMessage() noexcept;
    void QueueNotificationForPart(ActivePart part, TimelineInteractionPhase phase);
    void CommitKeyboardAdjustment();
    void SetHoverPart(ActivePart part) noexcept;
    void SetPressedPart(ActivePart part) noexcept;
    void SetFocusMotion(bool focused) noexcept;
    void UpdateMotionTimer() noexcept;
    [[nodiscard]] bool AdvanceMotion() noexcept;
    void ResetMotion() noexcept;
    void EnsureLayoutCache() noexcept;
    void UpdateLayoutCache(int width, int height, UINT dpi) noexcept;
    void ReleaseBackBuffer() noexcept;
    [[nodiscard]] bool EnsureBackBuffer(HDC targetDc) noexcept;
    [[nodiscard]] int TimeToX(std::chrono::milliseconds value) noexcept;
    [[nodiscard]] std::chrono::milliseconds XToTime(int x) noexcept;
    [[nodiscard]] ActivePart HitTestPart(POINT point) noexcept;
    [[nodiscard]] int PartCenterX(ActivePart part) noexcept;
    static bool RegisterWindowClass(HINSTANCE instance);

    enum class BoundarySnap : std::uint8_t {
        None,
        Start,
        End,
    };

    HWND window_{};
    HFONT font_{};
    int controlId_{};
    std::chrono::milliseconds duration_{1};
    std::chrono::milliseconds trimStart_{};
    std::chrono::milliseconds trimEnd_{1};
    std::chrono::milliseconds playhead_{};
    ActivePart activePart_{ActivePart::None};
    ActivePart dragPart_{ActivePart::None};
    ActivePart hoverPart_{ActivePart::None};
    BoundarySnap boundarySnap_{BoundarySnap::None};
    int dragAnchorOffsetX_{};
    ui::MotionState startHoverMotion_{};
    ui::MotionState endHoverMotion_{};
    ui::MotionState playheadHoverMotion_{};
    ui::MotionState startPressMotion_{};
    ui::MotionState endPressMotion_{};
    ui::MotionState playheadPressMotion_{};
    ui::MotionState focusMotion_{};
    std::optional<TimelineNotification> pendingNotification_;
    std::optional<TimelineNotification> deferredNotification_;
    HDC backBufferDc_{};
    HBITMAP backBufferBitmap_{};
    HGDIOBJ backBufferPreviousBitmap_{};
    timeline_detail::TrackGeometry cachedTrack_{};
    UINT cachedDpi_{USER_DEFAULT_SCREEN_DPI};
    int cachedClientWidth_{1};
    int cachedClientHeight_{1};
    bool dragging_{};
    bool trackingMouse_{};
    bool keyboardAdjusting_{};
    bool notificationMessagePosted_{};
    bool layoutCacheValid_{};
    bool motionTimerArmed_{};
    bool animationsEnabled_{true};
    bool enabled_{true};
};

}  // namespace qrec
