#pragma once

#include "update/GitHubUpdateClient.h"

#include <functional>
#include <mutex>
#include <thread>

namespace qrec::update {

// Coordinates one check or download at a time on a private worker thread.
// Status callbacks execute on that worker; UI consumers must marshal them to
// their window thread (for example with PostMessage).
class UpdateCoordinator final {
public:
    using StatusCallback = std::function<void(const UpdateSnapshot&)>;

    UpdateCoordinator(
        SemanticVersion currentVersion,
        GitHubUpdateClientOptions clientOptions);
    ~UpdateCoordinator();

    UpdateCoordinator(const UpdateCoordinator&) = delete;
    UpdateCoordinator& operator=(const UpdateCoordinator&) = delete;
    UpdateCoordinator(UpdateCoordinator&&) = delete;
    UpdateCoordinator& operator=(UpdateCoordinator&&) = delete;

    void SetStatusCallback(StatusCallback callback) noexcept;

    // Returns false when another operation is running or a worker could not be
    // created. A successful start always publishes Checking asynchronously.
    [[nodiscard]] bool CheckForUpdates() noexcept;

    // Starts only after CheckForUpdates reported UpdateAvailable.
    [[nodiscard]] bool DownloadAvailableUpdate() noexcept;

    // Requests cancellation without joining or blocking the calling thread.
    void Cancel() noexcept;

    // Cancels and joins. Do not call this from inside StatusCallback.
    void CancelAndWait() noexcept;

    [[nodiscard]] UpdateSnapshot Snapshot() const noexcept;
    [[nodiscard]] bool IsBusy() const noexcept;

private:
    [[nodiscard]] bool BeginCheckWorker() noexcept;
    [[nodiscard]] bool BeginDownloadWorker(
        SemanticVersion latestVersion) noexcept;
    void RunCheck(std::stop_token stopToken) noexcept;
    void RunDownload(
        SemanticVersion latestVersion,
        std::stop_token stopToken) noexcept;
    void Publish(UpdateSnapshot snapshot) noexcept;
    void Finish(UpdateSnapshot snapshot) noexcept;

    SemanticVersion currentVersion_;
    GitHubUpdateClient client_;

    mutable std::mutex mutex_;
    std::mutex lifecycleMutex_;
    StatusCallback callback_;
    UpdateSnapshot snapshot_;
    std::jthread worker_;
    bool busy_{};
};

}  // namespace qrec::update
