#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>

#include <memory>

namespace qrec {

// Internal owner-draw button group used by HotkeyEditorChrome.
class HotkeyEditorButtons final {
public:
    HotkeyEditorButtons();
    ~HotkeyEditorButtons();

    HotkeyEditorButtons(const HotkeyEditorButtons&) = delete;
    HotkeyEditorButtons& operator=(const HotkeyEditorButtons&) = delete;

    [[nodiscard]] bool Initialize(
        HINSTANCE instance,
        HWND parent,
        HFONT regularFont,
        HFONT strongFont,
        int defaultButtonId,
        int cancelButtonId,
        int saveButtonId);
    void Shutdown() noexcept;
    void SetFonts(HFONT regularFont, HFONT strongFont) const noexcept;
    void Layout(
        const RECT& defaultBounds,
        const RECT& cancelBounds,
        const RECT& saveBounds) const noexcept;

    [[nodiscard]] bool Draw(const DRAWITEMSTRUCT* item) const;
    [[nodiscard]] HWND DefaultButton() const noexcept;
    [[nodiscard]] HWND CancelButton() const noexcept;
    [[nodiscard]] HWND SaveButton() const noexcept;
    void SetSaveEnabled(bool enabled) const noexcept;
    void FocusFirst() const noexcept;
    void FocusSave() const noexcept;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace qrec
