#pragma once

#include <compare>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace qrec::update {

// A numeric Windows product version. Parsing accepts exactly three or four
// dot-separated components, each in the inclusive range 0..65535.
class SemanticVersion final {
public:
    constexpr SemanticVersion() noexcept = default;
    constexpr SemanticVersion(
        const std::uint16_t major,
        const std::uint16_t minor,
        const std::uint16_t patch,
        const std::uint16_t build = 0) noexcept
        : major_(major), minor_(minor), patch_(patch), build_(build) {}

    [[nodiscard]] static std::optional<SemanticVersion> Parse(
        std::string_view text) noexcept;
    [[nodiscard]] static std::optional<SemanticVersion> Parse(
        std::wstring_view text) noexcept;

    [[nodiscard]] constexpr std::uint16_t Major() const noexcept {
        return major_;
    }
    [[nodiscard]] constexpr std::uint16_t Minor() const noexcept {
        return minor_;
    }
    [[nodiscard]] constexpr std::uint16_t Patch() const noexcept {
        return patch_;
    }
    [[nodiscard]] constexpr std::uint16_t Build() const noexcept {
        return build_;
    }

    // The fourth component is omitted when it is zero so release tags and
    // temporary-directory names remain in the usual "1.4.0" form.
    [[nodiscard]] std::string ToString() const;
    [[nodiscard]] std::wstring ToWString() const;

    [[nodiscard]] constexpr bool operator==(
        const SemanticVersion& other) const noexcept = default;
    [[nodiscard]] constexpr std::strong_ordering operator<=> (
        const SemanticVersion& other) const noexcept {
        if (const auto order = major_ <=> other.major_; order != 0) {
            return order;
        }
        if (const auto order = minor_ <=> other.minor_; order != 0) {
            return order;
        }
        if (const auto order = patch_ <=> other.patch_; order != 0) {
            return order;
        }
        return build_ <=> other.build_;
    }

private:
    std::uint16_t major_{};
    std::uint16_t minor_{};
    std::uint16_t patch_{};
    std::uint16_t build_{};
};

}  // namespace qrec::update
