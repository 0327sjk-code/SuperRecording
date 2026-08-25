#pragma once

#include "update/detail/WinHttpTransport.h"

#include <cstdint>
#include <functional>

namespace qrec::update::detail {

using HttpTransferOperation =
    std::function<HttpTransferResult(const HttpChunkWriter& writer)>;

// Owns the transactional .part -> validated x64 PE -> final-file workflow.
class UpdateFileStore final {
public:
    explicit UpdateFileStore(std::uint64_t maximumBytes) noexcept;

    [[nodiscard]] ExecutableDownloadResult Acquire(
        const SemanticVersion& version,
        const HttpTransferOperation& transfer) const noexcept;

private:
    std::uint64_t maximumBytes_{};
};

}  // namespace qrec::update::detail
