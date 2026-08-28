#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "media/ExportArtifactCache.h"

#include <windows.h>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <string>

namespace qrec::media {

enum class InstantDeliveryOutcome : std::uint8_t {
    Delivered,
    NotApplicable,
    Failed,
};

struct InstantDeliveryResult final {
    InstantDeliveryOutcome outcome{InstantDeliveryOutcome::Failed};
    MediaArtifactDelivery delivery{MediaArtifactDelivery::None};
    std::filesystem::path outputPath;
    std::uint64_t outputBytes{};
    std::chrono::microseconds elapsed{};
    HRESULT nativeError{E_FAIL};
    std::wstring errorMessage;
};

class InstantArtifactDelivery final {
public:
    // Creates an O(1) NTFS hard link. Cross-volume or unsupported filesystems
    // return NotApplicable so the caller can use the asynchronous copy path.
    [[nodiscard]] static InstantDeliveryResult TryHardLink(
        const std::filesystem::path& artifactPath,
        const std::filesystem::path& destinationPath) noexcept;
};

}  // namespace qrec::media
