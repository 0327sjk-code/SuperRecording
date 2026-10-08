#pragma once

// Persist scheduler state separately; recorder preferences are never rewritten.
#include <cstdint>
#include <filesystem>

namespace qrec::update {
class DailyUpdateStateStore final {
public:
    explicit DailyUpdateStateStore(std::filesystem::path path);
    [[nodiscard]] std::uint32_t LoadCompletedDate() const noexcept;
    [[nodiscard]] bool SaveCompletedDate(std::uint32_t date) const noexcept;
private:
    std::filesystem::path path_;
};
}  // namespace qrec::update
