#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <string>

namespace qrec::media {

enum class InterleavedAudioEncoding : std::uint8_t {
    SignedPcm,
    IeeeFloat,
};

struct InterleavedAudioFormat final {
    std::uint32_t sampleRate{48'000};
    std::uint16_t channelCount{2};
    InterleavedAudioEncoding encoding{InterleavedAudioEncoding::IeeeFloat};
    std::uint16_t containerBitsPerSample{32};
    std::uint16_t validBitsPerSample{32};
    std::uint32_t channelMask{};
};

struct AacAudioWriterConfig final {
    std::filesystem::path outputPath;
    InterleavedAudioFormat inputFormat{};
    // Bits per second. Zero selects RecommendBitrate(channelCount).
    std::uint32_t averageBitrate{};
    bool preferHardwareEncoder{true};
};

class AacAudioWriter final {
public:
    AacAudioWriter();
    ~AacAudioWriter();

    AacAudioWriter(const AacAudioWriter&) = delete;
    AacAudioWriter& operator=(const AacAudioWriter&) = delete;

    [[nodiscard]] bool Open(
        const AacAudioWriterConfig& config,
        std::wstring& errorMessage,
        long& nativeError) noexcept;

    [[nodiscard]] bool WriteInterleavedFrames(
        std::span<const std::byte> interleavedSamples,
        std::uint32_t frameCount,
        std::int64_t timestamp100Nanoseconds,
        std::int64_t duration100Nanoseconds,
        std::wstring& errorMessage,
        long& nativeError) noexcept;

    [[nodiscard]] bool WriteSilentFrames(
        std::uint32_t frameCount,
        std::int64_t timestamp100Nanoseconds,
        std::int64_t duration100Nanoseconds,
        std::wstring& errorMessage,
        long& nativeError) noexcept;

    [[nodiscard]] bool Finalize(
        std::wstring& errorMessage,
        long& nativeError) noexcept;

    [[nodiscard]] bool IsOpen() const noexcept;
    [[nodiscard]] std::uint32_t AverageBitrate() const noexcept;
    [[nodiscard]] InterleavedAudioFormat InputFormat() const noexcept;

    [[nodiscard]] static std::uint32_t RecommendBitrate(
        std::uint16_t channelCount) noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace qrec::media
