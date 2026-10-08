#include "update/DailyUpdatePolicy.h"

#include <chrono>

namespace qrec::update {

DailyUpdatePolicy::DailyUpdatePolicy(const std::uint32_t completedDate) noexcept
    : completedDate_(IsValidDate(completedDate) ? completedDate : 0) {}

bool DailyUpdatePolicy::IsValidDate(const std::uint32_t date) noexcept {
    const auto year = date / 10'000;
    if (year < 2000 || year > 9999) return false;
    return std::chrono::year_month_day{
        std::chrono::year{static_cast<int>(year)},
        std::chrono::month{(date / 100) % 100},
        std::chrono::day{date % 100}}.ok();
}

void DailyUpdatePolicy::RequestManualCheck() noexcept {
    manualRequested_ = true;
    retryAfter_ = 0;
}

void DailyUpdatePolicy::ObserveActivity(const bool idle, const std::uint64_t now) noexcept {
    if (!idle) idleSince_.reset();
    else if (!idleSince_.has_value() || now < *idleSince_) idleSince_ = now;
}

AutomaticUpdateAction DailyUpdatePolicy::NextAction(
    const UpdateClock& clock, const bool idle, const UpdatePhase phase,
    const bool workerBusy) noexcept {
    ObserveActivity(idle, clock.monotonicMilliseconds);
    if (!idle || !idleSince_.has_value() ||
        clock.monotonicMilliseconds - *idleSince_ < IdleGraceMilliseconds ||
        workerBusy || clock.monotonicMilliseconds < retryAfter_) {
        return AutomaticUpdateAction::None;
    }
    // A manually initiated operation also obeys the exact same idle gate.
    if (phase == UpdatePhase::ReadyToInstall) return AutomaticUpdateAction::Install;
    if (phase == UpdatePhase::UpdateAvailable) return AutomaticUpdateAction::Download;
    if (phase == UpdatePhase::Checking || phase == UpdatePhase::Downloading)
        return AutomaticUpdateAction::None;
    if (manualRequested_ || (IsValidDate(clock.localDate) &&
        clock.secondsSinceMidnight >= CheckTimeSeconds &&
        clock.secondsSinceMidnight < 24 * 60 * 60 && completedDate_ != clock.localDate)) {
        return AutomaticUpdateAction::Check;
    }
    return AutomaticUpdateAction::None;
}

void DailyUpdatePolicy::CheckStarted() noexcept { manualRequested_ = false; }

void DailyUpdatePolicy::Completed(const UpdateClock& clock) noexcept {
    // A manual check before 11:00 must not suppress today's scheduled check.
    if (clock.secondsSinceMidnight >= CheckTimeSeconds && IsValidDate(clock.localDate))
        completedDate_ = clock.localDate;
    retryAfter_ = 0;
    manualRequested_ = false;
}

void DailyUpdatePolicy::RetryLater(const UpdateClock& clock) noexcept {
    retryAfter_ = clock.monotonicMilliseconds + RetryDelayMilliseconds;
    // Retry manual operations too, including those started before 11:00.
    manualRequested_ = true;
}

}  // namespace qrec::update
