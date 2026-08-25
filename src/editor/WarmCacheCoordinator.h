#pragma once

#include "media/MediaExporter.h"

#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <thread>

namespace qrec {

// Owns one persistent warm-cache worker. Submissions are latest-wins: a new
// request replaces the pending request and asks the active export to stop.
// Callbacks run on the coordinator worker and must marshal UI work themselves.
class WarmCacheCoordinator final {
public:
    using ProgressCallback =
        std::function<void(std::uint64_t generation, const ExportProgress& progress)>;
    using CompletionCallback =
        std::function<void(std::uint64_t generation, MediaExportResult result)>;

    WarmCacheCoordinator() = default;
    ~WarmCacheCoordinator();

    WarmCacheCoordinator(const WarmCacheCoordinator&) = delete;
    WarmCacheCoordinator& operator=(const WarmCacheCoordinator&) = delete;

    // Starts the persistent worker. Returns false when already started or when
    // the worker cannot be created.
    [[nodiscard]] bool Start(
        ProgressCallback progress,
        CompletionCallback completed) noexcept;

    // Replaces the single pending request. If a task is active, cancellation is
    // requested without waiting for that task on the calling thread.
    [[nodiscard]] bool Submit(
        std::uint64_t generation,
        ExportRequest request) noexcept;

    // Cancels active and pending work without waiting.
    void Cancel() noexcept;

    // Stops the persistent worker and waits for it. This is the only method,
    // besides destruction, that joins the worker thread.
    void StopAndWait() noexcept;

private:
    struct PendingRequest final {
        std::uint64_t generation{};
        std::uint64_t submissionSerial{};
        ExportRequest request;
    };

    struct CallbackBundle final {
        ProgressCallback progress;
        CompletionCallback completed;
    };

    void WorkerLoop(std::stop_token workerStopToken) noexcept;
    void DeliverProgress(
        std::uint64_t generation,
        std::uint64_t submissionSerial,
        const ExportProgress& progress) noexcept;
    void DeliverCompletion(
        std::uint64_t generation,
        std::uint64_t submissionSerial,
        MediaExportResult result) noexcept;

    std::mutex mutex_;
    std::condition_variable_any workAvailable_;
    std::shared_ptr<const CallbackBundle> callbacks_;
    std::optional<PendingRequest> pending_;
    std::stop_source activeTaskStopSource_;
    std::jthread worker_;
    std::uint64_t nextSubmissionSerial_{};
    std::uint64_t latestSubmissionSerial_{};
    bool acceptingSubmissions_{};
    bool taskActive_{};
    bool stopping_{};
};

}  // namespace qrec
