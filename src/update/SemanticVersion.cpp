#include "update/SemanticVersion.h"

#include <array>
#include <charconv>
#include <limits>
#include <type_traits>

namespace qrec::update {
namespace {

template <typename Character>
std::optional<SemanticVersion> ParseVersion(
    const std::basic_string_view<Character> text) noexcept {
    if (text.empty()) {
        return std::nullopt;
    }

    std::array<std::uint16_t, 4> components{};
    std::size_t componentIndex = 0;
    std::size_t componentStart = 0;

    for (std::size_t index = 0; index <= text.size(); ++index) {
        const bool atEnd = index == text.size();
        if (!atEnd && text[index] != static_cast<Character>('.')) {
            continue;
        }
        if (componentIndex >= components.size() || index == componentStart) {
            return std::nullopt;
        }

        const std::size_t componentLength = index - componentStart;
        if (componentLength > 1 &&
            text[componentStart] == static_cast<Character>('0')) {
            return std::nullopt;
        }

        std::uint32_t value = 0;
        for (std::size_t digitIndex = componentStart;
             digitIndex < index;
             ++digitIndex) {
            const Character digit = text[digitIndex];
            if (digit < static_cast<Character>('0') ||
                digit > static_cast<Character>('9')) {
                return std::nullopt;
            }
            value = value * 10U + static_cast<std::uint32_t>(
                digit - static_cast<Character>('0'));
            if (value > std::numeric_limits<std::uint16_t>::max()) {
                return std::nullopt;
            }
        }
        components[componentIndex] = static_cast<std::uint16_t>(value);
        ++componentIndex;
        componentStart = index + 1;
    }

    if (componentIndex != 3 && componentIndex != 4) {
        return std::nullopt;
    }
    return SemanticVersion(
        components[0], components[1], components[2], components[3]);
}

template <typename String, typename Character>
String FormatVersion(
    const std::uint16_t major,
    const std::uint16_t minor,
    const std::uint16_t patch,
    const std::uint16_t build) {
    String result;
    result.reserve(24);

    const auto appendNumber = [&result](const std::uint16_t value) {
        if constexpr (std::is_same_v<Character, char>) {
            std::array<char, 6> buffer{};
            const auto conversion = std::to_chars(
                buffer.data(), buffer.data() + buffer.size(), value);
            result.append(buffer.data(), conversion.ptr);
        } else {
            const std::string narrow = std::to_string(value);
            for (const char character : narrow) {
                result.push_back(static_cast<Character>(character));
            }
        }
    };

    appendNumber(major);
    result.push_back(static_cast<Character>('.'));
    appendNumber(minor);
    result.push_back(static_cast<Character>('.'));
    appendNumber(patch);
    if (build != 0) {
        result.push_back(static_cast<Character>('.'));
        appendNumber(build);
    }
    return result;
}

}  // namespace

std::optional<SemanticVersion> SemanticVersion::Parse(
    const std::string_view text) noexcept {
    return ParseVersion(text);
}

std::optional<SemanticVersion> SemanticVersion::Parse(
    const std::wstring_view text) noexcept {
    return ParseVersion(text);
}

std::string SemanticVersion::ToString() const {
    return FormatVersion<std::string, char>(major_, minor_, patch_, build_);
}

std::wstring SemanticVersion::ToWString() const {
    return FormatVersion<std::wstring, wchar_t>(
        major_, minor_, patch_, build_);
}

}  // namespace qrec::update
