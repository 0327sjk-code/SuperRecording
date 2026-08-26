#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "ui/Motion.h"

#include <windows.h>

#include <cstdint>
#include <functional>

namespace qrec {

enum class EditorSpeedInteractionPhase : std::uint8_t {
    Preview,
    Committed,
};

// Compact, keyboard-accessible playback/export speed control. Values are stored
// as tenths so the UI, preview and exporter share one exact representation.
class EditorSpeedControl final {
public:
    using ChangedCallback =
        std::function<void(int speedTenths, EditorSpeedInteractionPhase phase)>;

    static constexpr int MinimumSpeedTenths = 1;
    static constexpr int MaximumSpeedTenths = 30;
    static constexpr int DefaultSpeedTenths = 10;
    static constexpr int PreferredWidthDip = 184;
    static constexpr int MinimumWidthDip = 164;
    static constexpr int HeightDip = 30;

    EditorSpeedControl() = default;
    ~EditorSpeedControl();

    EditorSpeedControl(const EditorSpeedControl&) = delete;
    EditorSpeedControl& operator=(const EditorSpeedControl&) = delete;

    [[nodiscard]] bool Create(
        HWND parent,
        int controlId,
        HINSTANCE instance,
        ChangedCallback changed = {});
    void Destroy() noexcept;

    void SetEnabled(bool enabled) noexcept;
    void SetValueTenths(int speedTenths) noexcept;
    void SetChangedCallback(ChangedCallback changed);

    [[nodiscard]] int ValueTenths() const noexcept { return valueTenths_; }
    [[nodiscard]] HWND WindowHandle() const noexcept { return window_; }

private:
    static bool RegisterWindowClass(HINSTANCE instance) noexcept;
    static LRESULT CALLBACK WindowProc(
        HWND window,
        UINT message,
        WPARAM wParam,
        LPARAM lParam) noexcept;

    LRESULT HandleMessage(UINT message, WPARAM wParam, LPARAM lParam);
    void Paint();
    void DrawContent(HDC dc, const RECT& client) const;
    void RecreateFonts() noexcept;
    void DeleteFonts() noexcept;
    [[nodiscard]] bool EnsureBackBuffer(HDC targetDc, int width, int height) noexcept;
    void ReleaseBackBuffer() noexcept;

    [[nodiscard]] RECT TrackBounds() const noexcept;
    [[nodiscard]] int ValueFromClientX(int x) const noexcept;
    [[nodiscard]] int ThumbCenterX() const noexcept;
    [[nodiscard]] bool ApplyUserValue(int speedTenths);
    void CommitInteraction();
    void UpdateAccessibleText() noexcept;

    void SetHoverState(bool hovered) noexcept;
    void SetPressState(bool pressed) noexcept;
    void SetFocusState(bool focused) noexcept;
    void CancelInput(bool commit) noexcept;
    void UpdateMotionTimer() noexcept;
    [[nodiscard]] bool AdvanceMotion() noexcept;
    void JumpMotionToTargets() noexcept;

    HWND window_{};
    HWND parent_{};
    HINSTANCE instance_{};
    int controlId_{};
    HFONT labelFont_{};
    HFONT valueFont_{};
    HDC backBufferDc_{};
    HBITMAP backBufferBitmap_{};
    HGDIOBJ backBufferPreviousBitmap_{};
    int backBufferWidth_{};
    int backBufferHeight_{};
    ui::MotionState hoverMotion_{};
    ui::MotionState pressMotion_{};
    ui::MotionState focusMotion_{};
    ChangedCallback changedCallback_;
    int valueTenths_{DefaultSpeedTenths};
    UINT keyboardAdjustmentKey_{};
    bool enabled_{true};
    bool hovered_{};
    bool trackingMouse_{};
    bool dragging_{};
    bool interactionChanged_{};
    bool timerArmed_{};
    bool animationsEnabled_{true};
};

}  // namespace qrec
