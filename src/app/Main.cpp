#include "app/AppController.h"
#include "app/CommandLineOptions.h"

#include "common/AppMessages.h"
#include "common/Win32Helpers.h"
#include "update/SelfUpdateBootstrap.h"

#include <windows.h>

#include <chrono>
#include <string>
#include <thread>

namespace {

[[nodiscard]] std::wstring BootstrapErrorMessage(
    const qrec::update::BootstrapResult& result) {
    std::wstring message = result.message.empty()
        ? L"SuperRecording 更新失败。"
        : result.message;
    if (result.nativeError != ERROR_SUCCESS) {
        message.append(L"\n\n系统错误：");
        message.append(qrec::win32::FormatLastError(result.nativeError));
    }
    if (result.rollbackError != ERROR_SUCCESS) {
        message.append(L"\n回滚错误：");
        message.append(qrec::win32::FormatLastError(result.rollbackError));
    }
    return message;
}

}  // namespace

int WINAPI wWinMain(
    const HINSTANCE instance,
    HINSTANCE,
    PWSTR,
    int) {
    ::SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    const qrec::app::CommandLineOptions commandLine =
        qrec::app::ParseCommandLine();
    if (!commandLine.valid) {
        qrec::win32::ShowError(
            nullptr,
            L"SuperRecording 无法启动",
            commandLine.errorMessage.empty()
                ? L"启动参数无效。"
                : commandLine.errorMessage);
        return 2;
    }

    if (commandLine.applyUpdate) {
        qrec::update::ApplyUpdateRequest request;
        request.targetExecutable = commandLine.targetExecutable;
        request.parentProcessId = commandLine.parentProcessId;
        const qrec::update::BootstrapResult result =
            qrec::update::ApplyUpdate(request);
        if (!result.success) {
            qrec::win32::ShowError(
                nullptr,
                L"SuperRecording 更新失败",
                BootstrapErrorMessage(result));
            return 3;
        }
        return 0;
    }

    qrec::win32::ScopedCoInitialize com;
    if (FAILED(com.Result()) && com.Result() != RPC_E_CHANGED_MODE) {
        qrec::win32::ShowError(nullptr, L"SuperRecording 无法启动", qrec::win32::FormatError(com.Result()));
        return 1;
    }

    const bool launchedAtStartup = commandLine.launchedAtStartup;
    HANDLE mutex = ::CreateMutexW(nullptr, TRUE, L"Local\\SuperRecording.SingleInstance.v1");
    if (mutex == nullptr) {
        qrec::win32::ShowError(nullptr, L"SuperRecording 无法启动", qrec::win32::FormatLastError());
        return 1;
    }
    const bool anotherInstance = ::GetLastError() == ERROR_ALREADY_EXISTS;
    if (anotherInstance) {
        if (!launchedAtStartup) {
            for (int attempt = 0; attempt < 10; ++attempt) {
                const HWND existing = ::FindWindowW(qrec::AppController::WindowClassName, nullptr);
                if (existing != nullptr) {
                    ::PostMessageW(existing, qrec::messages::ExistingInstance, 0, 0);
                    break;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
        }
        ::CloseHandle(mutex);
        return 0;
    }

    qrec::AppController application(instance, launchedAtStartup);
    if (!application.Initialize()) {
        ::ReleaseMutex(mutex);
        ::CloseHandle(mutex);
        return 1;
    }

    if (commandLine.HasCleanupRequest()) {
        const qrec::update::BootstrapResult healthResult =
            qrec::update::SignalUpdateReady(
                commandLine.updateHealthEventName);
        if (!healthResult.success) {
            qrec::win32::ShowError(
                nullptr,
                L"SuperRecording 更新失败",
                BootstrapErrorMessage(healthResult));
            ::ReleaseMutex(mutex);
            ::CloseHandle(mutex);
            return 4;
        }

        qrec::update::CleanupUpdateArtifacts(
            commandLine.cleanupUpdateExecutable,
            commandLine.updateBootstrapProcessId);
    }

    const int result = application.Run();
    ::ReleaseMutex(mutex);
    ::CloseHandle(mutex);
    return result;
}
