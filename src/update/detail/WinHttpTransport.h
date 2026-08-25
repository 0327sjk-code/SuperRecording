#pragma once

#include "update/GitHubUpdateClient.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <stop_token>
#include <string_view>

namespace qrec::update::detail {

struct HttpTransferResult final {
    std::uint64_t bytesTransferred{};
    std::optional<std::uint64_t> contentLength;
    UpdateFailure failure;
};

using HttpChunkWriter = std::function<bool(
    std::span<const std::byte> chunk,
    UpdateFailure* failure)>;

class WinHttpTransport final {
public:
    explicit WinHttpTransport(const GitHubUpdateClientOptions& options);
    ~WinHttpTransport();

    WinHttpTransport(const WinHttpTransport&) = delete;
    WinHttpTransport& operator=(const WinHttpTransport&) = delete;

    void BeginOperation() noexcept;
    void Cancel() noexcept;
    [[nodiscard]] bool CancellationRequested(
        std::stop_token stopToken) const noexcept;

    [[nodiscard]] HttpTransferResult Get(
        std::wstring_view url,
        std::uint64_t maximumBytes,
        const HttpChunkWriter& writer,
        const DownloadProgressCallback& progress,
        std::stop_token stopToken) noexcept;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace qrec::update::detail
