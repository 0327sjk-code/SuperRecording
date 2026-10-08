#include "app/AppUpdateController.h"

#include "app/TrayIcon.h"
#include "common/AppMessages.h"
#include "common/Logger.h"
#include "common/ProductInfo.h"
#include "common/Win32Helpers.h"
#include "update/DailyUpdateStateStore.h"
#include "update/UpdateCoordinator.h"

#include <shellapi.h>

#include <format>
#include <optional>
#include <string>
#include <utility>

namespace qrec {
namespace {

std::wstring UpdateFailureText(const update::UpdateFailure& failure) {
    switch (failure.code) {
    case update::UpdateErrorCode::InvalidConfiguration:
    case update::UpdateErrorCode::InvalidUrl:
        return L"GitHub 更新地址配置无效。";
    case update::UpdateErrorCode::Network:
        return L"无法连接 GitHub，请检查网络后重试。";
    case update::UpdateErrorCode::HttpStatus:
        return failure.httpStatus == 0
            ? L"GitHub 暂时无法提供更新文件。"
            : std::format(L"GitHub 返回 HTTP {}，请稍后重试。", failure.httpStatus);
    case update::UpdateErrorCode::ResponseTooLarge:
        return L"GitHub 返回的更新文件大小异常。";
    case update::UpdateErrorCode::InvalidVersion:
        return L"GitHub Release 中的版本信息无效。";
    case update::UpdateErrorCode::FileSystem:
        return L"无法写入系统更新缓存目录。";
    case update::UpdateErrorCode::InvalidExecutable:
        return L"下载的 SuperRecording.exe 无效，请重新检查更新。";
    case update::UpdateErrorCode::NoUpdateAvailable:
        return L"当前没有可下载的新版本。";
    case update::UpdateErrorCode::OutOfMemory:
        return L"系统内存不足，无法完成在线更新。";
    case update::UpdateErrorCode::Cancelled:
        return L"在线更新已取消。";
    case update::UpdateErrorCode::WorkerStartFailed:
    case update::UpdateErrorCode::Unexpected:
    case update::UpdateErrorCode::None:
    default:
        return L"GitHub 更新检查或下载失败，请稍后重试。";
    }
}

}  // namespace

AppUpdateController::AppUpdateController() = default;

AppUpdateController::~AppUpdateController() {
    Shutdown();
}

bool AppUpdateController::Initialize(
    const HWND messageWindow,
    TrayIcon& trayIcon,
    Logger& logger,
    ApplyRequestedCallback applyRequested,
    IsIdleCallback isIdle) noexcept {
    if (coordinator_) {
        return true;
    }

    trayIcon_ = &trayIcon;
    logger_ = &logger;
    shuttingDown_ = false;
    try {
        applyRequested_ = std::move(applyRequested);
        isIdle_ = std::move(isIdle);
        scheduleStore_ = std::make_unique<update::DailyUpdateStateStore>(
            win32::LocalAppDataDirectory() / L"automatic-update.ini");
        schedule_ = update::DailyUpdatePolicy(scheduleStore_->LoadCompletedDate());
        const std::optional<update::SemanticVersion> currentVersion =
            update::SemanticVersion::Parse(product::Version);
        if (!currentVersion.has_value()) {
            LogError(L"产品版本号无效，在线更新功能未初始化：" +
                     std::wstring(product::Version));
            return false;
        }

        update::GitHubUpdateClientOptions options;
        options.versionUrl = product::UpdateVersionUrl;
        options.executableUrl = product::UpdateExecutableUrl;
        options.userAgent = std::wstring(product::Name) + L"/" + product::Version;
        coordinator_ = std::make_unique<update::UpdateCoordinator>(
            *currentVersion,
            std::move(options));

        coordinator_->SetStatusCallback(
            [messageWindow](const update::UpdateSnapshot&) noexcept {
                static_cast<void>(::PostMessageW(
                    messageWindow,
                    messages::UpdateStatusChanged,
                    0,
                    0));
            });
        return true;
    } catch (...) {
        coordinator_.reset();
        LogError(L"在线更新模块初始化失败；录屏功能不受影响。");
        return false;
    }
}

void AppUpdateController::HandleStatusChanged() noexcept {
    if (!coordinator_ || shuttingDown_) {
        return;
    }

    try {
        const update::UpdateSnapshot snapshot = coordinator_->Snapshot();
        switch (snapshot.phase) {
        case update::UpdatePhase::UpdateAvailable: {
            // Poll starts downloading only when recorder/editor activity is idle.
            break;
        }
        case update::UpdatePhase::UpToDate:
            if (terminalNotification_ == TerminalNotification::UpToDate) {
                break;
            }
            terminalNotification_ = TerminalNotification::UpToDate;
            downloadRequested_ = false;
            readyExecutable_.clear();
            schedule_.Completed(ClockNow());
            PersistCompletedDate();
            LogInfo(L"在线更新检查完成：当前已是最新版本。");
            Notify(
                L"当前已是最新版本",
                L"SuperRecording " + std::wstring(product::Version) +
                    L" 无需更新。",
                NIIF_INFO);
            automaticOperation_ = true;
            break;
        case update::UpdatePhase::ReadyToInstall: {
            downloadRequested_ = false;
            const bool alreadyHandled =
                !readyExecutable_.empty() &&
                readyExecutable_ == snapshot.downloadedFile;
            readyExecutable_ = snapshot.downloadedFile;
            if (alreadyHandled) {
                break;
            }
            const std::wstring version = snapshot.latestVersion.has_value()
                ? snapshot.latestVersion->ToWString()
                : L"最新版";
            LogInfo(L"SuperRecording " + version +
                    L" 已下载到：" + readyExecutable_.wstring());
            Notify(
                L"SuperRecording " + version + L" 已就绪",
                L"空闲后将自动更新并重新启动，不会关闭正在录制或编辑的内容。",
                NIIF_INFO);
            break;
        }
        case update::UpdatePhase::Failed: {
            if (terminalNotification_ == TerminalNotification::Failed) {
                break;
            }
            terminalNotification_ = TerminalNotification::Failed;
            downloadRequested_ = false;
            readyExecutable_.clear();
            schedule_.RetryLater(ClockNow());
            const std::wstring diagnostic = snapshot.failure.message.empty()
                ? L"无底层诊断"
                : snapshot.failure.message;
            LogError(L"在线更新失败：" + diagnostic);
            Notify(
                L"在线更新失败",
                UpdateFailureText(snapshot.failure),
                NIIF_WARNING);
            automaticOperation_ = true;
            break;
        }
        case update::UpdatePhase::Cancelled:
            if (terminalNotification_ == TerminalNotification::Cancelled) break;
            terminalNotification_ = TerminalNotification::Cancelled;
            downloadRequested_ = false;
            readyExecutable_.clear();
            if (activityPaused_) {
                // Activity cancellation is a deferral, not a network failure.
                schedule_.RequestManualCheck();
                activityPaused_ = false;
            } else {
                schedule_.RetryLater(ClockNow());
            }
            break;
        case update::UpdatePhase::Idle:
        case update::UpdatePhase::Checking:
        case update::UpdatePhase::Downloading:
        default:
            break;
        }
        Poll();
    } catch (...) {
        LogError(L"在线更新状态处理失败；录屏功能不受影响。");
    }
}

void AppUpdateController::HandleMenuCommand(const MenuCommand command) noexcept {
    switch (command) {
    case MenuCommand::CheckForUpdates:
        CheckForUpdates();
        break;
    case MenuCommand::ApplyDownloadedUpdate:
        Poll();
        break;
    default:
        break;
    }
}

void AppUpdateController::AppendTrayMenu(
    const HMENU menu,
    const UINT checkCommandId,
    const UINT applyCommandId) const noexcept {
    if (menu == nullptr) {
        return;
    }

    try {
        const update::UpdateSnapshot snapshot = coordinator_
            ? coordinator_->Snapshot()
            : update::UpdateSnapshot{};
        const bool updateReady = !readyExecutable_.empty() ||
            (snapshot.phase == update::UpdatePhase::ReadyToInstall &&
             !snapshot.downloadedFile.empty());
        if (updateReady) {
            std::wstring applyLabel = L"更新已就绪，空闲后自动重启";
            if (snapshot.latestVersion.has_value()) {
                applyLabel.append(L" · ");
                applyLabel.append(snapshot.latestVersion->ToWString());
            }
            ::AppendMenuW(
                menu,
                MF_STRING | MF_GRAYED,
                applyCommandId,
                applyLabel.c_str());
        } else {
            std::wstring checkLabel = L"检查更新…";
            UINT availability = MF_ENABLED;
            if (snapshot.phase == update::UpdatePhase::Checking) {
                checkLabel = L"正在检查更新…";
                availability = MF_GRAYED;
            } else if (snapshot.phase == update::UpdatePhase::Downloading ||
                       (snapshot.phase == update::UpdatePhase::UpdateAvailable &&
                        downloadRequested_)) {
                checkLabel = L"正在下载更新…";
                availability = MF_GRAYED;
            }
            ::AppendMenuW(
                menu,
                MF_STRING | availability,
                checkCommandId,
                checkLabel.c_str());
        }

        const std::wstring versionLabel =
            L"当前版本：" + std::wstring(product::Version);
        ::AppendMenuW(menu, MF_STRING | MF_DISABLED, 0, L"自动更新：每天 11:00，空闲时安装");
        ::AppendMenuW(menu, MF_STRING | MF_DISABLED, 0, versionLabel.c_str());
        ::AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    } catch (...) {
        ::AppendMenuW(
            menu,
            MF_STRING | MF_GRAYED,
            checkCommandId,
            L"在线更新暂不可用");
        ::AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    }
}

void AppUpdateController::Shutdown() noexcept {
    if (shuttingDown_) {
        return;
    }
    shuttingDown_ = true;
    if (coordinator_) {
        coordinator_->SetStatusCallback({});
        coordinator_->CancelAndWait();
        coordinator_.reset();
    }
    applyRequested_ = {};
    isIdle_ = {};
    scheduleStore_.reset();
    readyExecutable_.clear();
    terminalNotification_ = TerminalNotification::None;
    downloadRequested_ = false;
    activityPaused_ = false;
    trayIcon_ = nullptr;
    logger_ = nullptr;
}

void AppUpdateController::CheckForUpdates() noexcept {
    automaticOperation_ = false;
    if (!coordinator_ || shuttingDown_) {
        Notify(
            L"暂时无法检查更新",
            L"在线更新模块未正确初始化。",
            NIIF_WARNING);
        return;
    }

    try {
        if (!readyExecutable_.empty()) {
            Notify(L"更新已就绪", L"空闲后将自动更新，无需手动重启。", NIIF_INFO);
            return;
        }

        const update::UpdateSnapshot snapshot = coordinator_->Snapshot();
        if (coordinator_->IsBusy()) {
            Notify(
                L"SuperRecording 正在更新",
                snapshot.phase == update::UpdatePhase::Downloading
                    ? L"新版正在后台下载，请稍候。"
                    : L"正在检查 GitHub 上的最新版本，请稍候。",
                NIIF_INFO);
            return;
        }

        schedule_.RequestManualCheck();
        Notify(
            L"检查更新已安排",
            ApplicationIdle() ? L"将连接 GitHub Releases 并自动完成更新。"
                              : L"录屏或编辑结束后会自动检查并更新。",
            NIIF_INFO);
        Poll();
    } catch (...) {
        LogError(L"无法开始在线更新检查。");
        Notify(
            L"无法开始检查更新",
            L"更新任务暂时不可用，请稍后重试。",
            NIIF_WARNING);
    }
}

void AppUpdateController::ApplyDownloadedUpdate() noexcept {
    if (shuttingDown_ || !ApplicationIdle()) {
        return;
    }

    try {
        if (readyExecutable_.empty() && coordinator_) {
            const update::UpdateSnapshot snapshot = coordinator_->Snapshot();
            if (snapshot.phase == update::UpdatePhase::ReadyToInstall) {
                readyExecutable_ = snapshot.downloadedFile;
            }
        }
        if (readyExecutable_.empty()) {
            return;
        }

        const DWORD attributes = ::GetFileAttributesW(readyExecutable_.c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES ||
            (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
            readyExecutable_.clear();
            // Discard the stale ReadyToInstall snapshot by starting a fresh check.
            StartCheck();
            Notify(
                L"更新文件不可用",
                L"请重新检查并下载更新。",
                NIIF_WARNING);
            return;
        }

        ApplyRequestedCallback applyRequested = applyRequested_;
        std::filesystem::path readyExecutable = readyExecutable_;
        if (applyRequested) {
            schedule_.Completed(ClockNow());
            PersistCompletedDate();
            LogInfo(L"应用已空闲，自动安装更新并重新启动。");
            if (!applyRequested(std::move(readyExecutable), automaticOperation_) && !shuttingDown_) {
                schedule_.RetryLater(ClockNow());
                automaticOperation_ = true;
                LogError(L"更新安装未启动，将在 15 分钟后空闲时重试。");
            }
        }
    } catch (...) {
        schedule_.RetryLater(ClockNow());
        LogError(L"无法提交已下载的更新文件。");
        Notify(
            L"更新文件不可用",
            L"请重新检查并下载更新。",
            NIIF_WARNING);
    }
}

void AppUpdateController::Notify(
    const std::wstring_view title,
    const std::wstring_view text,
    const DWORD flags) const noexcept {
    try {
        if (trayIcon_ != nullptr && !automaticOperation_) {
            trayIcon_->ShowNotification(title, text, flags);
        }
    } catch (...) {
    }
}

void AppUpdateController::LogInfo(const std::wstring_view message) const noexcept {
    try {
        if (logger_ != nullptr) {
            logger_->Info(message);
        }
    } catch (...) {
    }
}

void AppUpdateController::LogError(const std::wstring_view message) const noexcept {
    try {
        if (logger_ != nullptr) {
            logger_->Error(message);
        }
    } catch (...) {
    }
}

}  // namespace qrec
