#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>

#include <cstdint>
#include <filesystem>
#include <string>

namespace qrec {

enum class Mp4EditListPatchOutcome : std::uint8_t {
    Succeeded,
    Unsupported,
    Failed,
};

struct Mp4EditListPatchResult final {
    Mp4EditListPatchOutcome outcome{Mp4EditListPatchOutcome::Failed};
    HRESULT nativeError{E_FAIL};
    std::wstring errorMessage;
    bool usedVersion1EditList{};
    std::uint64_t originalMoovBytes{};
    std::uint64_t patchedMoovBytes{};
};

// Rewrites the trailing moov box of a finalized, non-fragmented H.264 MP4.
// mediaStart100Nanoseconds is the amount of decoded preroll hidden at movie
// time zero. visibleDuration100Nanoseconds becomes the movie, track, and edit
// duration. The media-header duration is intentionally preserved so decoders
// can still consume the hidden preroll samples.
//
// The caller must pass a disposable export-staging file. Because moov is the
// final top-level box, the patcher rewrites only that tail and truncates or
// extends the file in place; mdat is never copied or loaded into memory. A
// failed write makes one best-effort O(moov) rollback before reporting failure.
class Mp4EditListPatcher final {
public:
    [[nodiscard]] static Mp4EditListPatchResult Patch(
        const std::filesystem::path& path,
        std::int64_t mediaStart100Nanoseconds,
        std::int64_t visibleDuration100Nanoseconds) noexcept;
};

}  // namespace qrec
