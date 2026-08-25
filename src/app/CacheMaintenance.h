#pragma once

#include <cstdint>
#include <filesystem>

namespace qrec {

struct CacheCleanupReport final {
    std::uint64_t removedFiles{};
    std::uint64_t releasedBytes{};
    std::uint64_t failures{};
};

// Owns the retention policy for files created only inside SuperRecording's
// private cache directories. It never recurses and never removes directories.
class CacheMaintenance final {
public:
    [[nodiscard]] static CacheCleanupReport Cleanup(
        const std::filesystem::path& saveDirectory) noexcept;
};

}  // namespace qrec
