#pragma once

#include <chrono>
#include <cstdint>

namespace qrec::capture {

struct SystemAudioCatchUpPlan final {
    std::uint64_t targetFrames{};
    std::uint64_t silenceFrames{};
};

// Only advances the synthetic-silence timeline through a region that is older
// than both the normal packet holdback and the late-packet grace window. This
// prevents a packet-period-sized scheduling delay from becoming an audible
// zero block before the real WASAPI packet arrives.
[[nodiscard]] SystemAudioCatchUpPlan PlanSystemAudioSilenceCatchUp(
    std::chrono::nanoseconds activeDuration,
    std::uint64_t encodedFrames,
    std::uint32_t sampleRate,
    std::chrono::nanoseconds packetHoldback,
    std::chrono::nanoseconds latePacketGrace) noexcept;

}  // namespace qrec::capture
