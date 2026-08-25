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

enum class Mp4BoundaryTrimOutcome : std::uint8_t {
    Succeeded,
    Unsupported,
    Cancelled,
    Failed,
};

struct Mp4BoundaryTrimResult final {
    Mp4BoundaryTrimOutcome outcome{Mp4BoundaryTrimOutcome::Failed};
    HRESULT nativeError{E_FAIL};
    std::wstring errorMessage;
    std::uint64_t encodedFrames{};
    std::uint64_t passthroughSamples{};
    std::chrono::nanoseconds boundaryDuration{};
    std::chrono::milliseconds scanDuration{};
    std::chrono::milliseconds decodeEncodeDuration{};
    std::chrono::milliseconds remuxDuration{};
    bool usedPrewarmedEncoder{};
    std::chrono::milliseconds encoderPrepareWait{};
    std::chrono::milliseconds encoderOpen{};
};

// Exact-start vertical slice for the application's single-track, no-B H.264
// recordings. Frames from the requested start through the next source clean
// point are decoded and re-encoded. Samples from that clean point to the end
// are copied without decoding. The resulting MP4 starts at time zero with an
// IDR and does not rely on an edit list.
class Mp4BoundaryTrimmer final {
public:
    [[nodiscard]] static Mp4BoundaryTrimResult Trim(
        const std::filesystem::path& sourcePath,
        const std::filesystem::path& destinationPath,
        std::chrono::milliseconds trimStart,
        std::chrono::milliseconds trimEnd,
        std::stop_token stopToken = {}) noexcept;
};

}  // namespace qrec
