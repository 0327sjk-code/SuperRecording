#pragma once

#include "media/Mp4BoundaryEncoderPool.h"

#include <windows.h>

#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>

namespace qrec::detail {
namespace boundary_encoder_pool {

enum class EntryState : std::uint8_t {
    Queued,
    Opening,
    Ready,
    Leased,
    Idle,
    Failed,
    Discarded,
};

[[nodiscard]] std::optional<std::filesystem::path> NormalizeSourcePath(
    const std::filesystem::path& sourcePath) noexcept;
[[nodiscard]] std::wstring SourceIdentity(
    const std::filesystem::path& normalizedPath);
[[nodiscard]] bool KeysMatch(
    const Mp4BoundaryEncoderKey& left,
    const Mp4BoundaryEncoderKey& right) noexcept;
[[nodiscard]] bool IsCurrentThreadMta() noexcept;
void RemoveOwnedPath(const std::filesystem::path& path) noexcept;

struct PreparedSession final {
    std::filesystem::path outputPath;
    std::unique_ptr<media::Mp4Writer> writer;
    std::chrono::milliseconds openDuration{};

    ~PreparedSession();
};

struct PoolEntry final {
    std::uint64_t generation{};
    std::wstring sourceIdentity;
    std::filesystem::path sourcePath;
    std::optional<Mp4BoundaryEncoderKey> key;
    std::unique_ptr<PreparedSession> ready;
    EntryState state{EntryState::Queued};
};

struct OpenWork final {
    std::uint64_t generation{};
    std::filesystem::path sourcePath;
    std::optional<Mp4BoundaryEncoderKey> key;
};

}  // namespace boundary_encoder_pool

struct Mp4BoundaryEncoderLease::Session final {
    Mp4BoundaryEncoderPool* owner{};
    Mp4BoundaryEncoderKey key;
    std::uint64_t generation{};
    std::filesystem::path outputPath;
    std::unique_ptr<media::Mp4Writer> writer;
    bool outputTransferred{};

    ~Session();
};

struct Mp4BoundaryEncoderPool::Impl final {
    Impl();
    ~Impl();

    Impl(const Impl&) = delete;
    Impl& operator=(const Impl&) = delete;

    [[nodiscard]] bool HasWorkerAction() const noexcept;
    void MarkOpenFailed(std::uint64_t generation) noexcept;
    void PublishProbedKey(
        std::uint64_t generation,
        const Mp4BoundaryEncoderKey& key) noexcept;
    [[nodiscard]] std::optional<boundary_encoder_pool::OpenWork> SelectWork(
        std::unique_ptr<boundary_encoder_pool::PreparedSession>* cleanup);
    void DrainReadySessions() noexcept;
    void WorkerLoop(std::stop_token stopToken) noexcept;

    std::mutex mutex;
    std::condition_variable_any changed;
    std::unordered_map<std::uint64_t, boundary_encoder_pool::PoolEntry> entries;
    std::unordered_map<std::wstring, std::uint64_t> currentBySource;
    std::uint64_t nextGeneration{};
    std::jthread worker;
};

}  // namespace qrec::detail
