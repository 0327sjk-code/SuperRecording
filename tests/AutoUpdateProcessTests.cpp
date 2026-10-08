// Exercises the actual updater process/rollback handshake with isolated fixture executables.
#include "app/CommandLineOptions.h"
#include "update/SelfUpdateBootstrap.h"
#include "update/detail/SelfUpdatePath.h"
#include "update/detail/SelfUpdateProcess.h"

#include <windows.h>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>

void RunAutoUpdatePolicyTests(const std::filesystem::path& root);
void RunAutoUpdateControllerTests(const std::filesystem::path& root, const std::filesystem::path& executable);

namespace {
void Expect(const bool value, const char* name) {
    if (!value) throw std::runtime_error(name);
    std::printf("PASS %s\n", name);
}
void Marker(const std::filesystem::path& path, const char* text) {
    std::ofstream file(path); file << text;
    if (!file) throw std::runtime_error("fixture marker write failed");
}
bool AwaitFile(const std::filesystem::path& path) {
    const auto deadline = ::GetTickCount64() + 10'000;
    while (::GetTickCount64() < deadline) {
        if (std::filesystem::is_regular_file(path)) return true;
        ::Sleep(20);
    }
    return false;
}

enum class BootstrapScenario { Success, Unhealthy, LockedBackup };

void TestBootstrap(const std::filesystem::path& root, const BootstrapScenario mode) {
    namespace update = qrec::update;
    const bool failHealth = mode == BootstrapScenario::Unhealthy;
    const bool lockedBackup = mode == BootstrapScenario::LockedBackup;
    const auto scenario = root / (failHealth ? L"rollback" : lockedBackup ? L"locked-backup" : L"success");
    const auto download = scenario / L"download" / L"SuperRecording.exe";
    const auto target = scenario / L"installed" / L"SuperRecording.exe";
    std::filesystem::create_directories(download.parent_path());
    std::filesystem::create_directories(target.parent_path());
    const auto self = update::detail::CurrentExecutablePath(nullptr);
    std::filesystem::copy_file(self, download);
    std::filesystem::copy_file(self, target);
    // A trailing fixture marker distinguishes the prior usable executable byte-for-byte.
    { std::ofstream previous(target, std::ios::binary | std::ios::app); previous << "previous-build"; }
    if (failHealth) Marker(target.parent_path() / L"fail-health.flag", "fixture failure");
    if (lockedBackup) std::filesystem::create_directory(update::detail::BackupPathFor(target));
    const auto oldSize = std::filesystem::file_size(target);
    const auto result = update::LaunchApplyUpdate(download, target, 0x7fffffffu, true);
    Expect(result.success, "bootstrap launched without UI or manual restart");
    Expect(AwaitFile(download.parent_path() / L"bootstrap-result.txt"), "bootstrap completed in background");
    std::ifstream resultFile(download.parent_path() / L"bootstrap-result.txt");
    int success = -1; resultFile >> success;
    if (failHealth || lockedBackup) {
        Expect(success == 0, "unhealthy or unwritable update rejected");
        Expect(AwaitFile(target.parent_path() / L"previous-restarted.txt"), "previous version automatically restarted");
        Expect(std::filesystem::file_size(target) == oldSize, "previous executable restored exactly");
    } else {
        Expect(success == 1, "new executable passed real named-event health handshake");
        Expect(AwaitFile(target.parent_path() / L"installed-started.txt"), "new version starts automatically");
        DWORD error = 0;
        Expect(update::detail::VerifyInstalledCopy(download, target, &error), "installed file matches downloaded bytes");
    }
}
}  // namespace

int wmain(const int count, wchar_t* arguments[]) {
    try {
        const auto options = qrec::app::ParseCommandLine();
        if (!options.valid) return 21;
        const auto self = qrec::update::detail::CurrentExecutablePath(nullptr);
        if (options.applyUpdate) {
            if (!options.launchedAtStartup) return 22;
            const auto result = qrec::update::ApplyUpdate({options.targetExecutable, options.parentProcessId});
            Marker(self.parent_path() / L"bootstrap-result.txt", result.success ? "1" : "0");
            return result.success ? 0 : 23;
        }
        if (options.HasCleanupRequest()) {
            if (!options.launchedAtStartup) return 24;
            if (std::filesystem::exists(self.parent_path() / L"fail-health.flag")) return 25;
            const auto result = qrec::update::SignalUpdateReady(options.updateHealthEventName);
            if (!result.success) return 26;
            Marker(self.parent_path() / L"installed-started.txt", "ready");
            ::Sleep(2'000);  // Keep the fixture alive while the bootstrap verifies health.
            return 0;
        }
        if (options.launchedAtStartup) {
            Marker(self.parent_path() / L"previous-restarted.txt", "rollback launched silently");
            return 0;
        }
        if (count != 2) throw std::runtime_error("Supply a unique isolated test output directory");
        const std::filesystem::path root(arguments[1]);
        if (!root.is_absolute() || std::filesystem::exists(root))
            throw std::runtime_error("Test output must be a new absolute directory");
        std::filesystem::create_directories(root);
        RunAutoUpdatePolicyTests(root);
        RunAutoUpdateControllerTests(root, self);
        TestBootstrap(root, BootstrapScenario::Success);
        TestBootstrap(root, BootstrapScenario::Unhealthy);
        TestBootstrap(root, BootstrapScenario::LockedBackup);
        ::Sleep(2'100);  // Test-owned helper processes finish before the runner exits.
        std::puts("All automatic update policy and process tests passed");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL %s\n", error.what()); return 1;
    }
}
