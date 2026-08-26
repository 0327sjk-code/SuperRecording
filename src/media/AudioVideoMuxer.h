#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <stop_token>
#include <string>

namespace qrec {

enum class AudioVideoMuxOutcome : std::uint8_t {
    Succeeded,
    Unsupported,
    Cancelled,
    Failed,
};

// AAC is copied as complete compressed access units. A unit that crosses the
// requested start or end is not emitted, so neither boundary can expose audio
// outside the selected interval. This can leave less than one AAC access unit
// of silence at each edge. Sample timestamps remain synchronized to the
// requested trim start; the first audio timestamp is therefore normally in
// [0, oneAccessUnitDuration), while the first video timestamp is exactly zero.
enum class AacBoundaryAlignment : std::uint8_t {
    CompleteAccessUnitsInsideRange,
};

struct AudioVideoMuxRequest final {
    // The video must already represent the selected interval, begin at time
    // zero with an H.264 CleanPoint, and contain decode-order/no-B samples.
    // Its compressed payloads are passed to the destination MP4 unchanged.
    std::filesystem::path trimmedVideoPath;

    // Full-recording AAC-in-M4A/MP4 sidecar. Its timestamps must use the same
    // recording clock as trimStart/trimEnd.
    std::filesystem::path audioSidecarPath;
    std::filesystem::path destinationPath;
    std::chrono::milliseconds trimStart{};
    std::chrono::milliseconds trimEnd{};
};

struct AudioVideoMuxResult final {
    AudioVideoMuxOutcome outcome{AudioVideoMuxOutcome::Failed};
    HRESULT nativeError{E_FAIL};
    std::wstring errorMessage;
    AacBoundaryAlignment alignment{
        AacBoundaryAlignment::CompleteAccessUnitsInsideRange};
    std::uint64_t videoSamples{};
    std::uint64_t audioSamples{};

    // These gaps are measured against [trimStart, trimEnd]. They include the
    // access-unit boundary loss and any gap already present in the sidecar.
    std::chrono::nanoseconds audioLeadingGap{};
    std::chrono::nanoseconds audioTrailingGap{};
    bool droppedLeadingBoundaryAccessUnit{};
    bool droppedTrailingBoundaryAccessUnit{};
};

struct CompressedVideoRetimeRequest final {
    // Source must begin with a time-zero H.264 CleanPoint and must not contain
    // B-frame reordering. The compressed H.264 payload is copied byte-for-byte.
    std::filesystem::path sourcePath;
    std::filesystem::path destinationPath;
    int playbackSpeedTenths{10};
};

struct CompressedVideoRetimeResult final {
    AudioVideoMuxOutcome outcome{AudioVideoMuxOutcome::Failed};
    HRESULT nativeError{E_FAIL};
    std::wstring errorMessage;
    std::uint64_t videoSamples{};
    std::chrono::nanoseconds outputDuration{};
};

// Repackages an already-trimmed H.264 MP4 and a full-recording AAC sidecar
// into one MP4. Neither track is decoded or encoded. H.264 samples are copied
// byte-for-byte through the Media Foundation MPEG-4 sink; AAC samples are
// selected by source timestamps and rebased by trimStart. The operation is
// synchronous and intended for a cancellable background thread.
class AudioVideoMuxer final {
public:
    [[nodiscard]] static AudioVideoMuxResult Mux(
        const AudioVideoMuxRequest& request,
        std::stop_token stopToken = {}) noexcept;

    // Re-times an H.264-only MP4 without decoding or re-encoding its picture.
    // Only sample timestamps/durations and container timing are regenerated.
    [[nodiscard]] static CompressedVideoRetimeResult RetimeCompressedVideo(
        const CompressedVideoRetimeRequest& request,
        std::stop_token stopToken = {}) noexcept;
};

}  // namespace qrec
