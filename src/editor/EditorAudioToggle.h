#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "ui/Motion.h"

#include <windows.h>

#include <functional>
#include <string>
#include <string_view>

namespace qrec {

// A single child HWND with native checkbox accessibility semantics and a fully
// custom-drawn editor appearance. The callback is emitted only for user or
// accessibility activation; SetChecked() is intentionally silent.
class EditorAudioToggle final {
public:
    using ChangedCallback = std::function<void(bool checked)>;

    static constexpr int PreferredWidthDip = 104;
    static constexpr int MinimumWidthDip = 96;
    static constexpr int HeightDip = 40;

    EditorAudioToggle() = default;
    ~EditorAudioToggle();

    EditorAudioToggle(const EditorAudioToggle&) = delete;
    EditorAudioToggle& operator=(const EditorAudioToggle&) = delete;

    [[nodiscard]] bool Create(
        HWND parent,
        int controlId,
        HINSTANCE instance,
        ChangedCallback changed = {});
    void Destroy() noexcept;

    void SetEnabled(bool enabled) noexcept;
    void SetChecked(bool checked) noexcept;
    void SetStatusText(std::wstring_view statusText);
    void SetChangedCallback(ChangedCallback changed);

    [[nodiscard]] bool Checked() const noexcept { return checked_; }
    [[nodiscard]] HWND WindowHandle() const noexcept { return window_; }

private:
    static LRESULT CALLBACK SubclassProc(
        HWND control,
        UINT message,
        WPARAM wParam,
        LPARAM lParam,
        UINT_PTR subclassId,
        DWORD_PTR referenceData) noexcept;

    LRESULT HandleMessage(
        HWND control,
        UINT message,
        WPARAM wParam,
        LPARAM lParam,
        UINT_PTR subclassId);
    void ActivateFromUser();
    void ApplyCheckedState(bool checked, bool animate) noexcept;
    void UpdateAccessibleText();
    void RecreateFonts() noexcept;
    void DeleteFonts() noexcept;
    void PaintWindow();
    void DrawContent(HDC dc, const RECT& client);
    [[nodiscard]] bool EnsureBackBuffer(HDC targetDc, int width, int height) noexcept;
    void ReleaseBackBuffer() noexcept;
    void SetHoverState(bool hovered) noexcept;
    void SetPressState(bool pressed) noexcept;
    void SetFocusState(bool focused) noexcept;
    void CancelInput() noexcept;
    void UpdateMotionTimer() noexcept;
    [[nodiscard]] bool AdvanceMotion() noexcept;
    [[nodiscard]] bool HasActiveMotion() const noexcept;
    void JumpMotionToTargets() noexcept;

    HWND window_{};
    HWND parent_{};
    HINSTANCE instance_{};
    int controlId_{};
    HFONT labelFont_{};
    HDC backBufferDc_{};
    HBITMAP backBufferBitmap_{};
    HGDIOBJ backBufferPreviousBitmap_{};
    int backBufferWidth_{};
    int backBufferHeight_{};
    ui::MotionState hoverMotion_{};
    ui::MotionState pressMotion_{};
    ui::MotionState focusMotion_{};
    ui::MotionState checkedMotion_{};
    std::wstring statusText_;
    ChangedCallback changedCallback_;
    UINT keyboardActivationKey_{};
    bool enabled_{true};
    bool checked_{};
    bool hovered_{};
    bool trackingMouse_{};
    bool mousePressed_{};
    bool timerArmed_{};
    bool animationsEnabled_{true};
};

}  // namespace qrec
