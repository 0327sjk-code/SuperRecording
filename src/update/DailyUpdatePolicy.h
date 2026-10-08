#pragma once

// Daily local-time scheduling and idle gating, independent of Win32/network I/O.
#include "update/UpdateTypes.h"

#include <cstdint>
#include <optional>

namespace qrec::update {

struct UpdateClock final {
    std::uint32_t localDate{};  // YYYYMMDD, in the computer's current time zone.
    std::uint32_t secondsSinceMidnight{};
    std::uint64_t monotonicMilliseconds{};
};

enum class AutomaticUpdateAction : std::uint8_t { None, Check, Download, Install };

class DailyUpdatePolicy final {
public:
    static constexpr std::uint32_t CheckTimeSeconds = 11 * 60 * 60;
    static constexpr std::uint64_t IdleGraceMilliseconds = 3'000;
    static constexpr std::uint64_t RetryDelayMilliseconds = 15 * 60 * 1'000;

    explicit DailyUpdatePolicy(std::uint32_t completedDate = 0) noexcept;
    void RequestManualCheck() noexcept;
    void ObserveActivity(bool idle, std::uint64_t now) noexcept;
    [[nodiscard]] AutomaticUpdateAction NextAction(
        const UpdateClock& clock, bool idle, UpdatePhase phase, bool workerBusy) noexcept;
    void CheckStarted() noexcept;
    void Completed(const UpdateClock& clock) noexcept;
    void RetryLater(const UpdateClock& clock) noexcept;
    [[nodiscard]] std::uint32_t CompletedDate() const noexcept { return completedDate_; }
    [[nodiscard]] static bool IsValidDate(std::uint32_t date) noexcept;

private:
    std::uint32_t completedDate_{};
    std::uint64_t retryAfter_{};
    std::optional<std::uint64_t> idleSince_;
    bool manualRequested_{};
};

}  // namespace qrec::update
