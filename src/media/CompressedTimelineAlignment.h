#pragma once

#include <cstdint>

namespace qrec::media {

struct CompressedTimelineAlignment final {
    std::int64_t requestedDurationTicks{};
    std::int64_t outputDurationTicks{};
    std::int64_t videoStartAdjustmentTicks{};
    std::int64_t effectiveAudioTrimStartTicks{};
    std::int64_t effectiveAudioTrimEndTicks{};
};

// Aligns a frame-quantized video artifact to the requested range's end. The
// source trimmer always preserves the requested end, while its effective start
// can move forward to the first representable video frame.
[[nodiscard]] bool TryAlignCompressedVideoToRange(
    std::int64_t requestedStartTicks,
    std::int64_t requestedEndTicks,
    std::int64_t measuredVideoDurationTicks,
    CompressedTimelineAlignment* output) noexcept;

// Accepts at most one measured/nominal frame of start-boundary quantization.
// A larger shortfall remains a real truncated-track failure.
[[nodiscard]] bool CoversFrameQuantizedRequestedSpan(
    std::int64_t requestedDurationTicks,
    std::int64_t outputDurationTicks,
    std::int64_t nominalFrameDurationTicks,
    std::int64_t maximumSampleDurationTicks,
    std::int64_t timestampToleranceTicks) noexcept;

}  // namespace qrec::media
