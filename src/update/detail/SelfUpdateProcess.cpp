#include "update/detail/SelfUpdateProcess.h"

#include "app/CommandLineOptions.h"

#include <windows.h>

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <iterator>
#include <string>
#include <utility>
#include <vector>

namespace qrec::update::detail {
namespace {

constexpr DWORD TerminatedUpdateExitCode = ERROR_CANCELLED;
std::atomic_uint64_t healthEventSequence{};

class ScopedHandle final {
public:
    explicit ScopedHandle(HANDLE handle = nullptr) noexcept : handle_(handle) {}
    ~ScopedHandle() {
        if (handle_ != nullptr && handle_ != INVALID_HANDLE_VALUE) {
            ::CloseHandle(handle_);
        }
    }

    ScopedHandle(const ScopedHandle&) = delete;
    ScopedHandle& operator=(const ScopedHandle&) = delete;

    [[nodiscard]] HANDLE Get() const noexcept { return handle_; }
    [[nodiscard]] HANDLE Release() noexcept {
        return std::exchange(handle_, nullptr);
    }
    void Reset(const HANDLE handle = nullptr) noexcept {
        if (handle_ != nullptr && handle_ != INVALID_HANDLE_VALUE) {
            ::CloseHandle(handle_);
        }
        handle_ = handle;
    }

private:
    HANDLE handle_{};
};

[[nodiscard]] std::wstring BuildBootstrapCommandLine(
    const std::filesystem::path& downloadedExecutable,
    const std::filesystem::path& targetExecutable,
    const std::uint32_t parentProcessId) {
    std::wstring commandLine;
    const std::wstring processId = std::to_wstring(parentProcessId);
    commandLine.reserve(
        downloadedExecutable.native().size() + targetExecutable.native().size() +
        processId.size() + 80);
    commandLine.push_back(L'"');
    commandLine.append(downloadedExecutable.native());
    commandLine.append(L"\" ");
    commandLine.append(app::command_line::ApplyUpdateArgument);
    commandLine.push_back(L' ');
    commandLine.append(app::command_line::TargetExecutablePrefix);
    commandLine.push_back(L'"');
    commandLine.append(targetExecutable.native());
    commandLine.append(L"\" ");
    commandLine.append(app::command_line::ParentProcessIdPrefix);
    commandLine.append(processId);
    return commandLine;
}

[[nodiscard]] std::wstring BuildInstalledCommandLine(
    const std::filesystem::path& targetExecutable,
    const std::filesystem::path& bootstrapExecutable,
    const std::wstring_view healthEventName) {
    std::wstring commandLine;
    const std::wstring processId = std::to_wstring(::GetCurrentProcessId());
    commandLine.reserve(
        targetExecutable.native().size() + bootstrapExecutable.native().size() +
        healthEventName.size() + processId.size() + 112);
    commandLine.push_back(L'"');
    commandLine.append(targetExecutable.native());
    commandLine.append(L"\" ");
    commandLine.append(app::command_line::CleanupUpdatePrefix);
    commandLine.push_back(L'"');
    commandLine.append(bootstrapExecutable.native());
    commandLine.append(L"\" ");
    commandLine.append(app::command_line::UpdateBootstrapProcessIdPrefix);
    commandLine.append(processId);
    commandLine.push_back(L' ');
    commandLine.append(app::command_line::UpdateHealthEventPrefix);
    commandLine.append(healthEventName);
    return commandLine;
}

[[nodiscard]] std::wstring BuildNormalCommandLine(
    const std::filesystem::path& executable) {
    std::wstring commandLine;
    commandLine.reserve(executable.native().size() + 2);
    commandLine.push_back(L'"');
    commandLine.append(executable.native());
    commandLine.push_back(L'"');
    return commandLine;
}

[[nodiscard]] bool CreateProcessWithCommandLine(
    const std::filesystem::path& executable,
    const std::filesystem::path& workingDirectory,
    std::wstring commandLine,
    HANDLE* processHandle,
    DWORD* error) {
    if (processHandle != nullptr) {
        *processHandle = nullptr;
    }
    if (commandLine.size() >= 32'767) {
        if (error != nullptr) {
            *error = ERROR_FILENAME_EXCED_RANGE;
        }
        return false;
    }
    std::vector<wchar_t> mutableCommandLine(commandLine.begin(), commandLine.end());
    mutableCommandLine.push_back(L'\0');

    STARTUPINFOW startupInfo{};
    startupInfo.cb = sizeof(startupInfo);
    PROCESS_INFORMATION processInformation{};
    const BOOL created = ::CreateProcessW(
        executable.c_str(),
        mutableCommandLine.data(),
        nullptr,
        nullptr,
        FALSE,
        CREATE_UNICODE_ENVIRONMENT,
        nullptr,
        workingDirectory.c_str(),
        &startupInfo,
        &processInformation);
    if (created == FALSE) {
        if (error != nullptr) {
            *error = ::GetLastError();
        }
        return false;
    }
    ::CloseHandle(processInformation.hThread);
    if (processHandle != nullptr) {
        *processHandle = processInformation.hProcess;
    } else {
        ::CloseHandle(processInformation.hProcess);
    }
    return true;
}

[[nodiscard]] bool CreateUniqueHealthEvent(
    ScopedHandle* eventHandle,
    std::wstring* eventName,
    DWORD* error) {
    if (eventHandle == nullptr || eventName == nullptr) {
        if (error != nullptr) {
            *error = ERROR_INVALID_PARAMETER;
        }
        return false;
    }

    constexpr std::uint32_t MaximumAttempts = 32;
    for (std::uint32_t attempt = 0; attempt < MaximumAttempts; ++attempt) {
        std::wstring candidate = app::command_line::UpdateHealthEventNamePrefix;
        candidate.append(std::to_wstring(::GetCurrentProcessId()));
        candidate.push_back(L'.');
        candidate.append(std::to_wstring(::GetTickCount64()));
        candidate.push_back(L'.');
        candidate.append(std::to_wstring(
            healthEventSequence.fetch_add(1, std::memory_order_relaxed)));

        ::SetLastError(ERROR_SUCCESS);
        ScopedHandle candidateHandle(::CreateEventW(
            nullptr,
            TRUE,
            FALSE,
            candidate.c_str()));
        if (candidateHandle.Get() == nullptr) {
            if (error != nullptr) {
                *error = ::GetLastError();
            }
            return false;
        }
        if (::GetLastError() != ERROR_ALREADY_EXISTS) {
            *eventName = std::move(candidate);
            eventHandle->Reset(candidateHandle.Release());
            return true;
        }
    }

    if (error != nullptr) {
        *error = ERROR_ALREADY_EXISTS;
    }
    return false;
}

}  // namespace

InstalledProcess::~InstalledProcess() {
    Reset();
}

InstalledProcess::InstalledProcess(InstalledProcess&& other) noexcept
    : processHandle_(std::exchange(other.processHandle_, nullptr)),
      healthEventHandle_(std::exchange(other.healthEventHandle_, nullptr)) {}

InstalledProcess& InstalledProcess::operator=(InstalledProcess&& other) noexcept {
    if (this != &other) {
        Reset(
            std::exchange(other.processHandle_, nullptr),
            std::exchange(other.healthEventHandle_, nullptr));
    }
    return *this;
}

bool InstalledProcess::IsValid() const noexcept {
    return processHandle_ != nullptr && healthEventHandle_ != nullptr;
}

HANDLE InstalledProcess::ProcessHandle() const noexcept {
    return processHandle_;
}

HANDLE InstalledProcess::HealthEventHandle() const noexcept {
    return healthEventHandle_;
}

void InstalledProcess::Reset(
    const HANDLE processHandle,
    const HANDLE healthEventHandle) noexcept {
    if (processHandle_ != nullptr) {
        ::CloseHandle(processHandle_);
    }
    if (healthEventHandle_ != nullptr) {
        ::CloseHandle(healthEventHandle_);
    }
    processHandle_ = processHandle;
    healthEventHandle_ = healthEventHandle;
}

bool WaitForProcessExit(
    const std::uint32_t processId,
    const DWORD timeoutMilliseconds,
    DWORD* error) noexcept {
    if (error != nullptr) {
        *error = ERROR_SUCCESS;
    }
    if (processId == 0 || processId == ::GetCurrentProcessId()) {
        if (error != nullptr) {
            *error = ERROR_INVALID_PARAMETER;
        }
        return false;
    }

    const ScopedHandle process(::OpenProcess(SYNCHRONIZE, FALSE, processId));
    if (process.Get() == nullptr) {
        const DWORD openError = ::GetLastError();
        if (openError == ERROR_INVALID_PARAMETER) {
            return true;
        }
        if (error != nullptr) {
            *error = openError;
        }
        return false;
    }

    const DWORD waitResult = ::WaitForSingleObject(process.Get(), timeoutMilliseconds);
    if (waitResult == WAIT_OBJECT_0) {
        return true;
    }
    if (error != nullptr) {
        *error = waitResult == WAIT_TIMEOUT ? ERROR_TIMEOUT : ::GetLastError();
        if (*error == ERROR_SUCCESS) {
            *error = ERROR_GEN_FAILURE;
        }
    }
    return false;
}

bool LaunchBootstrapExecutable(
    const std::filesystem::path& downloadedExecutable,
    const std::filesystem::path& targetExecutable,
    const std::uint32_t parentProcessId,
    DWORD* error) {
    return CreateProcessWithCommandLine(
        downloadedExecutable,
        downloadedExecutable.parent_path(),
        BuildBootstrapCommandLine(
            downloadedExecutable,
            targetExecutable,
            parentProcessId),
        nullptr,
        error);
}

bool LaunchInstalledExecutable(
    const std::filesystem::path& targetExecutable,
    const std::filesystem::path& bootstrapExecutable,
    InstalledProcess* installedProcess,
    DWORD* error) {
    if (installedProcess == nullptr) {
        if (error != nullptr) {
            *error = ERROR_INVALID_PARAMETER;
        }
        return false;
    }
    installedProcess->Reset();

    ScopedHandle healthEvent;
    std::wstring healthEventName;
    if (!CreateUniqueHealthEvent(&healthEvent, &healthEventName, error)) {
        return false;
    }

    HANDLE processHandle = nullptr;
    if (!CreateProcessWithCommandLine(
            targetExecutable,
            targetExecutable.parent_path(),
            BuildInstalledCommandLine(
                targetExecutable,
                bootstrapExecutable,
                healthEventName),
            &processHandle,
            error)) {
        return false;
    }
    installedProcess->Reset(processHandle, healthEvent.Release());
    return true;
}

InstalledHealthOutcome WaitForInstalledHealth(
    const InstalledProcess& installedProcess,
    const DWORD timeoutMilliseconds,
    DWORD* error) noexcept {
    if (error != nullptr) {
        *error = ERROR_SUCCESS;
    }
    if (!installedProcess.IsValid()) {
        if (error != nullptr) {
            *error = ERROR_INVALID_HANDLE;
        }
        return InstalledHealthOutcome::WaitFailed;
    }

    const HANDLE handles[] = {
        installedProcess.HealthEventHandle(),
        installedProcess.ProcessHandle(),
    };
    const DWORD waitResult = ::WaitForMultipleObjects(
        static_cast<DWORD>(std::size(handles)),
        handles,
        FALSE,
        timeoutMilliseconds);
    if (waitResult == WAIT_OBJECT_0) {
        return InstalledHealthOutcome::Ready;
    }
    if (waitResult == WAIT_OBJECT_0 + 1) {
        return InstalledHealthOutcome::ProcessExited;
    }
    if (waitResult == WAIT_TIMEOUT) {
        if (error != nullptr) {
            *error = ERROR_TIMEOUT;
        }
        return InstalledHealthOutcome::Timeout;
    }
    if (error != nullptr) {
        *error = ::GetLastError();
        if (*error == ERROR_SUCCESS) {
            *error = ERROR_GEN_FAILURE;
        }
    }
    return InstalledHealthOutcome::WaitFailed;
}

bool TerminateInstalledProcess(
    const InstalledProcess& installedProcess,
    const DWORD timeoutMilliseconds,
    DWORD* error) noexcept {
    if (error != nullptr) {
        *error = ERROR_SUCCESS;
    }
    const HANDLE process = installedProcess.ProcessHandle();
    if (process == nullptr) {
        if (error != nullptr) {
            *error = ERROR_INVALID_HANDLE;
        }
        return false;
    }
    if (::WaitForSingleObject(process, 0) == WAIT_OBJECT_0) {
        return true;
    }
    if (::TerminateProcess(process, TerminatedUpdateExitCode) == FALSE) {
        if (error != nullptr) {
            *error = ::GetLastError();
        }
        return false;
    }
    const DWORD waitResult = ::WaitForSingleObject(process, timeoutMilliseconds);
    if (waitResult == WAIT_OBJECT_0) {
        return true;
    }
    if (error != nullptr) {
        *error = waitResult == WAIT_TIMEOUT ? ERROR_TIMEOUT : ::GetLastError();
        if (*error == ERROR_SUCCESS) {
            *error = ERROR_GEN_FAILURE;
        }
    }
    return false;
}

bool LaunchExecutableNormally(
    const std::filesystem::path& executable,
    DWORD* error) {
    return CreateProcessWithCommandLine(
        executable,
        executable.parent_path(),
        BuildNormalCommandLine(executable),
        nullptr,
        error);
}

bool SignalHealthEvent(
    const std::wstring_view healthEventName,
    DWORD* error) {
    if (error != nullptr) {
        *error = ERROR_SUCCESS;
    }
    const std::wstring eventName(healthEventName);
    const ScopedHandle event(::OpenEventW(
        EVENT_MODIFY_STATE,
        FALSE,
        eventName.c_str()));
    if (event.Get() == nullptr) {
        if (error != nullptr) {
            *error = ::GetLastError();
        }
        return false;
    }
    if (::SetEvent(event.Get()) == FALSE) {
        if (error != nullptr) {
            *error = ::GetLastError();
        }
        return false;
    }
    return true;
}

}  // namespace qrec::update::detail
