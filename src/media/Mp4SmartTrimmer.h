#pragma once

#include <windows.h>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <stop_token>
#include <string>

namespace qrec {

enum class Mp4SmartTrimOutcome : std::uint8_t {
    Succeeded,
    Unsupported,
    Cancelled,
    Failed,
};

struct Mp4SmartTrimResult final {
    Mp4SmartTrimOutcome outcome{Mp4SmartTrimOutcome::Failed};
    HRESULT nativeError{E_FAIL};
    std::wstring errorMessage;
    std::uint64_t writtenSamples{};
    std::uint64_t prerollSamples{};
};

class Mp4SmartTrimmer final {
public:
    using ProgressCallback = std::function<void(double)>;

    // Copies already encoded H.264 samples into a new MP4 container. When the
    // requested start lies inside a GOP, the preceding clean point is retained
    // with non-negative timestamps. A dedicated ISO-BMFF patcher then writes
    // edts/elst so decoders consume that preroll while playback begins on the
    // first selected video frame. No H.264 sample is re-encoded.
    [[nodiscard]] static Mp4SmartTrimResult Trim(
        const std::filesystem::path& sourcePath,
        const std::filesystem::path& destinationPath,
        std::chrono::milliseconds trimStart,
        std::chrono::milliseconds trimEnd,
        std::stop_token stopToken,
        const ProgressCallback& progress = {});
};

}  // namespace qrec
