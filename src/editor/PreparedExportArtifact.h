#pragma once

#include "common/Types.h"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>

namespace qrec {

struct MediaExportResult;

// Keeps the exact artifact prepared for the current editor request. It avoids
// re-entering the cache coordinator when the user clicks Save or Copy.
class PreparedExportArtifact final {
public:
    PreparedExportArtifact();
    ~PreparedExportArtifact();

    PreparedExportArtifact(const PreparedExportArtifact&) = delete;
    PreparedExportArtifact& operator=(const PreparedExportArtifact&) = delete;

    void Clear() noexcept;
    [[nodiscard]] bool Store(
        const ExportRequest& request,
        const MediaExportResult& result) noexcept;
    [[nodiscard]] std::optional<std::filesystem::path> Resolve(
        const ExportRequest& request) const noexcept;
    [[nodiscard]] std::uint64_t OutputBytes() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace qrec
