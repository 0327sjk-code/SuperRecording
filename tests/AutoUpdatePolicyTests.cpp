// Deterministic time/activity tests; never change the Windows clock or show UI.
#include "update/DailyUpdatePolicy.h"
#include "update/DailyUpdateStateStore.h"
#include "app/UpdateActivity.h"

#include <windows.h>
#include <cstdio>
#include <filesystem>
#include <stdexcept>

namespace {
using namespace qrec::update;
using Action = AutomaticUpdateAction;
constexpr std::uint32_t Today = 20261008;
constexpr std::uint32_t Tomorrow = 20261009;
constexpr auto Eleven = DailyUpdatePolicy::CheckTimeSeconds;

void Expect(const bool value, const char* name) {
    if (!value) throw std::runtime_error(name);
    std::printf("PASS %s\n", name);
}

Action Poll(DailyUpdatePolicy& policy, const std::uint32_t date,
    const std::uint32_t second, const std::uint64_t tick,
    const bool idle = true, const UpdatePhase phase = UpdatePhase::Idle,
    const bool workerBusy = false) {
    return policy.NextAction({date, second, tick}, idle, phase, workerBusy);
}

void TestDailyTrigger() {
    DailyUpdatePolicy policy;
    Expect(Poll(policy, Today, Eleven - 10, 0) == Action::None, "startup before 11 is quiet");
    Expect(Poll(policy, Today, Eleven - 1, 9'000) == Action::None, "10:59:59 does not check");
    Expect(Poll(policy, Today, Eleven, 10'000) == Action::Check, "11:00 triggers check");
    policy.CheckStarted();
    Expect(Poll(policy, Today, Eleven, 10'001, true, UpdatePhase::Checking, true) == Action::None,
        "no overlapping network workers");
    policy.Completed({Today, Eleven, 10'002});
    Expect(Poll(policy, Today, Eleven + 60, 70'000, true, UpdatePhase::UpToDate) == Action::None,
        "successful daily check does not repeat");
    DailyUpdatePolicy restarted(policy.CompletedDate());
    static_cast<void>(Poll(restarted, Today, Eleven + 100, 0));
    Expect(Poll(restarted, Today, Eleven + 103, 3'000) == Action::None,
        "restart on same day does not re-check");
    Expect(Poll(policy, Tomorrow, Eleven - 1, 90'000) == Action::None, "next day waits for 11");
    Expect(Poll(policy, Tomorrow, Eleven, 91'000) == Action::Check, "next day checks again");
}

void TestMissedAndBusy() {
    DailyUpdatePolicy policy;
    Expect(Poll(policy, Today, Eleven + 3600, 0) == Action::None, "late startup has idle grace");
    Expect(Poll(policy, Today, Eleven + 3603, 3'000) == Action::Check, "late startup catches missed check");
    for (const auto phase : {UpdatePhase::Idle, UpdatePhase::UpdateAvailable, UpdatePhase::ReadyToInstall}) {
        Expect(Poll(policy, Today, Eleven + 4000, 5'000, false, phase) == Action::None,
            "busy blocks check, download and install");
        Expect(Poll(policy, Today, Eleven + 4001, 6'000, true, phase) == Action::None,
            "closing editor starts a new grace interval");
        Expect(Poll(policy, Today, Eleven + 4003, 8'999, true, phase) == Action::None,
            "no install during short idle gaps");
        const auto expected = phase == UpdatePhase::Idle ? Action::Check :
            phase == UpdatePhase::UpdateAvailable ? Action::Download : Action::Install;
        Expect(Poll(policy, Today, Eleven + 4004, 9'000, true, phase) == expected,
            "idle resumes pending work without a click");
    }
    policy.ObserveActivity(false, 10'000);
    Expect(Poll(policy, Tomorrow, 60, 11'000, true, UpdatePhase::ReadyToInstall) == Action::None,
        "resume resets idle grace");
    Expect(Poll(policy, Tomorrow, 63, 14'000, true, UpdatePhase::ReadyToInstall) == Action::Install,
        "downloaded update can install after midnight before 11");
}

void TestFailureAndManual() {
    DailyUpdatePolicy policy;
    static_cast<void>(Poll(policy, Today, Eleven, 0));
    policy.RetryLater({Today, Eleven, 3'000});
    Expect(Poll(policy, Today, Eleven + 899, 902'999, true, UpdatePhase::Failed) == Action::None,
        "offline failure is rate limited for 15 minutes");
    Expect(Poll(policy, Today, Eleven + 900, 903'000, true, UpdatePhase::Failed) == Action::Check,
        "network failure retries automatically");
    policy.RetryLater({Today, Eleven, 904'000});
    policy.RequestManualCheck();
    Expect(Poll(policy, Today, Eleven + 901, 904'001, true, UpdatePhase::Failed) == Action::Check,
        "manual check can override retry cooldown");
    policy.ObserveActivity(false, 904'002);
    policy.RequestManualCheck();
    Expect(Poll(policy, Today, Eleven + 902, 904'003, false, UpdatePhase::Cancelled) == Action::None,
        "cancelled download remains paused throughout recording and editing");
    static_cast<void>(Poll(policy, Today, Eleven + 903, 905'000, true, UpdatePhase::Cancelled));
    Expect(Poll(policy, Today, Eleven + 906, 908'000, true, UpdatePhase::Cancelled) == Action::Check,
        "activity cancellation resumes after idle grace without failure cooldown");
    policy.Completed({Today, Eleven, 904'010});
    policy.RetryLater({Today, Eleven, 904'020});
    Expect(Poll(policy, Today, Eleven, 904'021, true, UpdatePhase::ReadyToInstall) == Action::None,
        "failed bootstrap is not retried every timer tick");
    Expect(Poll(policy, Today, Eleven, 1'804'020, true, UpdatePhase::ReadyToInstall) == Action::Install,
        "failed bootstrap retries after cooldown");

    DailyUpdatePolicy morning;
    morning.RequestManualCheck();
    static_cast<void>(Poll(morning, Today, 3600, 0));
    Expect(Poll(morning, Today, 3603, 3'000) == Action::Check, "manual morning check works");
    morning.CheckStarted();
    morning.Completed({Today, 3604, 4'000});
    Expect(Poll(morning, Today, Eleven, 40'000, true, UpdatePhase::UpToDate) == Action::Check,
        "morning check does not suppress 11:00 schedule");
    Expect(!DailyUpdatePolicy::IsValidDate(20260230), "invalid persisted calendar date rejected");
    Expect(DailyUpdatePolicy::IsValidDate(20280229), "leap day supported");
    DailyUpdatePolicy clockChanged(Tomorrow);
    static_cast<void>(Poll(clockChanged, Today, Eleven, 0));
    Expect(Poll(clockChanged, Today, Eleven + 3, 3'000) == Action::Check,
        "clock moved backward does not suppress updates for days");
}
}  // namespace

void RunAutoUpdatePolicyTests(const std::filesystem::path& root) {
    TestDailyTrigger(); TestMissedAndBusy(); TestFailureAndManual();
    for (const auto state : {qrec::RecordingState::Selecting, qrec::RecordingState::Recording,
         qrec::RecordingState::Paused, qrec::RecordingState::Finalizing, qrec::RecordingState::Editing}) {
        Expect(!qrec::UpdateActivity{state, false, false, false, true}.IsIdle(),
            "every active recording/editor state blocks final restart gate");
    }
    using State = qrec::RecordingState;
    Expect(!qrec::UpdateActivity{State::Idle, true, false, false, true}.IsIdle(),
        "editor ownership blocks restart even if hidden or state is stale");
    Expect(!qrec::UpdateActivity{State::Idle, false, true, false, true}.IsIdle(),
        "settings dialog and tray menu block restart");
    Expect(!qrec::UpdateActivity{State::Idle, false, false, true, true}.IsIdle(),
        "shutdown cannot initiate a competing update");
    Expect(!qrec::UpdateActivity{State::Idle, false, false, false, false}.IsIdle(),
        "unavailable or modal-disabled owner blocks restart");
    Expect(qrec::UpdateActivity{State::Idle, false, false, false, true}.IsIdle(),
        "only idle tray application permits install hand-off");
    const auto path = root / L"isolated-scheduler.ini";
    qrec::update::DailyUpdateStateStore store(path);
    Expect(store.LoadCompletedDate() == 0, "missing scheduler state is safe");
    Expect(store.SaveCompletedDate(Today), "completed date persisted separately");
    qrec::update::DailyUpdateStateStore reopened(path);
    Expect(reopened.LoadCompletedDate() == Today, "another process instance sees completed date");
    Expect(!store.SaveCompletedDate(20261308), "invalid dates cannot be persisted");
    Expect(reopened.LoadCompletedDate() == Today, "invalid write preserves prior state");
    Expect(::DeleteFileW(path.c_str()) != FALSE, "remove only owned test scheduler file");
}
