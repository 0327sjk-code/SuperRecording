#include "media/Mp4BoundaryEncoderPool.h"

#include "media/Mp4BoundaryEncoderPoolInternal.h"
#include "media/Mp4Writer.h"

#include <utility>

namespace qrec::detail {

using boundary_encoder_pool::EntryState;
using boundary_encoder_pool::PoolEntry;

Mp4BoundaryEncoderLease::Mp4BoundaryEncoderLease() noexcept = default;

Mp4BoundaryEncoderLease::Mp4BoundaryEncoderLease(
    std::unique_ptr<Session> session) noexcept
    : session_(std::move(session)) {}

Mp4BoundaryEncoderLease::~Mp4BoundaryEncoderLease() = default;

Mp4BoundaryEncoderLease::Mp4BoundaryEncoderLease(
    Mp4BoundaryEncoderLease&& other) noexcept = default;

Mp4BoundaryEncoderLease& Mp4BoundaryEncoderLease::operator=(
    Mp4BoundaryEncoderLease&& other) noexcept = default;

Mp4BoundaryEncoderLease::operator bool() const noexcept {
    return session_ != nullptr && session_->writer != nullptr;
}

media::Mp4Writer* Mp4BoundaryEncoderLease::Writer() const noexcept {
    return session_ != nullptr ? session_->writer.get() : nullptr;
}

const std::filesystem::path& Mp4BoundaryEncoderLease::OutputPath() const noexcept {
    static const std::filesystem::path empty;
    return session_ != nullptr ? session_->outputPath : empty;
}

std::uint64_t Mp4BoundaryEncoderLease::Generation() const noexcept {
    return session_ != nullptr ? session_->generation : 0;
}

std::filesystem::path Mp4BoundaryEncoderLease::TakeFinalizedPath() noexcept {
    if (session_ == nullptr || session_->writer == nullptr ||
        session_->writer->IsOpen()) {
        return {};
    }
    session_->writer.reset();
    session_->outputTransferred = true;
    return std::move(session_->outputPath);
}

Mp4BoundaryEncoderPool& Mp4BoundaryEncoderPool::Shared() {
    static Mp4BoundaryEncoderPool pool;
    return pool;
}

Mp4BoundaryEncoderPool::Mp4BoundaryEncoderPool()
    : impl_(std::make_unique<Impl>()) {}

Mp4BoundaryEncoderPool::~Mp4BoundaryEncoderPool() = default;

std::optional<Mp4BoundaryEncoderKey> Mp4BoundaryEncoderPool::MakeKey(
    const std::filesystem::path& sourcePath,
    const std::uint32_t width,
    const std::uint32_t height,
    const int framesPerSecond,
    const std::uint32_t averageBitrate) noexcept {
    const std::optional<std::filesystem::path> normalized =
        boundary_encoder_pool::NormalizeSourcePath(sourcePath);
    if (!normalized.has_value() || width == 0 || height == 0 ||
        (framesPerSecond != 30 && framesPerSecond != 60) ||
        averageBitrate == 0) {
        return std::nullopt;
    }
    return Mp4BoundaryEncoderKey{
        *normalized,
        width,
        height,
        framesPerSecond,
        averageBitrate};
}

std::uint64_t Mp4BoundaryEncoderPool::Prepare(
    const std::filesystem::path& sourcePath) noexcept {
    try {
        const std::optional<std::filesystem::path> normalized =
            boundary_encoder_pool::NormalizeSourcePath(sourcePath);
        if (!normalized.has_value()) {
            return 0;
        }
        const std::wstring identity =
            boundary_encoder_pool::SourceIdentity(*normalized);
        std::uint64_t generation = 0;
        {
            std::scoped_lock lock(impl_->mutex);
            const auto current = impl_->currentBySource.find(identity);
            if (current != impl_->currentBySource.end()) {
                const auto existing = impl_->entries.find(current->second);
                if (existing != impl_->entries.end() &&
                    existing->second.state != EntryState::Discarded) {
                    if (existing->second.state == EntryState::Failed ||
                        existing->second.state == EntryState::Idle) {
                        existing->second.state = EntryState::Queued;
                    }
                    generation = existing->second.generation;
                }
            }
            if (generation == 0) {
                generation = ++impl_->nextGeneration;
                if (generation == 0) {
                    generation = ++impl_->nextGeneration;
                }
                PoolEntry entry{};
                entry.generation = generation;
                entry.sourceIdentity = identity;
                entry.sourcePath = *normalized;
                entry.state = EntryState::Queued;
                impl_->entries.emplace(generation, std::move(entry));
                impl_->currentBySource[identity] = generation;
            }
        }
        impl_->changed.notify_all();
        return generation;
    } catch (...) {
        return 0;
    }
}

void Mp4BoundaryEncoderPool::Discard(
    const std::filesystem::path& sourcePath,
    const std::uint64_t generation) noexcept {
    if (generation == 0) {
        return;
    }
    try {
        const std::optional<std::filesystem::path> normalized =
            boundary_encoder_pool::NormalizeSourcePath(sourcePath);
        if (!normalized.has_value()) {
            return;
        }
        const std::wstring identity =
            boundary_encoder_pool::SourceIdentity(*normalized);
        {
            std::scoped_lock lock(impl_->mutex);
            const auto current = impl_->currentBySource.find(identity);
            if (current == impl_->currentBySource.end() ||
                current->second != generation) {
                return;
            }
            impl_->currentBySource.erase(current);
            const auto entry = impl_->entries.find(generation);
            if (entry == impl_->entries.end()) {
                return;
            }
            if (entry->second.state == EntryState::Idle ||
                entry->second.state == EntryState::Failed) {
                impl_->entries.erase(entry);
            } else {
                entry->second.state = EntryState::Discarded;
            }
        }
        impl_->changed.notify_all();
    } catch (...) {
    }
}

Mp4BoundaryEncoderAcquireOutcome Mp4BoundaryEncoderPool::TryAcquire(
    const Mp4BoundaryEncoderKey& requestedKey,
    const std::stop_token stopToken,
    Mp4BoundaryEncoderLease* lease,
    Mp4BoundaryEncoderAcquireStats* stats) noexcept {
    if (lease == nullptr || stats == nullptr) {
        return Mp4BoundaryEncoderAcquireOutcome::Unavailable;
    }
    *stats = {};
    if (stopToken.stop_requested()) {
        return Mp4BoundaryEncoderAcquireOutcome::Cancelled;
    }
    const std::optional<Mp4BoundaryEncoderKey> normalizedKey = MakeKey(
        requestedKey.sourcePath,
        requestedKey.width,
        requestedKey.height,
        requestedKey.framesPerSecond,
        requestedKey.averageBitrate);
    if (!normalizedKey.has_value() ||
        !boundary_encoder_pool::IsCurrentThreadMta()) {
        return Mp4BoundaryEncoderAcquireOutcome::Unavailable;
    }

    try {
        const std::wstring identity =
            boundary_encoder_pool::SourceIdentity(normalizedKey->sourcePath);
        std::unique_lock lock(impl_->mutex);
        auto current = impl_->currentBySource.find(identity);
        if (current == impl_->currentBySource.end()) {
            return Mp4BoundaryEncoderAcquireOutcome::Unavailable;
        }
        stats->generation = current->second;

        bool waited = false;
        const auto waitStarted = std::chrono::steady_clock::now();
        auto readyToDecide = [&]() noexcept {
            if (stopToken.stop_requested()) {
                return true;
            }
            current = impl_->currentBySource.find(identity);
            if (current == impl_->currentBySource.end() ||
                current->second != stats->generation) {
                return true;
            }
            const auto entry = impl_->entries.find(stats->generation);
            if (entry == impl_->entries.end()) {
                return true;
            }
            if (entry->second.key.has_value() &&
                !boundary_encoder_pool::KeysMatch(
                    *entry->second.key,
                    *normalizedKey)) {
                return true;
            }
            return entry->second.state != EntryState::Queued &&
                   entry->second.state != EntryState::Opening;
        };

        const auto initialEntry = impl_->entries.find(stats->generation);
        if (initialEntry != impl_->entries.end() &&
            (initialEntry->second.state == EntryState::Queued ||
             initialEntry->second.state == EntryState::Opening)) {
            waited = true;
            static_cast<void>(impl_->changed.wait(
                lock,
                stopToken,
                readyToDecide));
        }
        if (waited) {
            stats->prepareWait =
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - waitStarted);
        }
        if (stopToken.stop_requested()) {
            return Mp4BoundaryEncoderAcquireOutcome::Cancelled;
        }

        current = impl_->currentBySource.find(identity);
        if (current == impl_->currentBySource.end() ||
            current->second != stats->generation) {
            return Mp4BoundaryEncoderAcquireOutcome::Unavailable;
        }
        const auto entry = impl_->entries.find(stats->generation);
        if (entry == impl_->entries.end() ||
            entry->second.state != EntryState::Ready ||
            entry->second.ready == nullptr ||
            !entry->second.key.has_value() ||
            !boundary_encoder_pool::KeysMatch(
                *entry->second.key,
                *normalizedKey)) {
            return Mp4BoundaryEncoderAcquireOutcome::Unavailable;
        }

        auto leaseSession = std::make_unique<Mp4BoundaryEncoderLease::Session>();
        leaseSession->owner = this;
        leaseSession->key = *normalizedKey;
        leaseSession->generation = stats->generation;
        leaseSession->outputPath =
            std::move(entry->second.ready->outputPath);
        leaseSession->writer = std::move(entry->second.ready->writer);
        stats->encoderOpen = entry->second.ready->openDuration;
        entry->second.ready->outputPath.clear();
        entry->second.ready.reset();
        entry->second.state = EntryState::Leased;
        stats->usedPrewarmedEncoder = true;
        lock.unlock();
        *lease = Mp4BoundaryEncoderLease(std::move(leaseSession));
        return Mp4BoundaryEncoderAcquireOutcome::Acquired;
    } catch (...) {
        return Mp4BoundaryEncoderAcquireOutcome::Unavailable;
    }
}

void Mp4BoundaryEncoderPool::Replenish(
    const Mp4BoundaryEncoderKey& requestedKey,
    const std::uint64_t generation) noexcept {
    if (generation == 0) {
        return;
    }
    try {
        const std::optional<Mp4BoundaryEncoderKey> key = MakeKey(
            requestedKey.sourcePath,
            requestedKey.width,
            requestedKey.height,
            requestedKey.framesPerSecond,
            requestedKey.averageBitrate);
        if (!key.has_value()) {
            return;
        }
        const std::wstring identity =
            boundary_encoder_pool::SourceIdentity(key->sourcePath);
        {
            std::scoped_lock lock(impl_->mutex);
            const auto current = impl_->currentBySource.find(identity);
            const auto entry = impl_->entries.find(generation);
            if (current == impl_->currentBySource.end() ||
                current->second != generation || entry == impl_->entries.end() ||
                (entry->second.state != EntryState::Idle &&
                 entry->second.state != EntryState::Failed)) {
                return;
            }
            entry->second.key = *key;
            entry->second.state = EntryState::Queued;
        }
        impl_->changed.notify_all();
    } catch (...) {
    }
}

void Mp4BoundaryEncoderPool::ReleaseLease(
    const Mp4BoundaryEncoderKey& key,
    const std::uint64_t generation) noexcept {
    static_cast<void>(key);
    try {
        std::scoped_lock lock(impl_->mutex);
        const auto entry = impl_->entries.find(generation);
        if (entry == impl_->entries.end()) {
            return;
        }
        if (entry->second.state == EntryState::Leased) {
            entry->second.state = EntryState::Idle;
        } else if (entry->second.state == EntryState::Discarded) {
            impl_->entries.erase(entry);
        }
        impl_->changed.notify_all();
    } catch (...) {
    }
}

}  // namespace qrec::detail
