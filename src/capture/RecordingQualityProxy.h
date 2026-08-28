#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <string>

namespace qrec::capture {

struct RecordingQualityProxyConfig final {
    std::filesystem::path outputPath;
    std::uint32_t sourceWidth{};
    std::uint32_t sourceHeight{};
    std::uint32_t outputWidth{};
    std::uint32_t outputHeight{};
    int framesPerSecond{60};
    int qualityPercent{100};
};

struct RecordingQualityProxyResult final {
    bool attempted{};
    bool success{};
    std::filesystem::path outputPath;
    std::uint32_t outputWidth{};
    std::uint32_t outputHeight{};
    int qualityPercent{100};
    std::uint64_t encodedFrames{};
    std::wstring errorMessage;
    long nativeError{};
};

// Encodes the persisted output-quality variant while recording is in progress.
// A bounded queue protects the primary full-quality recorder from proxy stalls.
class RecordingQualityProxy final {
public:
    RecordingQualityProxy();
    ~RecordingQualityProxy();

    RecordingQualityProxy(const RecordingQualityProxy&) = delete;
    RecordingQualityProxy& operator=(const RecordingQualityProxy&) = delete;

    [[nodiscard]] bool Start(
        const RecordingQualityProxyConfig& config,
        std::wstring* errorMessage = nullptr) noexcept;

    [[nodiscard]] bool SubmitFrame(
        std::span<const std::uint8_t> bgra,
        std::uint32_t sourceStride,
        std::int64_t timestamp100Nanoseconds,
        std::int64_t duration100Nanoseconds) noexcept;

    [[nodiscard]] RecordingQualityProxyResult Finish() noexcept;
    void Cancel() noexcept;
    [[nodiscard]] bool IsAccepting() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace qrec::capture
