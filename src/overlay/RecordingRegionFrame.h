#pragma once

#include <windows.h>

#include <array>
#include <cstddef>

#include "../common/Types.h"

namespace qrec::overlay {

// Four capture-excluded dimmer windows cover the virtual desktop outside the
// active recording region. Four additional capture-excluded edge windows mark
// the region while keeping its center physically window-free and interactive.
class RecordingRegionFrame final {
public:
    RecordingRegionFrame() = default;
    ~RecordingRegionFrame();

    RecordingRegionFrame(const RecordingRegionFrame&) = delete;
    RecordingRegionFrame& operator=(const RecordingRegionFrame&) = delete;

    [[nodiscard]] bool Create(HWND owner, const IntRect& recordingRegion);
    void Destroy() noexcept;

    [[nodiscard]] bool SetRegion(const IntRect& recordingRegion);

    [[nodiscard]] bool IsCreated() const noexcept;
    [[nodiscard]] bool CaptureExcluded() const noexcept { return captureExcluded_; }
    [[nodiscard]] IntRect Region() const noexcept { return recordingRegion_; }

private:
    static constexpr std::size_t LayerCount = 4;

    static LRESULT CALLBACK WindowProcedure(
        HWND window,
        UINT message,
        WPARAM wParam,
        LPARAM lParam);

    [[nodiscard]] bool RegisterWindowClass();
    [[nodiscard]] bool CreateLayerWindows(HWND owner);
    [[nodiscard]] bool ConfigureCaptureExclusion(HWND window);
    [[nodiscard]] bool ApplyLayout();
    [[nodiscard]] bool ComposeDimmer(HWND window, const RECT& bounds) const;
    [[nodiscard]] bool CommitLayerPositions(
        const std::array<RECT, LayerCount>& dimmerBounds,
        const std::array<RECT, LayerCount>& edgeBounds);
    [[nodiscard]] bool IsDimmerWindow(HWND window) const noexcept;
    void Paint(HWND window) const;
    void RefreshLayoutForDisplayChange();

    HINSTANCE instance_{};
    HWND owner_{};
    std::array<HWND, LayerCount> windows_{};
    std::array<HWND, LayerCount> dimmerWindows_{};
    HBRUSH brush_{};
    IntRect recordingRegion_{};
    bool captureExcluded_{};
    bool applyingLayout_{};
    bool layoutRefreshPending_{};
};

}  // namespace qrec::overlay
