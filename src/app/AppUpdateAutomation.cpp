// Background update orchestration. All methods run on the application's UI thread.
#include "app/AppUpdateController.h"
#include "update/DailyUpdateStateStore.h"
#include "update/UpdateCoordinator.h"

#include <shellapi.h>

namespace qrec {

update::UpdateClock AppUpdateController::ClockNow() noexcept {
    SYSTEMTIME local{};
    ::GetLocalTime(&local);
    return {static_cast<std::uint32_t>(local.wYear) * 10'000u +
                static_cast<std::uint32_t>(local.wMonth) * 100u + local.wDay,
            static_cast<std::uint32_t>(local.wHour) * 3600u +
                static_cast<std::uint32_t>(local.wMinute) * 60u + local.wSecond,
            ::GetTickCount64()};
}

bool AppUpdateController::ApplicationIdle() const noexcept {
    try { return !shuttingDown_ && isIdle_ && isIdle_(); }
    catch (...) { return false; }  // Unknown activity is never safe to interrupt.
}

void AppUpdateController::ObserveBusy() noexcept {
    schedule_.ObserveActivity(false, ::GetTickCount64());
    if (coordinator_ && coordinator_->IsBusy() && !activityPaused_) {
        activityPaused_ = true;
        coordinator_->Cancel();
        LogInfo(L"用户开始操作录屏软件，已暂停后台更新；空闲后自动继续。");
    }
}

void AppUpdateController::Poll() noexcept {
    if (!coordinator_ || shuttingDown_) return;
    try {
        const bool idle = ApplicationIdle();
        if (!idle) { ObserveBusy(); return; }
        const auto snapshot = coordinator_->Snapshot();
        // Terminal messages must be accounted for (date/retry) before another job starts.
        if ((snapshot.phase == update::UpdatePhase::Failed &&
             terminalNotification_ != TerminalNotification::Failed) ||
            (snapshot.phase == update::UpdatePhase::UpToDate &&
             terminalNotification_ != TerminalNotification::UpToDate) ||
            (snapshot.phase == update::UpdatePhase::Cancelled &&
             terminalNotification_ != TerminalNotification::Cancelled)) return;
        const auto action = schedule_.NextAction(
            ClockNow(), idle, snapshot.phase, coordinator_->IsBusy());
        switch (action) {
        case update::AutomaticUpdateAction::Check: StartCheck(); break;
        case update::AutomaticUpdateAction::Download: StartDownload(); break;
        case update::AutomaticUpdateAction::Install: ApplyDownloadedUpdate(); break;
        case update::AutomaticUpdateAction::None: break;
        }
    } catch (...) {
        schedule_.RetryLater(ClockNow());
        LogError(L"自动更新调度异常，将在 15 分钟后重试；录屏不受影响。");
    }
}

void AppUpdateController::StartCheck() noexcept {
    terminalNotification_ = TerminalNotification::None;
    activityPaused_ = false;
    downloadRequested_ = false;
    readyExecutable_.clear();
    schedule_.CheckStarted();
    LogInfo(L"开始检查 GitHub 更新（每天本地时间 11:00，错过后空闲补查）。");
    if (!coordinator_->CheckForUpdates()) {
        schedule_.RetryLater(ClockNow());
        LogError(L"更新检查未启动，将在 15 分钟后重试。");
    }
}

void AppUpdateController::StartDownload() noexcept {
    if (downloadRequested_) return;
    downloadRequested_ = true;
    activityPaused_ = false;
    LogInfo(L"发现新版本，空闲时开始后台下载；安装前将再次确认空闲状态。");
    Notify(L"发现新版本", L"正在后台下载，完成后将在空闲时自动更新。", NIIF_INFO);
    if (!coordinator_->DownloadAvailableUpdate()) {
        downloadRequested_ = false;
        schedule_.RetryLater(ClockNow());
        LogError(L"更新下载未启动，将在 15 分钟后重试。");
    }
}

void AppUpdateController::PersistCompletedDate() noexcept {
    const auto date = schedule_.CompletedDate();
    if (date != 0 && scheduleStore_ && !scheduleStore_->SaveCompletedDate(date))
        LogError(L"自动更新日期保存失败；本次运行仍会避免重复检查。");
}

}  // namespace qrec
