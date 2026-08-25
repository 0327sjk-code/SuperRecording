#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>

#include <optional>
#include <string>
#include <string_view>

namespace qrec {

// Stores only user-selectable modifiers. MOD_NOREPEAT is applied at the
// RegisterHotKey boundary by RegistrationModifiers().
struct HotkeyBinding final {
    UINT modifiers{};
    UINT virtualKey{VK_F3};

    [[nodiscard]] bool operator==(const HotkeyBinding&) const noexcept = default;
};

[[nodiscard]] constexpr HotkeyBinding DefaultHotkeyBinding() noexcept {
    return HotkeyBinding{0U, VK_F3};
}

[[nodiscard]] bool IsValidHotkeyBinding(const HotkeyBinding& binding) noexcept;
[[nodiscard]] std::wstring HotkeyValidationError(const HotkeyBinding& binding);

// Human-readable label used by the tray menu and settings UI.
// Example: "Ctrl + Shift + R".
[[nodiscard]] std::wstring FormatHotkeyBinding(const HotkeyBinding& binding);

// Stable single-field settings representation. Example: "CTRL+SHIFT+R".
[[nodiscard]] std::wstring SerializeHotkeyBinding(const HotkeyBinding& binding);
[[nodiscard]] std::optional<HotkeyBinding> ParseHotkeyBinding(std::wstring_view value);

// Returns the Win32 RegisterHotKey modifier mask, including MOD_NOREPEAT.
[[nodiscard]] UINT RegistrationModifiers(const HotkeyBinding& binding) noexcept;

}  // namespace qrec
