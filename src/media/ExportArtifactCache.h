#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "common/Types.h"

#include <windows.h>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>

namespace qrec {

struct ExportArtifactCacheKey final {
    // Preserves the real canonical casing for file access. It deliberately
    // does not participate in equality or hashing.
    std::wstring sourceAccessPath;
    std::wstring normalizedSourcePath;
    std::uint64_t sourceSize{};
    std::uint64_t sourceLastWriteTime{};
    std::uint64_t sourceVolumeSerialNumber{};
    std::uint64_t sourceFileId{};
    std::uint64_t sourceChangeTime{};
    bool includeSystemAudio{};
    std::wstring audioAccessPath;
    std::wstring normalizedAudioPath;
    std::uint64_t audioSize{};
    std::uint64_t audioLastWriteTime{};
    std::uint64_t audioVolumeSerialNumber{};
    std::uint64_t audioFileId{};
    std::uint64_t audioChangeTime{};
    std::int64_t trimStartMilliseconds{};
    std::int64_t trimEndMilliseconds{};
    OutputFormat format{OutputFormat::Mp4};
    int playbackSpeedTenths{10};

    [[nodiscard]] bool operator==(
        const ExportArtifactCacheKey& other) const noexcept {
        return normalizedSourcePath == other.normalizedSourcePath &&
            sourceSize == other.sourceSize &&
            sourceLastWriteTime == other.sourceLastWriteTime &&
            sourceVolumeSerialNumber == other.sourceVolumeSerialNumber &&
            sourceFileId == other.sourceFileId &&
            sourceChangeTime == other.sourceChangeTime &&
            includeSystemAudio == other.includeSystemAudio &&
            normalizedAudioPath == other.normalizedAudioPath &&
            audioSize == other.audioSize &&
            audioLastWriteTime == other.audioLastWriteTime &&
            audioVolumeSerialNumber == other.audioVolumeSerialNumber &&
            audioFileId == other.audioFileId &&
            audioChangeTime == other.audioChangeTime &&
            trimStartMilliseconds == other.trimStartMilliseconds &&
            trimEndMilliseconds == other.trimEndMilliseconds &&
            format == other.format &&
            playbackSpeedTenths == other.playbackSpeedTenths;
    }
};

enum class MediaArtifactDelivery : std::uint8_t {
    None,
    HardLinked,
    Copied,
};

struct ExportArtifactCacheResult final {
    bool success{};
    bool cancelled{};
    bool cacheHit{};
    bool waitedForBuilder{};
    HRESULT nativeError{E_FAIL};
    std::chrono::milliseconds builderWait{};
    std::chrono::milliseconds generationElapsed{};
    std::filesystem::path artifactPath;
    std::wstring errorMessage;
    std::wstring keyId;
};

class ExportArtifactCache final {
public:
    using Generator = std::function<HRESULT(
        const std::filesystem::path& stagingPath,
        std::stop_token stopToken,
        std::wstring* errorMessage)>;
    using CopyProgressCallback = std::function<void(
        std::uint64_t transferredBytes,
        std::uint64_t totalBytes)>;

    [[nodiscard]] static ExportArtifactCache& Shared();

    [[nodiscard]] static std::optional<ExportArtifactCacheKey> BuildKey(
        const std::filesystem::path& sourcePath,
        std::chrono::milliseconds trimStart,
        std::chrono::milliseconds trimEnd,
        OutputFormat format,
        std::wstring* errorMessage = nullptr) noexcept;

    [[nodiscard]] static std::optional<ExportArtifactCacheKey> BuildKey(
        const ExportRequest& request,
        std::wstring* errorMessage = nullptr) noexcept;

    [[nodiscard]] static std::wstring KeyId(const ExportArtifactCacheKey& key);
    [[nodiscard]] static bool SourceMatchesKey(
        const ExportArtifactCacheKey& key) noexcept;

    [[nodiscard]] ExportArtifactCacheResult GetOrCreate(
        const ExportArtifactCacheKey& key,
        std::wstring_view extension,
        std::stop_token stopToken,
        const Generator& generator);

    [[nodiscard]] static HRESULT Materialize(
        const std::filesystem::path& artifactPath,
        const std::filesystem::path& destinationPath,
        std::stop_token stopToken,
        MediaArtifactDelivery* delivery,
        const CopyProgressCallback& progress = {});

    [[nodiscard]] const std::filesystem::path& RootDirectory() const noexcept;

    ~ExportArtifactCache();
    ExportArtifactCache(const ExportArtifactCache&) = delete;
    ExportArtifactCache& operator=(const ExportArtifactCache&) = delete;

private:
    ExportArtifactCache();

    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace qrec
