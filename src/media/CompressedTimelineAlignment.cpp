#include "media/CompressedTimelineAlignment.h"

#include <algorithm>
#include <limits>

namespace qrec::media {

bool TryAlignCompressedVideoToRange(
    const std::int64_t requestedStartTicks,
    const std::int64_t requestedEndTicks,
    const std::int64_t measuredVideoDurationTicks,
    CompressedTimelineAlignment* output) noexcept {
    if (output == nullptr || requestedStartTicks < 0 ||
        requestedEndTicks <= requestedStartTicks ||
        measuredVideoDurationTicks <= 0) {
        return false;
    }

    const std::int64_t requestedDurationTicks =
        requestedEndTicks - requestedStartTicks;
    const std::int64_t outputDurationTicks = std::min(
        requestedDurationTicks,
        measuredVideoDurationTicks);
    if (outputDurationTicks <= 0) {
        return false;
    }

    CompressedTimelineAlignment aligned{};
    aligned.requestedDurationTicks = requestedDurationTicks;
    aligned.outputDurationTicks = outputDurationTicks;
    aligned.effectiveAudioTrimEndTicks = requestedEndTicks;
    aligned.effectiveAudioTrimStartTicks =
        requestedEndTicks - outputDurationTicks;
    aligned.videoStartAdjustmentTicks =
        aligned.effectiveAudioTrimStartTicks - requestedStartTicks;
    *output = aligned;
    return true;
}

bool CoversFrameQuantizedRequestedSpan(
    const std::int64_t requestedDurationTicks,
    const std::int64_t outputDurationTicks,
    const std::int64_t nominalFrameDurationTicks,
    const std::int64_t maximumSampleDurationTicks,
    const std::int64_t timestampToleranceTicks) noexcept {
    if (requestedDurationTicks <= 0 || outputDurationTicks <= 0 ||
        timestampToleranceTicks < 0) {
        return false;
    }
    if (outputDurationTicks >= requestedDurationTicks) {
        return true;
    }

    const std::int64_t frameDurationTicks = std::max(
        nominalFrameDurationTicks,
        maximumSampleDurationTicks);
    if (frameDurationTicks <= 0) {
        return false;
    }
    const std::int64_t permittedShortfallTicks =
        frameDurationTicks >
                std::numeric_limits<std::int64_t>::max() -
                    timestampToleranceTicks
            ? std::numeric_limits<std::int64_t>::max()
            : frameDurationTicks + timestampToleranceTicks;
    return requestedDurationTicks - outputDurationTicks <=
        permittedShortfallTicks;
}

}  // namespace qrec::media
