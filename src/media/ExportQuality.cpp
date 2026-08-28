#include "media/ExportQuality.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace qrec::media {
namespace {

constexpr std::uint32_t kMinimumMp4Dimension = 16;
constexpr std::uint32_t kLegacyMinimumBitrate = 4'000'000;
constexpr std::uint32_t kReducedMinimumBitrate = 500'000;
constexpr std::uint32_t kMaximumBitrate = 60'000'000;
constexpr std::uint32_t kReducedBitrateStep = 50'000;
constexpr long double kBitsPerPixelAt60Fps = 0.160L;
constexpr long double kBitsPerPixelAt30Fps = 0.190L;
constexpr long double kMinimumReducedQualityFactor = 0.70L;
constexpr long double kReducedQualityFactorRange = 0.30L;

[[nodiscard]] std::uint32_t ScaleEvenDimension(
    const std::uint32_t source,
    const int qualityPercent) noexcept {
    if (source == 0) {
        return 0;
    }
    if (qualityPercent >= ExportQuality::MaximumPercent) {
        return source;
    }

    const long double scaled = static_cast<long double>(source) *
        static_cast<long double>(qualityPercent) /
        static_cast<long double>(ExportQuality::MaximumPercent);
    std::uint64_t rounded = static_cast<std::uint64_t>(std::llround(scaled));
    rounded = std::max<std::uint64_t>(kMinimumMp4Dimension, rounded);
    rounded &= ~std::uint64_t{1};

    const std::uint64_t largestEvenSource = source & ~std::uint32_t{1};
    if (largestEvenSource >= kMinimumMp4Dimension) {
        rounded = std::min(rounded, largestEvenSource);
    }
    return static_cast<std::uint32_t>(std::min<std::uint64_t>(
        rounded,
        std::numeric_limits<std::uint32_t>::max()));
}

[[nodiscard]] std::uint32_t RoundBitrate(
    const long double value,
    const std::uint32_t minimum,
    const std::uint32_t maximum,
    const std::uint32_t step) noexcept {
    const auto bounded = static_cast<std::uint64_t>(std::llround(std::clamp(
        value,
        static_cast<long double>(minimum),
        static_cast<long double>(maximum))));
    const std::uint64_t rounded = step == 0
        ? bounded
        : ((bounded + step / 2U) / step) * step;
    return static_cast<std::uint32_t>(std::min<std::uint64_t>(rounded, maximum));
}

}  // namespace

bool ExportQuality::IsValid(const int qualityPercent) noexcept {
    return qualityPercent >= MinimumPercent && qualityPercent <= MaximumPercent;
}

int ExportQuality::Clamp(const int qualityPercent) noexcept {
    return std::clamp(qualityPercent, MinimumPercent, MaximumPercent);
}

int ExportQuality::Normalize(const int qualityPercent) noexcept {
    const int clamped = Clamp(qualityPercent);
    const int offset = clamped - MinimumPercent;
    const int snappedSteps = (offset + StepPercent / 2) / StepPercent;
    return std::clamp(
        MinimumPercent + snappedSteps * StepPercent,
        MinimumPercent,
        MaximumPercent);
}

ExportPixelSize ExportQuality::ComputeMp4Size(
    const std::uint32_t sourceWidth,
    const std::uint32_t sourceHeight,
    const int qualityPercent) noexcept {
    const int quality = Clamp(qualityPercent);
    return {
        ScaleEvenDimension(sourceWidth, quality),
        ScaleEvenDimension(sourceHeight, quality)};
}

ExportPixelSize ExportQuality::ComputeGifSize(
    const std::uint32_t sourceWidth,
    const std::uint32_t sourceHeight,
    const int qualityPercent) noexcept {
    if (sourceWidth == 0 || sourceHeight == 0) {
        return {};
    }
    const long double maximumScale = std::min({
        1.0L,
        static_cast<long double>(GifMaximumWidth) / sourceWidth,
        static_cast<long double>(GifMaximumHeight) / sourceHeight});
    const long double qualityScale = static_cast<long double>(Clamp(qualityPercent)) /
        static_cast<long double>(MaximumPercent);
    const long double scale = maximumScale * qualityScale;
    return {
        std::max<std::uint32_t>(
            1,
            static_cast<std::uint32_t>(std::llround(sourceWidth * scale))),
        std::max<std::uint32_t>(
            1,
            static_cast<std::uint32_t>(std::llround(sourceHeight * scale)))};
}

std::uint32_t ExportQuality::ComputeVideoBitrate(
    const std::uint32_t outputWidth,
    const std::uint32_t outputHeight,
    const int framesPerSecond,
    const int qualityPercent) noexcept {
    if (outputWidth == 0 || outputHeight == 0 || framesPerSecond <= 0) {
        return 0;
    }

    const long double bitsPerPixel = framesPerSecond >= 60
        ? kBitsPerPixelAt60Fps
        : kBitsPerPixelAt30Fps;
    const long double base = static_cast<long double>(outputWidth) *
        static_cast<long double>(outputHeight) *
        static_cast<long double>(framesPerSecond) * bitsPerPixel;
    if (qualityPercent >= MaximumPercent) {
        return RoundBitrate(
            base,
            kLegacyMinimumBitrate,
            kMaximumBitrate,
            1);
    }

    const long double normalizedQuality = static_cast<long double>(Clamp(qualityPercent)) /
        static_cast<long double>(MaximumPercent);
    const long double qualityFactor = kMinimumReducedQualityFactor +
        kReducedQualityFactorRange * normalizedQuality;
    return RoundBitrate(
        base * qualityFactor,
        kReducedMinimumBitrate,
        kMaximumBitrate,
        kReducedBitrateStep);
}

}  // namespace qrec::media
