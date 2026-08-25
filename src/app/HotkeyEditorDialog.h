#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "common/HotkeyBinding.h"

#include <windows.h>

#include <functional>
#include <memory>
#include <string>

namespace qrec {

// Return an empty string after the binding has been registered and persisted.
// Return a user-facing error to keep the dialog open and show it inline.
using HotkeySaveCallback = std::function<std::wstring(const HotkeyBinding&)>;

class HotkeyEditorDialog final {
public:
    explicit HotkeyEditorDialog(HINSTANCE instance, HWND owner = nullptr);
    ~HotkeyEditorDialog();

    HotkeyEditorDialog(const HotkeyEditorDialog&) = delete;
    HotkeyEditorDialog& operator=(const HotkeyEditorDialog&) = delete;

    [[nodiscard]] bool Open(
        const HotkeyBinding& currentBinding,
        HotkeySaveCallback saveCallback,
        std::wstring* errorMessage = nullptr);
    void Close() noexcept;

    [[nodiscard]] HWND WindowHandle() const noexcept;
    [[nodiscard]] bool IsOpen() const noexcept;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace qrec
