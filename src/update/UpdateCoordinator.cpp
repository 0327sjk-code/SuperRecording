#include "update/UpdateCoordinator.h"

#include <windows.h>

#include <exception>
#include <new>
#include <optional>
#include <utility>

namespace qrec::update {
namespace {

UpdateFailure MakeCoordinatorFailure(
    const UpdateErrorCode code,
    std::wstring message) {
    UpdateFailure failure{};
    failure.code = code;
    failure.message = std::move(message);
    return failure;
}

UpdateSnapshot MakeTerminalFailure(
    const SemanticVersion& currentVersion,
    std::optional<SemanticVersion> latestVersion,
    UpdateFailure failure) {
    UpdateSnapshot snapshot{};
    snapshot.phase = failure.code == UpdateErrorCode::Cancelled
        ? UpdatePhase::Cancelled
        : UpdatePhase::Failed;
    snapshot.currentVersion = currentVersion;
    snapshot.latestVersion = std::move(latestVersion);
    snapshot.failure = std::move(failure);
    return snapshot;
}

}  // namespace

UpdateCoordinator::UpdateCoordinator(
    const SemanticVersion currentVersion,
    GitHubUpdateClientOptions clientOptions)
    : currentVersion_(currentVersion),
      client_(std::move(clientOptions)) {
    snapshot_.currentVersion = currentVersion_;
}

UpdateCoordinator::~UpdateCoordinator() {
    CancelAndWait();
}

void UpdateCoordinator::SetStatusCallback(StatusCallback callback) noexcept {
    try {
        std::scoped_lock lock(mutex_);
        callback_ = std::move(callback);
    } catch (...) {
        // A callback update must not destabilize the tray process.
    }
}

bool UpdateCoordinator::CheckForUpdates() noexcept {
    try {
        std::scoped_lock lifecycleLock(lifecycleMutex_);
        return BeginCheckWorker();
    } catch (...) {
        return false;
    }
}

bool UpdateCoordinator::DownloadAvailableUpdate() noexcept {
    try {
        std::scoped_lock lifecycleLock(lifecycleMutex_);
        std::optional<SemanticVersion> latestVersion;
        {
            std::scoped_lock stateLock(mutex_);
            if (busy_ || !snapshot_.latestVersion.has_value() ||
                *snapshot_.latestVersion <= currentVersion_) {
                return false;
            }
            latestVersion = snapshot_.latestVersion;
        }
        return BeginDownloadWorker(*latestVersion);
    } catch (...) {
        return false;
    }
}

void UpdateCoordinator::Cancel() noexcept {
    try {
        std::scoped_lock lifecycleLock(lifecycleMutex_);
        if (worker_.joinable()) {
            worker_.request_stop();
        }
        client_.Cancel();
    } catch (...) {
    }
}

void UpdateCoordinator::CancelAndWait() noexcept {
    std::jthread workerToJoin;
    try {
        {
            std::scoped_lock lifecycleLock(lifecycleMutex_);
            if (worker_.joinable()) {
                worker_.request_stop();
                client_.Cancel();
                if (worker_.get_id() == std::this_thread::get_id()) {
                    return;
                }
                workerToJoin = std::move(worker_);
            }
        }
        if (workerToJoin.joinable()) {
            workerToJoin.join();
        }
    } catch (...) {
        // A local jthread still joins during stack cleanup when it owns work.
    }
}

UpdateSnapshot UpdateCoordinator::Snapshot() const noexcept {
    try {
        std::scoped_lock lock(mutex_);
        return snapshot_;
    } catch (...) {
        UpdateSnapshot fallback{};
        fallback.phase = UpdatePhase::Failed;
        fallback.currentVersion = currentVersion_;
        fallback.failure.code = UpdateErrorCode::Unexpected;
        return fallback;
    }
}

bool UpdateCoordinator::IsBusy() const noexcept {
    try {
        std::scoped_lock lock(mutex_);
        return busy_;
    } catch (...) {
        return false;
    }
}

bool UpdateCoordinator::BeginCheckWorker() noexcept {
    try {
        {
            std::scoped_lock stateLock(mutex_);
            if (busy_ ||
                (worker_.joinable() &&
                 worker_.get_id() == std::this_thread::get_id())) {
                return false;
            }
        }
        if (worker_.joinable()) {
            worker_.join();
        }
        {
            std::scoped_lock stateLock(mutex_);
            busy_ = true;
        }
        try {
            worker_ = std::jthread(
                [this](const std::stop_token stopToken) noexcept {
                    static_cast<void>(::SetThreadPriority(
                        ::GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL));
                    RunCheck(stopToken);
                });
        } catch (const std::bad_alloc&) {
            Finish(MakeTerminalFailure(
                currentVersion_,
                std::nullopt,
                MakeCoordinatorFailure(
                    UpdateErrorCode::OutOfMemory,
                    L"Not enough memory to create the update worker.")));
            return false;
        } catch (...) {
            Finish(MakeTerminalFailure(
                currentVersion_,
                std::nullopt,
                MakeCoordinatorFailure(
                    UpdateErrorCode::WorkerStartFailed,
                    L"Could not create the update worker.")));
            return false;
        }
        return true;
    } catch (...) {
        try {
            std::scoped_lock stateLock(mutex_);
            busy_ = false;
        } catch (...) {
        }
        return false;
    }
}

bool UpdateCoordinator::BeginDownloadWorker(
    const SemanticVersion latestVersion) noexcept {
    try {
        {
            std::scoped_lock stateLock(mutex_);
            if (busy_ ||
                (worker_.joinable() &&
                 worker_.get_id() == std::this_thread::get_id())) {
                return false;
            }
        }
        if (worker_.joinable()) {
            worker_.join();
        }
        {
            std::scoped_lock stateLock(mutex_);
            busy_ = true;
        }
        try {
            worker_ = std::jthread(
                [this, latestVersion](
                    const std::stop_token stopToken) noexcept {
                    static_cast<void>(::SetThreadPriority(
                        ::GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL));
                    RunDownload(latestVersion, stopToken);
                });
        } catch (const std::bad_alloc&) {
            Finish(MakeTerminalFailure(
                currentVersion_,
                latestVersion,
                MakeCoordinatorFailure(
                    UpdateErrorCode::OutOfMemory,
                    L"Not enough memory to create the update worker.")));
            return false;
        } catch (...) {
            Finish(MakeTerminalFailure(
                currentVersion_,
                latestVersion,
                MakeCoordinatorFailure(
                    UpdateErrorCode::WorkerStartFailed,
                    L"Could not create the update worker.")));
            return false;
        }
        return true;
    } catch (...) {
        try {
            std::scoped_lock stateLock(mutex_);
            busy_ = false;
        } catch (...) {
        }
        return false;
    }
}

void UpdateCoordinator::RunCheck(
    const std::stop_token stopToken) noexcept {
    try {
        UpdateSnapshot checking{};
        checking.phase = UpdatePhase::Checking;
        checking.currentVersion = currentVersion_;
        Publish(std::move(checking));

        VersionFetchResult result = client_.FetchLatestVersion(stopToken);
        if (!result.Succeeded()) {
            Finish(MakeTerminalFailure(
                currentVersion_,
                std::nullopt,
                std::move(result.failure)));
            return;
        }

        UpdateSnapshot completed{};
        completed.currentVersion = currentVersion_;
        completed.latestVersion = result.version;
        completed.phase = *result.version > currentVersion_
            ? UpdatePhase::UpdateAvailable
            : UpdatePhase::UpToDate;
        Finish(std::move(completed));
    } catch (const std::bad_alloc&) {
        Finish(MakeTerminalFailure(
            currentVersion_,
            std::nullopt,
            MakeCoordinatorFailure(
                UpdateErrorCode::OutOfMemory,
                L"Not enough memory to check for updates.")));
    } catch (...) {
        Finish(MakeTerminalFailure(
            currentVersion_,
            std::nullopt,
            MakeCoordinatorFailure(
                UpdateErrorCode::Unexpected,
                L"Unexpected update-check coordinator failure.")));
    }
}

void UpdateCoordinator::RunDownload(
    const SemanticVersion latestVersion,
    const std::stop_token stopToken) noexcept {
    try {
        UpdateSnapshot downloading{};
        downloading.phase = UpdatePhase::Downloading;
        downloading.currentVersion = currentVersion_;
        downloading.latestVersion = latestVersion;
        Publish(std::move(downloading));

        ExecutableDownloadResult result = client_.DownloadExecutable(
            latestVersion,
            [this, latestVersion](
                const std::uint64_t downloadedBytes,
                const std::optional<std::uint64_t> totalBytes) noexcept {
                UpdateSnapshot progress{};
                progress.phase = UpdatePhase::Downloading;
                progress.currentVersion = currentVersion_;
                progress.latestVersion = latestVersion;
                progress.downloadedBytes = downloadedBytes;
                progress.totalBytes = totalBytes;
                Publish(std::move(progress));
            },
            stopToken);
        if (!result.Succeeded()) {
            Finish(MakeTerminalFailure(
                currentVersion_,
                latestVersion,
                std::move(result.failure)));
            return;
        }

        UpdateSnapshot ready{};
        ready.phase = UpdatePhase::ReadyToInstall;
        ready.currentVersion = currentVersion_;
        ready.latestVersion = latestVersion;
        ready.downloadedFile = std::move(result.filePath);
        ready.downloadedBytes = result.downloadedBytes;
        ready.totalBytes = result.downloadedBytes;
        Finish(std::move(ready));
    } catch (const std::bad_alloc&) {
        Finish(MakeTerminalFailure(
            currentVersion_,
            latestVersion,
            MakeCoordinatorFailure(
                UpdateErrorCode::OutOfMemory,
                L"Not enough memory to coordinate the update download.")));
    } catch (...) {
        Finish(MakeTerminalFailure(
            currentVersion_,
            latestVersion,
            MakeCoordinatorFailure(
                UpdateErrorCode::Unexpected,
                L"Unexpected update-download coordinator failure.")));
    }
}

void UpdateCoordinator::Publish(UpdateSnapshot snapshot) noexcept {
    try {
        StatusCallback callback;
        UpdateSnapshot delivered;
        {
            std::scoped_lock lock(mutex_);
            snapshot_ = std::move(snapshot);
            callback = callback_;
            delivered = snapshot_;
        }
        if (callback) {
            try {
                callback(delivered);
            } catch (...) {
                // Consumer callback failures must not terminate the worker.
            }
        }
    } catch (...) {
        // A state-copy allocation failure is reported by the worker boundary.
    }
}

void UpdateCoordinator::Finish(UpdateSnapshot snapshot) noexcept {
    try {
        StatusCallback callback;
        UpdateSnapshot delivered;
        {
            std::scoped_lock lock(mutex_);
            snapshot_ = std::move(snapshot);
            busy_ = false;
            callback = callback_;
            delivered = snapshot_;
        }
        if (callback) {
            try {
                callback(delivered);
            } catch (...) {
                // Consumer callback failures must not terminate the worker.
            }
        }
    } catch (...) {
        try {
            std::scoped_lock lock(mutex_);
            busy_ = false;
        } catch (...) {
        }
    }
}

}  // namespace qrec::update
