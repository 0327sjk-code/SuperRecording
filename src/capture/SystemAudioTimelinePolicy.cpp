#include "capture/SystemAudioTimelinePolicy.h"

#include <algorithm>
#include <limits>

namespace qrec::capture {
namespace {

[[nodiscard]] std::chrono::nanoseconds SaturatingAdd(
    const std::chrono::nanoseconds left,
    const std::chrono::nanoseconds right) noexcept {
    if (left <= std::chrono::nanoseconds::zero()) {
        return std::max(right, std::chrono::nanoseconds::zero());
    }
    if (right <= std::chrono::nanoseconds::zero()) {
        return left;
    }
    if (right.count() >
        std::chrono::nanoseconds::max().count() - left.count()) {
        return std::chrono::nanoseconds::max();
    }
    return left + right;
}

[[nodiscard]] std::uint64_t FramesForDurationCeiling(
    const std::chrono::nanoseconds duration,
    const std::uint32_t sampleRate) noexcept {
    if (duration <= std::chrono::nanoseconds::zero() || sampleRate == 0) {
        return 0;
    }

    constexpr std::uint64_t kNanosecondsPerSecond = 1'000'000'000ULL;
    const std::uint64_t nanoseconds =
        static_cast<std::uint64_t>(duration.count());
    const std::uint64_t wholeSeconds = nanoseconds / kNanosecondsPerSecond;
    const std::uint64_t remainingNanoseconds =
        nanoseconds % kNanosecondsPerSecond;
    if (wholeSeconds >
        std::numeric_limits<std::uint64_t>::max() / sampleRate) {
        return std::numeric_limits<std::uint64_t>::max();
    }

    const std::uint64_t wholeFrames = wholeSeconds * sampleRate;
    const std::uint64_t partialFrames =
        (remainingNanoseconds * sampleRate + kNanosecondsPerSecond - 1ULL) /
        kNanosecondsPerSecond;
    if (partialFrames >
        std::numeric_limits<std::uint64_t>::max() - wholeFrames) {
        return std::numeric_limits<std::uint64_t>::max();
    }
    return wholeFrames + partialFrames;
}

}  // namespace

SystemAudioCatchUpPlan PlanSystemAudioSilenceCatchUp(
    const std::chrono::nanoseconds activeDuration,
    const std::uint64_t encodedFrames,
    const std::uint32_t sampleRate,
    const std::chrono::nanoseconds packetHoldback,
    const std::chrono::nanoseconds latePacketGrace) noexcept {
    if (activeDuration <= std::chrono::nanoseconds::zero() ||
        sampleRate == 0) {
        return {};
    }

    const std::chrono::nanoseconds protectedDuration = SaturatingAdd(
        std::max(packetHoldback, std::chrono::nanoseconds::zero()),
        std::max(latePacketGrace, std::chrono::nanoseconds::zero()));
    if (activeDuration <= protectedDuration) {
        return {};
    }

    const std::uint64_t targetFrames = FramesForDurationCeiling(
        activeDuration - protectedDuration,
        sampleRate);
    return SystemAudioCatchUpPlan{
        targetFrames,
        targetFrames > encodedFrames ? targetFrames - encodedFrames : 0,
    };
}

}  // namespace qrec::capture
