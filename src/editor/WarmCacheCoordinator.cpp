#include "editor/WarmCacheCoordinator.h"

#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <new>
#include <string>
#include <utility>

namespace qrec {
namespace {

constexpr auto kProgressInterval = std::chrono::milliseconds(100);

MediaExportResult MakeUnhandledFailure(const bool allocationFailure) {
    MediaExportResult result{};
    result.success = false;
    result.cancelled = false;
    result.nativeError = allocationFailure ? E_OUTOFMEMORY : E_UNEXPECTED;
    result.errorMessage = allocationFailure
        ? L"后台预生成内存不足。"
        : L"后台预生成发生未知异常。";
    return result;
}

class ProgressLimiter final {
public:
    using Sink = std::function<void(const ExportProgress&)>;

    explicit ProgressLimiter(Sink sink)
        : sink_(std::move(sink)) {}

    void Push(const ExportProgress& source) {
        ExportProgress normalized = source;
        if (!std::isfinite(normalized.fraction)) {
            normalized.fraction = 0.0;
        }
        normalized.fraction = std::clamp(normalized.fraction, 0.0, 1.0);
        const int percentage = static_cast<int>(
            std::lround(normalized.fraction * 100.0));

        const bool duplicatesDelivered = delivered_ &&
            normalized.phase == deliveredPhase_ &&
            percentage == deliveredPercentage_;
        const bool duplicatesPending = pending_.has_value() &&
            normalized.phase == pending_->progress.phase &&
            percentage == pending_->percentage;
        if (!duplicatesDelivered && !duplicatesPending) {
            pending_ = PendingProgress{std::move(normalized), percentage};
        }

        const auto now = Clock::now();
        if (pending_.has_value() &&
            (!delivered_ || now - deliveredAt_ >= kProgressInterval)) {
            DispatchPending(now);
        }
    }

    void FlushIfDue() {
        if (!pending_.has_value()) {
            return;
        }
        const auto now = Clock::now();
        if (!delivered_ || now - deliveredAt_ >= kProgressInterval) {
            DispatchPending(now);
        }
    }

private:
    using Clock = std::chrono::steady_clock;

    struct PendingProgress final {
        ExportProgress progress;
        int percentage{};
    };

    void DispatchPending(const Clock::time_point now) {
        PendingProgress current = std::move(*pending_);
        pending_.reset();
        delivered_ = true;
        deliveredAt_ = now;
        deliveredPhase_ = current.progress.phase;
        deliveredPercentage_ = current.percentage;
        if (sink_) {
            sink_(current.progress);
        }
    }

    Sink sink_;
    std::optional<PendingProgress> pending_;
    Clock::time_point deliveredAt_{};
    std::wstring deliveredPhase_;
    int deliveredPercentage_{-1};
    bool delivered_{};
};

}  // namespace

WarmCacheCoordinator::~WarmCacheCoordinator() {
    StopAndWait();
}

bool WarmCacheCoordinator::Start(
    ProgressCallback progress,
    CompletionCallback completed) noexcept {
    try {
        auto callbacks = std::make_shared<CallbackBundle>();
        callbacks->progress = std::move(progress);
        callbacks->completed = std::move(completed);

        std::scoped_lock lock(mutex_);
        if (worker_.joinable()) {
            return false;
        }

        callbacks_ = std::move(callbacks);
        pending_.reset();
        activeTaskStopSource_ = std::stop_source{};
        acceptingSubmissions_ = true;
        taskActive_ = false;
        stopping_ = false;
        worker_ = std::jthread(
            [this](const std::stop_token stopToken) noexcept {
                WorkerLoop(stopToken);
            });
        return true;
    } catch (...) {
        std::scoped_lock lock(mutex_);
        callbacks_.reset();
        pending_.reset();
        acceptingSubmissions_ = false;
        taskActive_ = false;
        stopping_ = false;
        return false;
    }
}

bool WarmCacheCoordinator::Submit(
    const std::uint64_t generation,
    ExportRequest request) noexcept {
    try {
        {
            std::scoped_lock lock(mutex_);
            if (!acceptingSubmissions_ || stopping_ || !worker_.joinable()) {
                return false;
            }

            const std::uint64_t submissionSerial = ++nextSubmissionSerial_;
            latestSubmissionSerial_ = submissionSerial;
            pending_ = PendingRequest{
                generation,
                submissionSerial,
                std::move(request)};
            if (taskActive_) {
                activeTaskStopSource_.request_stop();
            }
        }
        workAvailable_.notify_one();
        return true;
    } catch (...) {
        return false;
    }
}

void WarmCacheCoordinator::Cancel() noexcept {
    try {
        {
            std::scoped_lock lock(mutex_);
            pending_.reset();
            latestSubmissionSerial_ = ++nextSubmissionSerial_;
            if (taskActive_) {
                activeTaskStopSource_.request_stop();
            }
        }
        workAvailable_.notify_one();
    } catch (...) {
    }
}

void WarmCacheCoordinator::StopAndWait() noexcept {
    std::jthread workerToJoin;
    try {
        {
            std::scoped_lock lock(mutex_);
            acceptingSubmissions_ = false;
            stopping_ = true;
            pending_.reset();
            latestSubmissionSerial_ = ++nextSubmissionSerial_;
            activeTaskStopSource_.request_stop();
            if (worker_.joinable()) {
                worker_.request_stop();
                if (worker_.get_id() == std::this_thread::get_id()) {
                    workAvailable_.notify_all();
                    return;
                }
                workerToJoin = std::move(worker_);
            }
        }
        workAvailable_.notify_all();
        if (workerToJoin.joinable()) {
            workerToJoin.join();
        }
    } catch (...) {
        // A local jthread still owns any moved worker and joins during cleanup.
    }

    try {
        std::scoped_lock lock(mutex_);
        callbacks_.reset();
        pending_.reset();
        activeTaskStopSource_ = std::stop_source{};
        taskActive_ = false;
        stopping_ = false;
    } catch (...) {
    }
}

void WarmCacheCoordinator::WorkerLoop(
    const std::stop_token workerStopToken) noexcept {
    static_cast<void>(::SetThreadPriority(
        ::GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL));

    for (;;) {
        std::optional<PendingRequest> task;
        std::stop_token taskStopToken;
        try {
            std::unique_lock lock(mutex_);
            workAvailable_.wait(
                lock,
                workerStopToken,
                [this]() noexcept {
                    return stopping_ || pending_.has_value();
                });
            if (workerStopToken.stop_requested() || stopping_) {
                break;
            }
            if (!pending_.has_value()) {
                continue;
            }

            task = std::move(pending_);
            pending_.reset();
            activeTaskStopSource_ = std::stop_source{};
            taskStopToken = activeTaskStopSource_.get_token();
            taskActive_ = true;
        } catch (...) {
            break;
        }

        MediaExportResult result{};
        try {
            ProgressLimiter limiter(
                [this,
                 generation = task->generation,
                 submissionSerial = task->submissionSerial](
                    const ExportProgress& progress) noexcept {
                    DeliverProgress(generation, submissionSerial, progress);
                });
            result = MediaExporter::WarmCache(
                task->request,
                [&limiter](const ExportProgress& progress) {
                    limiter.Push(progress);
                },
                taskStopToken);
            limiter.FlushIfDue();
        } catch (const std::bad_alloc&) {
            result = MakeUnhandledFailure(true);
        } catch (...) {
            result = MakeUnhandledFailure(false);
        }

        DeliverCompletion(
            task->generation,
            task->submissionSerial,
            std::move(result));
    }

    try {
        std::scoped_lock lock(mutex_);
        acceptingSubmissions_ = false;
        taskActive_ = false;
    } catch (...) {
    }
}

void WarmCacheCoordinator::DeliverProgress(
    const std::uint64_t generation,
    const std::uint64_t submissionSerial,
    const ExportProgress& progress) noexcept {
    std::shared_ptr<const CallbackBundle> callbacks;
    try {
        {
            std::scoped_lock lock(mutex_);
            if (stopping_ || submissionSerial != latestSubmissionSerial_) {
                return;
            }
            callbacks = callbacks_;
        }
        if (callbacks && callbacks->progress) {
            callbacks->progress(generation, progress);
        }
    } catch (...) {
        // Consumer callback failures must never terminate the worker.
    }
}

void WarmCacheCoordinator::DeliverCompletion(
    const std::uint64_t generation,
    const std::uint64_t submissionSerial,
    MediaExportResult result) noexcept {
    std::shared_ptr<const CallbackBundle> callbacks;
    bool deliver = false;
    try {
        {
            std::scoped_lock lock(mutex_);
            taskActive_ = false;
            deliver = !stopping_ &&
                submissionSerial == latestSubmissionSerial_ &&
                !pending_.has_value();
            if (deliver) {
                callbacks = callbacks_;
            }
        }
        if (callbacks && callbacks->completed) {
            callbacks->completed(generation, std::move(result));
        }
    } catch (...) {
        // Consumer callback failures must never terminate the worker.
    }
}

}  // namespace qrec
