// Headless tests of the production controller's asynchronous terminal-message handling.
// The compile-time probe feeds snapshots; it neither contacts GitHub nor adds a tray icon.
#include "app/AppUpdateController.h"
#include "update/DailyUpdateStateStore.h"
#include "update/UpdateCoordinator.h"

#include <cstdio>
#include <filesystem>
#include <stdexcept>

namespace qrec {
struct UpdateControllerProbe final {
    static void Expect(const bool value, const char* name) {
        if (!value) throw std::runtime_error(name);
        std::printf("PASS %s\n", name);
    }
    static void Initialize(AppUpdateController& app, const std::filesystem::path& state,
        bool& idle, AppUpdateController::ApplyRequestedCallback callback) {
        app.coordinator_ = std::make_unique<update::UpdateCoordinator>(
            update::SemanticVersion::Parse(L"1.7.0").value(), update::GitHubUpdateClientOptions{});
        app.scheduleStore_ = std::make_unique<update::DailyUpdateStateStore>(state);
        app.isIdle_ = [&idle]() { return idle; };
        app.applyRequested_ = std::move(callback);
    }
    static void Publish(AppUpdateController& app, const update::UpdatePhase phase,
        const std::filesystem::path& executable = {}) {
        update::UpdateSnapshot snapshot;
        snapshot.phase = phase;
        snapshot.downloadedFile = executable;
        app.coordinator_->Finish(std::move(snapshot));
    }
    static void ElapseIdleGrace(AppUpdateController& app) {
        const auto now = AppUpdateController::ClockNow().monotonicMilliseconds;
        app.schedule_.ObserveActivity(false, now);
        app.schedule_.ObserveActivity(true, now - update::DailyUpdatePolicy::IdleGraceMilliseconds);
    }
    static void Run(const std::filesystem::path& root, const std::filesystem::path& executable) {
        bool idle = false;
        int calls = 0;
        AppUpdateController app;
        Initialize(app, root / L"controller-state.ini", idle,
            [&](std::filesystem::path candidate, const bool silent) {
                Expect(candidate == executable && silent, "automatic install receives correct file and silent mode");
                ++calls; return false;
            });
        Publish(app, update::UpdatePhase::ReadyToInstall, executable);
        app.HandleStatusChanged();
        app.Poll();
        Expect(calls == 0, "download-ready notifications cannot restart a busy editor");
        idle = true;
        app.Poll();
        Expect(calls == 0, "controller waits for stable idle after editor closes");
        ElapseIdleGrace(app);
        app.Poll();
        Expect(calls == 1, "controller automatically applies without menu interaction");
        app.HandleStatusChanged(); app.Poll();
        Expect(calls == 1, "duplicate ready messages cannot bypass failed-install cooldown");
        app.Shutdown(); app.Poll();
        Expect(calls == 1, "shutdown stops all automatic work");

        AppUpdateController cancelled;
        Initialize(cancelled, root / L"cancel-state.ini", idle, {});
        Publish(cancelled, update::UpdatePhase::Downloading);
        cancelled.coordinator_->busy_ = true;  // No worker thread exists in this fixture.
        idle = false;
        cancelled.Poll();
        Expect(cancelled.activityPaused_, "beginning recording cancels an in-flight background download");
        Publish(cancelled, update::UpdatePhase::Cancelled);
        cancelled.HandleStatusChanged(); cancelled.HandleStatusChanged();
        idle = true;
        ElapseIdleGrace(cancelled);
        const auto resumed = cancelled.schedule_.NextAction(
            AppUpdateController::ClockNow(), true, update::UpdatePhase::Cancelled, false);
        Expect(resumed == update::AutomaticUpdateAction::Check,
            "duplicate cancelled messages preserve immediate idle resume, not 15-minute retry");
        cancelled.Shutdown();

        AppUpdateController healthy;
        Initialize(healthy, root / L"healthy-state.ini", idle,
            [&](std::filesystem::path, bool) { ++calls; healthy.Shutdown(); return true; });
        Publish(healthy, update::UpdatePhase::ReadyToInstall, executable);
        ElapseIdleGrace(healthy);
        healthy.HandleStatusChanged();
        Expect(calls == 2 && healthy.shuttingDown_,
            "successful hand-off safely tears down controller during its callback");

        AppUpdateController failure;
        Initialize(failure, root / L"failure-state.ini", idle, {});
        Publish(failure, update::UpdatePhase::Failed);
        failure.HandleStatusChanged(); failure.HandleStatusChanged();
        ElapseIdleGrace(failure);
        Expect(failure.schedule_.NextAction(AppUpdateController::ClockNow(), true,
            update::UpdatePhase::Failed, false) == update::AutomaticUpdateAction::None,
            "production controller rate-limits background network failures");
        failure.Shutdown();
    }
};
}  // namespace qrec

void RunAutoUpdateControllerTests(const std::filesystem::path& root,
    const std::filesystem::path& executable) {
    qrec::UpdateControllerProbe::Run(root, executable);
}
