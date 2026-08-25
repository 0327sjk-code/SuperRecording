#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>

#include <memory>
#include <string_view>

namespace qrec {

enum class HotkeyEditorTone : unsigned char {
    Neutral,
    Success,
    Error,
};

struct HotkeyEditorChromeView final {
    bool capturing{};
    HotkeyEditorTone tone{HotkeyEditorTone::Neutral};
    std::wstring_view displayText;
    std::wstring_view inlineMessage;
};

// Owns the native visual layer of the hotkey editor. Keyboard capture and
// persistence deliberately remain in HotkeyEditorDialog.
class HotkeyEditorChrome final {
public:
    static constexpr int ClientWidth = 540;
    static constexpr int ClientHeight = 360;
    static constexpr int DefaultButtonId = 1101;
    static constexpr int CancelButtonId = 1102;
    static constexpr int SaveButtonId = 1103;

    HotkeyEditorChrome();
    ~HotkeyEditorChrome();

    HotkeyEditorChrome(const HotkeyEditorChrome&) = delete;
    HotkeyEditorChrome& operator=(const HotkeyEditorChrome&) = delete;

    [[nodiscard]] bool Initialize(HINSTANCE instance, HWND window);
    void Shutdown() noexcept;

    [[nodiscard]] bool RecreateFonts();
    void Layout();
    void RefreshWindowChrome();
    void UpdateWindowRegion() const noexcept;

    void Paint(const HotkeyEditorChromeView& view) const;
    [[nodiscard]] bool DrawButton(const DRAWITEMSTRUCT* item) const;

    void UpdateCaptureHover(POINT point);
    void ClearCaptureHover();
    [[nodiscard]] bool HandleTimer(UINT_PTR timerId);
    [[nodiscard]] bool HitTestCaptureCard(POINT point) const noexcept;
    [[nodiscard]] bool SetCaptureCardCursor(LPARAM hitTestData) const noexcept;

    [[nodiscard]] HWND DefaultButton() const noexcept;
    [[nodiscard]] HWND CancelButton() const noexcept;
    [[nodiscard]] HWND SaveButton() const noexcept;
    void SetSaveEnabled(bool enabled) const noexcept;
    void FocusFirstButton() const noexcept;
    void FocusSaveButton() const noexcept;

    void InvalidateAll() const noexcept;
    void InvalidateCaptureCard() const noexcept;
    void InvalidateInlineMessage() const noexcept;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace qrec
