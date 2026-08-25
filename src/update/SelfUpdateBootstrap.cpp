#include "update/SelfUpdateBootstrap.h"

#include "update/detail/SelfUpdatePath.h"
#include "update/detail/SelfUpdateProcess.h"

#include <windows.h>

#include <filesystem>
#include <new>
#include <string_view>

namespace qrec::update {
namespace {

constexpr DWORD PreviousProcessExitTimeoutMs = 120'000;
constexpr DWORD BootstrapCleanupTimeoutMs = 30'000;
constexpr DWORD InstalledHealthTimeoutMs = 120'000;
constexpr DWORD InstalledTerminationTimeoutMs = 5'000;

[[nodiscard]] BootstrapResult Failure(
    const BootstrapStage stage,
    const DWORD nativeError,
    const std::wstring_view message,
    const DWORD rollbackError = ERROR_SUCCESS) noexcept {
    BootstrapResult result;
    result.stage = stage;
    result.nativeError = nativeError;
    result.rollbackError = rollbackError;
    try {
        result.message.assign(message);
    } catch (...) {
        result.message.clear();
    }
    return result;
}

[[nodiscard]] BootstrapResult Success() noexcept {
    BootstrapResult result;
    result.success = true;
    return result;
}

[[nodiscard]] DWORD RestorePreviousExecutable(
    const std::filesystem::path& targetExecutable,
    const std::filesystem::path& backupExecutable) noexcept {
    const DWORD targetAttributes = ::GetFileAttributesW(targetExecutable.c_str());
    if (targetAttributes != INVALID_FILE_ATTRIBUTES &&
        (targetAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0) {
        static_cast<void>(::DeleteFileW(targetExecutable.c_str()));
    }
    if (::MoveFileExW(
            backupExecutable.c_str(),
            targetExecutable.c_str(),
            MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != FALSE) {
        return ERROR_SUCCESS;
    }
    return ::GetLastError();
}

[[nodiscard]] bool ValidateUpdateEndpoints(
    const std::filesystem::path& downloadedExecutable,
    const bool requireDownloadedProductName,
    const std::filesystem::path& targetExecutable,
    DWORD* error) {
    return detail::ValidateExecutablePath(
               downloadedExecutable,
               requireDownloadedProductName,
               error) &&
        detail::ValidateExecutablePath(targetExecutable, true, error) &&
        !detail::PathsEqualInsensitive(
            downloadedExecutable,
            targetExecutable);
}

[[nodiscard]] DWORD RestoreAndRestartPreviousExecutable(
    const std::filesystem::path& targetExecutable,
    const std::filesystem::path& backupExecutable) noexcept {
    const DWORD restoreError = RestorePreviousExecutable(
        targetExecutable,
        backupExecutable);
    if (restoreError != ERROR_SUCCESS) {
        return restoreError;
    }

    try {
        DWORD launchError = ERROR_SUCCESS;
        if (!detail::LaunchExecutableNormally(targetExecutable, &launchError)) {
            return launchError == ERROR_SUCCESS ? ERROR_GEN_FAILURE : launchError;
        }
    } catch (const std::bad_alloc&) {
        return ERROR_NOT_ENOUGH_MEMORY;
    } catch (...) {
        return ERROR_GEN_FAILURE;
    }
    return ERROR_SUCCESS;
}

[[nodiscard]] BootstrapResult InstallFailure(
    const BootstrapStage failureStage,
    const DWORD nativeError,
    const std::wstring_view message,
    const std::filesystem::path& targetExecutable,
    const std::filesystem::path& backupExecutable) noexcept {
    const DWORD rollbackError = RestoreAndRestartPreviousExecutable(
        targetExecutable,
        backupExecutable);
    return Failure(
        rollbackError == ERROR_SUCCESS
            ? failureStage
            : BootstrapStage::Rollback,
        nativeError,
        message,
        rollbackError);
}

[[nodiscard]] BootstrapResult HealthFailure(
    detail::InstalledProcess& installedProcess,
    DWORD nativeError,
    const std::wstring_view message,
    const std::filesystem::path& targetExecutable,
    const std::filesystem::path& backupExecutable) noexcept {
    if (nativeError == ERROR_SUCCESS) {
        nativeError = ERROR_PROCESS_ABORTED;
    }

    DWORD terminationError = ERROR_SUCCESS;
    static_cast<void>(detail::TerminateInstalledProcess(
        installedProcess,
        InstalledTerminationTimeoutMs,
        &terminationError));
    const DWORD rollbackError = RestoreAndRestartPreviousExecutable(
        targetExecutable,
        backupExecutable);
    return Failure(
        rollbackError == ERROR_SUCCESS
            ? BootstrapStage::WaitForInstalledExecutableHealth
            : BootstrapStage::Rollback,
        nativeError,
        message,
        rollbackError);
}

}  // namespace

BootstrapResult ApplyUpdate(const ApplyUpdateRequest& request) noexcept {
    std::filesystem::path targetExecutable;
    std::filesystem::path backupExecutable;
    bool rollbackRequired = false;
    try {
        DWORD nativeError = ERROR_SUCCESS;
        const std::filesystem::path bootstrapExecutable =
            detail::CurrentExecutablePath(&nativeError);
        targetExecutable = request.targetExecutable.lexically_normal();

        if (!ValidateUpdateEndpoints(
                bootstrapExecutable,
                false,
                targetExecutable,
                &nativeError)) {
            if (nativeError == ERROR_SUCCESS) {
                nativeError = ERROR_INVALID_PARAMETER;
            }
            return Failure(
                BootstrapStage::ValidateRequest,
                nativeError,
                L"更新文件或安装路径无效。路径必须是绝对 EXE 路径，且 EXE 本身不能是重解析文件。");
        }

        if (!detail::WaitForProcessExit(
                request.parentProcessId,
                PreviousProcessExitTimeoutMs,
                &nativeError)) {
            return Failure(
                BootstrapStage::WaitForPreviousProcess,
                nativeError,
                L"等待旧版 SuperRecording 退出失败。");
        }

        backupExecutable = detail::BackupPathFor(targetExecutable);
        const DWORD backupAttributes =
            ::GetFileAttributesW(backupExecutable.c_str());
        if (backupAttributes != INVALID_FILE_ATTRIBUTES &&
            ((backupAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0 ||
             (backupAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0)) {
            return Failure(
                BootstrapStage::PreservePreviousExecutable,
                ERROR_CANT_ACCESS_FILE,
                L"旧版备份路径被目录或重解析点占用，无法安全更新。");
        }

        if (::MoveFileExW(
                targetExecutable.c_str(),
                backupExecutable.c_str(),
                MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) == FALSE) {
            return Failure(
                BootstrapStage::PreservePreviousExecutable,
                ::GetLastError(),
                L"无法保留旧版 SuperRecording.exe。");
        }
        rollbackRequired = true;

        if (::CopyFileW(
                bootstrapExecutable.c_str(),
                targetExecutable.c_str(),
                TRUE) == FALSE) {
            return InstallFailure(
                BootstrapStage::InstallExecutable,
                ::GetLastError(),
                L"复制新版 SuperRecording.exe 失败，已尝试恢复旧版。",
                targetExecutable,
                backupExecutable);
        }

        if (!detail::VerifyInstalledCopy(
                bootstrapExecutable,
                targetExecutable,
                &nativeError)) {
            if (nativeError == ERROR_SUCCESS) {
                nativeError = ERROR_CRC;
            }
            return InstallFailure(
                BootstrapStage::InstallExecutable,
                nativeError,
                L"新版文件校验失败，已尝试恢复旧版。",
                targetExecutable,
                backupExecutable);
        }

        if (!detail::NormalizeInstalledExecutableAttributes(
                targetExecutable,
                &nativeError)) {
            return InstallFailure(
                BootstrapStage::InstallExecutable,
                nativeError,
                L"无法清除新版程序的临时文件属性，已尝试恢复旧版。",
                targetExecutable,
                backupExecutable);
        }

        detail::InstalledProcess installedProcess;
        if (!detail::LaunchInstalledExecutable(
                targetExecutable,
                bootstrapExecutable,
                &installedProcess,
                &nativeError)) {
            return InstallFailure(
                BootstrapStage::LaunchInstalledExecutable,
                nativeError,
                L"启动新版 SuperRecording 失败，已尝试恢复旧版。",
                targetExecutable,
                backupExecutable);
        }

        const detail::InstalledHealthOutcome healthOutcome =
            detail::WaitForInstalledHealth(
                installedProcess,
                InstalledHealthTimeoutMs,
                &nativeError);
        if (healthOutcome != detail::InstalledHealthOutcome::Ready) {
            if (healthOutcome == detail::InstalledHealthOutcome::ProcessExited) {
                nativeError = ERROR_PROCESS_ABORTED;
            }
            return HealthFailure(
                installedProcess,
                nativeError,
                healthOutcome == detail::InstalledHealthOutcome::Timeout
                    ? L"新版 SuperRecording 未在 120 秒内完成初始化，已恢复并重新启动旧版。"
                    : L"新版 SuperRecording 在完成初始化前退出，已恢复并重新启动旧版。",
                targetExecutable,
                backupExecutable);
        }

        detail::RemoveFileBestEffort(backupExecutable);
        rollbackRequired = false;
        return Success();
    } catch (const std::bad_alloc&) {
        const DWORD rollbackError = rollbackRequired
            ? RestoreAndRestartPreviousExecutable(
                  targetExecutable,
                  backupExecutable)
            : ERROR_SUCCESS;
        return Failure(
            rollbackError == ERROR_SUCCESS
                ? (rollbackRequired
                       ? BootstrapStage::InstallExecutable
                       : BootstrapStage::ValidateRequest)
                : BootstrapStage::Rollback,
            ERROR_NOT_ENOUGH_MEMORY,
            rollbackRequired
                ? L"执行更新时内存不足，已尝试恢复并重新启动旧版。"
                : L"执行更新时内存不足。",
            rollbackError);
    } catch (...) {
        const DWORD rollbackError = rollbackRequired
            ? RestoreAndRestartPreviousExecutable(
                  targetExecutable,
                  backupExecutable)
            : ERROR_SUCCESS;
        return Failure(
            rollbackError == ERROR_SUCCESS
                ? (rollbackRequired
                       ? BootstrapStage::InstallExecutable
                       : BootstrapStage::ValidateRequest)
                : BootstrapStage::Rollback,
            ERROR_GEN_FAILURE,
            rollbackRequired
                ? L"执行更新时发生未知错误，已尝试恢复并重新启动旧版。"
                : L"执行更新时发生未知错误。",
            rollbackError);
    }
}

BootstrapResult LaunchApplyUpdate(
    const std::filesystem::path& downloadedExecutable,
    const std::filesystem::path& targetExecutable,
    const std::uint32_t parentProcessId) noexcept {
    try {
        DWORD nativeError = ERROR_SUCCESS;
        const std::filesystem::path normalizedDownload =
            downloadedExecutable.lexically_normal();
        const std::filesystem::path normalizedTarget =
            targetExecutable.lexically_normal();
        if (parentProcessId == 0 ||
            !ValidateUpdateEndpoints(
                normalizedDownload,
                true,
                normalizedTarget,
                &nativeError)) {
            if (nativeError == ERROR_SUCCESS) {
                nativeError = ERROR_INVALID_PARAMETER;
            }
            return Failure(
                BootstrapStage::ValidateRequest,
                nativeError,
                L"下载文件或安装路径无效，无法启动更新程序。");
        }

        if (!detail::LaunchBootstrapExecutable(
                normalizedDownload,
                normalizedTarget,
                parentProcessId,
                &nativeError)) {
            return Failure(
                BootstrapStage::LaunchBootstrapExecutable,
                nativeError,
                L"无法启动 SuperRecording 更新程序。");
        }
        return Success();
    } catch (const std::bad_alloc&) {
        return Failure(
            BootstrapStage::LaunchBootstrapExecutable,
            ERROR_NOT_ENOUGH_MEMORY,
            L"启动更新程序时内存不足。");
    } catch (...) {
        return Failure(
            BootstrapStage::LaunchBootstrapExecutable,
            ERROR_GEN_FAILURE,
            L"启动更新程序时发生未知错误。");
    }
}

BootstrapResult SignalUpdateReady(
    const std::wstring_view healthEventName) noexcept {
    try {
        DWORD nativeError = ERROR_SUCCESS;
        if (healthEventName.empty() ||
            !detail::SignalHealthEvent(healthEventName, &nativeError)) {
            if (nativeError == ERROR_SUCCESS) {
                nativeError = ERROR_INVALID_NAME;
            }
            return Failure(
                BootstrapStage::SignalInstalledExecutableReady,
                nativeError,
                L"无法向更新引导程序报告新版初始化完成。");
        }
        return Success();
    } catch (const std::bad_alloc&) {
        return Failure(
            BootstrapStage::SignalInstalledExecutableReady,
            ERROR_NOT_ENOUGH_MEMORY,
            L"报告新版初始化状态时内存不足。");
    } catch (...) {
        return Failure(
            BootstrapStage::SignalInstalledExecutableReady,
            ERROR_GEN_FAILURE,
            L"报告新版初始化状态时发生未知错误。");
    }
}

void CleanupUpdateArtifacts(
    const std::filesystem::path& bootstrapExecutable,
    const std::uint32_t bootstrapProcessId) noexcept {
    try {
        if (bootstrapProcessId == 0 ||
            bootstrapProcessId == ::GetCurrentProcessId()) {
            return;
        }

        DWORD ignoredError = ERROR_SUCCESS;
        const std::filesystem::path normalizedBootstrap =
            bootstrapExecutable.lexically_normal();
        if (!detail::IsOwnedTemporaryUpdateExecutable(
                normalizedBootstrap,
                &ignoredError)) {
            return;
        }

        if (!detail::WaitForProcessExit(
                bootstrapProcessId,
                BootstrapCleanupTimeoutMs,
                &ignoredError)) {
            return;
        }

        detail::RemoveFileBestEffort(normalizedBootstrap);
        static_cast<void>(::RemoveDirectoryW(
            normalizedBootstrap.parent_path().c_str()));
    } catch (...) {
        // Cleanup must never prevent the installed application from starting.
    }
}

}  // namespace qrec::update
