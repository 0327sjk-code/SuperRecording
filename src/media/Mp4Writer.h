#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <string>

namespace qrec::media {

struct Mp4WriterConfig final {
    std::filesystem::path outputPath;
    std::uint32_t width{};
    std::uint32_t height{};
    int framesPerSecond{60};
    std::uint32_t averageBitrate{};
    bool preferHardwareEncoder{true};
    std::uint32_t forcedKeyframeIntervalMilliseconds{500};
};

class Mp4Writer final {
public:
    Mp4Writer();
    ~Mp4Writer();

    Mp4Writer(const Mp4Writer&) = delete;
    Mp4Writer& operator=(const Mp4Writer&) = delete;

    [[nodiscard]] bool Open(
        const Mp4WriterConfig& config,
        std::wstring& errorMessage,
        long& nativeError) noexcept;

    [[nodiscard]] bool WriteBgraFrame(
        std::span<const std::uint8_t> bgra,
        std::uint32_t sourceStride,
        std::int64_t timestamp100Nanoseconds,
        std::int64_t duration100Nanoseconds,
        std::wstring& errorMessage,
        long& nativeError) noexcept;

    [[nodiscard]] bool Finalize(
        std::wstring& errorMessage,
        long& nativeError) noexcept;

    [[nodiscard]] bool IsOpen() const noexcept;
    [[nodiscard]] std::uint32_t AverageBitrate() const noexcept;

    [[nodiscard]] static std::uint32_t RecommendBitrate(
        std::uint32_t width,
        std::uint32_t height,
        int framesPerSecond) noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace qrec::media
