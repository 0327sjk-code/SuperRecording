#include "common/HotkeyBinding.h"

#include <algorithm>
#include <cwctype>
#include <limits>
#include <vector>

namespace qrec {
namespace {

constexpr UINT kSupportedModifierMask = MOD_CONTROL | MOD_ALT | MOD_SHIFT | MOD_WIN;

[[nodiscard]] bool IsFunctionKey(const UINT virtualKey) noexcept {
    return virtualKey >= VK_F1 && virtualKey <= VK_F24;
}

[[nodiscard]] bool IsLetterOrDigit(const UINT virtualKey) noexcept {
    return (virtualKey >= static_cast<UINT>(L'A') &&
            virtualKey <= static_cast<UINT>(L'Z')) ||
        (virtualKey >= static_cast<UINT>(L'0') &&
         virtualKey <= static_cast<UINT>(L'9'));
}

[[nodiscard]] std::wstring TrimCopy(const std::wstring_view value) {
    const auto first = std::find_if_not(
        value.begin(),
        value.end(),
        [](const wchar_t character) { return std::iswspace(character) != 0; });
    const auto last = std::find_if_not(
        value.rbegin(),
        value.rend(),
        [](const wchar_t character) { return std::iswspace(character) != 0; }).base();
    if (first >= last) {
        return {};
    }
    return std::wstring(first, last);
}

[[nodiscard]] std::wstring UpperAsciiCopy(const std::wstring_view value) {
    std::wstring result = TrimCopy(value);
    std::ranges::transform(result, result.begin(), [](const wchar_t character) {
        if (character >= L'a' && character <= L'z') {
            return static_cast<wchar_t>(character - L'a' + L'A');
        }
        return character;
    });
    return result;
}

void AppendDisplayToken(std::wstring& destination, const std::wstring_view token) {
    if (!destination.empty()) {
        destination += L" + ";
    }
    destination.append(token);
}

void AppendSerializedToken(std::wstring& destination, const std::wstring_view token) {
    if (!destination.empty()) {
        destination.push_back(L'+');
    }
    destination.append(token);
}

[[nodiscard]] std::wstring VirtualKeyName(const UINT virtualKey) {
    if (virtualKey >= VK_F1 && virtualKey <= VK_F24) {
        return L"F" + std::to_wstring(virtualKey - VK_F1 + 1U);
    }
    if (IsLetterOrDigit(virtualKey)) {
        return std::wstring(1, static_cast<wchar_t>(virtualKey));
    }
    return {};
}

[[nodiscard]] std::optional<UINT> ParseVirtualKey(const std::wstring_view token) {
    if (token.size() == 1) {
        const wchar_t character = token.front();
        if ((character >= L'A' && character <= L'Z') ||
            (character >= L'0' && character <= L'9')) {
            return static_cast<UINT>(character);
        }
        return std::nullopt;
    }
    if (token.size() < 2 || token.front() != L'F') {
        return std::nullopt;
    }

    unsigned int number = 0;
    for (std::size_t index = 1; index < token.size(); ++index) {
        const wchar_t character = token[index];
        if (character < L'0' || character > L'9') {
            return std::nullopt;
        }
        constexpr unsigned int kDecimalBase = 10;
        const unsigned int digit = static_cast<unsigned int>(character - L'0');
        if (number > (std::numeric_limits<unsigned int>::max() - digit) / kDecimalBase) {
            return std::nullopt;
        }
        number = number * kDecimalBase + digit;
    }
    if (number < 1U || number > 24U) {
        return std::nullopt;
    }
    return VK_F1 + number - 1U;
}

}  // namespace

bool IsValidHotkeyBinding(const HotkeyBinding& binding) noexcept {
    if ((binding.modifiers & ~kSupportedModifierMask) != 0U ||
        binding.virtualKey == VK_F12) {
        return false;
    }
    if (IsFunctionKey(binding.virtualKey)) {
        return true;
    }
    return binding.modifiers != 0U && IsLetterOrDigit(binding.virtualKey);
}

std::wstring HotkeyValidationError(const HotkeyBinding& binding) {
    if ((binding.modifiers & ~kSupportedModifierMask) != 0U) {
        return L"快捷键包含不支持的修饰键。";
    }
    if (binding.virtualKey == VK_F12) {
        return L"F12 由 Windows 调试器保留，请选择其他按键。";
    }
    if (binding.virtualKey == 0U) {
        return L"请按下一个字母、数字或功能键。";
    }
    if (!IsFunctionKey(binding.virtualKey) && !IsLetterOrDigit(binding.virtualKey)) {
        return L"仅支持 A–Z、0–9 与 F1–F24。";
    }
    if (binding.modifiers == 0U && IsLetterOrDigit(binding.virtualKey)) {
        return L"字母和数字必须搭配 Ctrl、Alt、Shift 或 Win。";
    }
    return {};
}

std::wstring FormatHotkeyBinding(const HotkeyBinding& binding) {
    if (!IsValidHotkeyBinding(binding)) {
        return {};
    }

    std::wstring result;
    if ((binding.modifiers & MOD_CONTROL) != 0U) {
        AppendDisplayToken(result, L"Ctrl");
    }
    if ((binding.modifiers & MOD_ALT) != 0U) {
        AppendDisplayToken(result, L"Alt");
    }
    if ((binding.modifiers & MOD_SHIFT) != 0U) {
        AppendDisplayToken(result, L"Shift");
    }
    if ((binding.modifiers & MOD_WIN) != 0U) {
        AppendDisplayToken(result, L"Win");
    }
    AppendDisplayToken(result, VirtualKeyName(binding.virtualKey));
    return result;
}

std::wstring SerializeHotkeyBinding(const HotkeyBinding& binding) {
    if (!IsValidHotkeyBinding(binding)) {
        return {};
    }

    std::wstring result;
    if ((binding.modifiers & MOD_CONTROL) != 0U) {
        AppendSerializedToken(result, L"CTRL");
    }
    if ((binding.modifiers & MOD_ALT) != 0U) {
        AppendSerializedToken(result, L"ALT");
    }
    if ((binding.modifiers & MOD_SHIFT) != 0U) {
        AppendSerializedToken(result, L"SHIFT");
    }
    if ((binding.modifiers & MOD_WIN) != 0U) {
        AppendSerializedToken(result, L"WIN");
    }
    AppendSerializedToken(result, VirtualKeyName(binding.virtualKey));
    return result;
}

std::optional<HotkeyBinding> ParseHotkeyBinding(const std::wstring_view value) {
    const std::wstring normalized = UpperAsciiCopy(value);
    if (normalized.empty()) {
        return std::nullopt;
    }

    UINT modifiers = 0U;
    std::optional<UINT> virtualKey;
    std::size_t start = 0;
    while (start <= normalized.size()) {
        const std::size_t separator = normalized.find(L'+', start);
        const std::size_t end = separator == std::wstring::npos
            ? normalized.size()
            : separator;
        const std::wstring token = TrimCopy(
            std::wstring_view(normalized).substr(start, end - start));
        if (token.empty()) {
            return std::nullopt;
        }

        UINT modifier = 0U;
        if (token == L"CTRL" || token == L"CONTROL") {
            modifier = MOD_CONTROL;
        } else if (token == L"ALT") {
            modifier = MOD_ALT;
        } else if (token == L"SHIFT") {
            modifier = MOD_SHIFT;
        } else if (token == L"WIN" || token == L"WINDOWS") {
            modifier = MOD_WIN;
        }

        if (modifier != 0U) {
            if ((modifiers & modifier) != 0U || virtualKey.has_value()) {
                return std::nullopt;
            }
            modifiers |= modifier;
        } else {
            if (virtualKey.has_value()) {
                return std::nullopt;
            }
            virtualKey = ParseVirtualKey(token);
            if (!virtualKey.has_value()) {
                return std::nullopt;
            }
        }

        if (separator == std::wstring::npos) {
            break;
        }
        start = separator + 1U;
    }

    if (!virtualKey.has_value()) {
        return std::nullopt;
    }
    const HotkeyBinding binding{modifiers, *virtualKey};
    if (!IsValidHotkeyBinding(binding)) {
        return std::nullopt;
    }
    return binding;
}

UINT RegistrationModifiers(const HotkeyBinding& binding) noexcept {
    return (binding.modifiers & kSupportedModifierMask) | MOD_NOREPEAT;
}

}  // namespace qrec
