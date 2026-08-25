#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <stop_token>

namespace qrec::media {
class Mp4Writer;
}

namespace qrec::detail {

struct Mp4BoundaryEncoderKey final {
    std::filesystem::path sourcePath;
    std::uint32_t width{};
    std::uint32_t height{};
    int framesPerSecond{};
    std::uint32_t averageBitrate{};
};

enum class Mp4BoundaryEncoderAcquireOutcome : std::uint8_t {
    Acquired,
    Unavailable,
    Cancelled,
};

struct Mp4BoundaryEncoderAcquireStats final {
    std::uint64_t generation{};
    bool usedPrewarmedEncoder{};
    std::chrono::milliseconds prepareWait{};
    std::chrono::milliseconds encoderOpen{};
};

class Mp4BoundaryEncoderPool;

class Mp4BoundaryEncoderLease final {
public:
    Mp4BoundaryEncoderLease() noexcept;
    ~Mp4BoundaryEncoderLease();
    Mp4BoundaryEncoderLease(Mp4BoundaryEncoderLease&& other) noexcept;
    Mp4BoundaryEncoderLease& operator=(
        Mp4BoundaryEncoderLease&& other) noexcept;

    Mp4BoundaryEncoderLease(const Mp4BoundaryEncoderLease&) = delete;
    Mp4BoundaryEncoderLease& operator=(
        const Mp4BoundaryEncoderLease&) = delete;

    [[nodiscard]] explicit operator bool() const noexcept;
    [[nodiscard]] media::Mp4Writer* Writer() const noexcept;
    [[nodiscard]] const std::filesystem::path& OutputPath() const noexcept;
    [[nodiscard]] std::uint64_t Generation() const noexcept;

    // Call only after Mp4Writer::Finalize succeeds. Ownership of the finished
    // file transfers to the boundary trimmer, which performs transactional
    // cleanup after compatibility validation and remuxing.
    [[nodiscard]] std::filesystem::path TakeFinalizedPath() noexcept;

private:
    struct Session;
    explicit Mp4BoundaryEncoderLease(std::unique_ptr<Session> session) noexcept;

    std::unique_ptr<Session> session_;
    friend class Mp4BoundaryEncoderPool;
};

class Mp4BoundaryEncoderPool final {
public:
    [[nodiscard]] static Mp4BoundaryEncoderPool& Shared();

    Mp4BoundaryEncoderPool(const Mp4BoundaryEncoderPool&) = delete;
    Mp4BoundaryEncoderPool& operator=(const Mp4BoundaryEncoderPool&) = delete;

    [[nodiscard]] static std::optional<Mp4BoundaryEncoderKey> MakeKey(
        const std::filesystem::path& sourcePath,
        std::uint32_t width,
        std::uint32_t height,
        int framesPerSecond,
        std::uint32_t averageBitrate) noexcept;

    // Non-blocking. Source inspection and Mp4Writer::Open run on the
    // persistent MTA worker.
    [[nodiscard]] std::uint64_t Prepare(
        const std::filesystem::path& sourcePath) noexcept;

    // Non-blocking. Ready encoders are released and their files are deleted
    // by the persistent worker, never by the editor UI thread.
    void Discard(
        const std::filesystem::path& sourcePath,
        std::uint64_t generation) noexcept;

    [[nodiscard]] Mp4BoundaryEncoderAcquireOutcome TryAcquire(
        const Mp4BoundaryEncoderKey& key,
        std::stop_token stopToken,
        Mp4BoundaryEncoderLease* lease,
        Mp4BoundaryEncoderAcquireStats* stats) noexcept;

    // Non-blocking. Re-opens one encoder after a complete successful trim.
    void Replenish(
        const Mp4BoundaryEncoderKey& key,
        std::uint64_t generation) noexcept;

    ~Mp4BoundaryEncoderPool();

private:
    Mp4BoundaryEncoderPool();
    void ReleaseLease(
        const Mp4BoundaryEncoderKey& key,
        std::uint64_t generation) noexcept;

    struct Impl;
    std::unique_ptr<Impl> impl_;
    friend struct Mp4BoundaryEncoderLease::Session;
};

}  // namespace qrec::detail
