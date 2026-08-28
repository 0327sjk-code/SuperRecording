#pragma once

#include <cstdint>

namespace qrec::media {

struct ExportPixelSize final {
    std::uint32_t width{};
    std::uint32_t height{};
};

// Central policy shared by the editor estimate, preview proxy and final
// exporter. A percentage scales each image dimension, so 50% produces one
// quarter of the source pixel count while preserving the aspect ratio.
class ExportQuality final {
public:
    static constexpr int MinimumPercent = 25;
    static constexpr int MaximumPercent = 100;
    static constexpr int DefaultPercent = 100;
    static constexpr int StepPercent = 5;

    static constexpr std::uint32_t GifMaximumWidth = 1280;
    static constexpr std::uint32_t GifMaximumHeight = 720;

    [[nodiscard]] static bool IsValid(int qualityPercent) noexcept;
    [[nodiscard]] static int Clamp(int qualityPercent) noexcept;
    [[nodiscard]] static int Normalize(int qualityPercent) noexcept;

    [[nodiscard]] static ExportPixelSize ComputeMp4Size(
        std::uint32_t sourceWidth,
        std::uint32_t sourceHeight,
        int qualityPercent) noexcept;

    [[nodiscard]] static ExportPixelSize ComputeGifSize(
        std::uint32_t sourceWidth,
        std::uint32_t sourceHeight,
        int qualityPercent) noexcept;

    [[nodiscard]] static std::uint32_t ComputeVideoBitrate(
        std::uint32_t outputWidth,
        std::uint32_t outputHeight,
        int framesPerSecond,
        int qualityPercent) noexcept;
};

}  // namespace qrec::media
