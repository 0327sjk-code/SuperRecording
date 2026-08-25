#include "app/CommandLineOptions.h"

#include "app/StartupRegistration.h"

#include <windows.h>
#include <shellapi.h>

#include <cerrno>
#include <cwchar>
#include <limits>
#include <string_view>

namespace qrec::app {
namespace {

[[nodiscard]] bool EqualsInsensitive(
    const std::wstring_view left,
    const std::wstring_view right) noexcept {
    if (left.size() != right.size()) {
        return false;
    }
    return ::CompareStringOrdinal(
               left.data(),
               static_cast<int>(left.size()),
               right.data(),
               static_cast<int>(right.size()),
               TRUE) == CSTR_EQUAL;
}

[[nodiscard]] bool StartsWithInsensitive(
    const std::wstring_view value,
    const std::wstring_view prefix) noexcept {
    return value.size() >= prefix.size() &&
        ::CompareStringOrdinal(
            value.data(),
            static_cast<int>(prefix.size()),
            prefix.data(),
            static_cast<int>(prefix.size()),
            TRUE) == CSTR_EQUAL;
}

[[nodiscard]] bool ParseProcessId(
    const std::wstring_view value,
    std::uint32_t* processId) noexcept {
    if (processId == nullptr || value.empty()) {
        return false;
    }

    try {
        const std::wstring text(value);
        wchar_t* end = nullptr;
        errno = 0;
        const unsigned long parsed = std::wcstoul(text.c_str(), &end, 10);
        if (errno == ERANGE || end == text.c_str() || end == nullptr || *end != L'\0' ||
            parsed == 0 || parsed > std::numeric_limits<std::uint32_t>::max()) {
            return false;
        }
        *processId = static_cast<std::uint32_t>(parsed);
        return true;
    } catch (...) {
        return false;
    }
}

void SetError(CommandLineOptions* options, const std::wstring_view message) noexcept {
    if (options == nullptr || !options->valid) {
        return;
    }
    options->valid = false;
    try {
        options->errorMessage.assign(message);
    } catch (...) {
        options->errorMessage.clear();
    }
}

[[nodiscard]] bool AssignPath(
    const std::wstring_view value,
    bool* seen,
    std::filesystem::path* destination,
    CommandLineOptions* options) noexcept {
    if (seen == nullptr || destination == nullptr || options == nullptr) {
        return false;
    }
    if (*seen) {
        SetError(options, L"同一个路径参数不能重复。");
        return false;
    }
    if (value.empty()) {
        SetError(options, L"路径参数不能为空。");
        return false;
    }
    try {
        *destination = std::filesystem::path(std::wstring(value));
        *seen = true;
        return true;
    } catch (...) {
        SetError(options, L"路径参数无效。");
        return false;
    }
}

[[nodiscard]] bool AssignProcessId(
    const std::wstring_view value,
    bool* seen,
    std::uint32_t* destination,
    CommandLineOptions* options) noexcept {
    if (seen == nullptr || destination == nullptr || options == nullptr) {
        return false;
    }
    if (*seen) {
        SetError(options, L"同一个进程 ID 参数不能重复。");
        return false;
    }
    if (!ParseProcessId(value, destination)) {
        SetError(options, L"进程 ID 参数必须是有效的非零整数。");
        return false;
    }
    *seen = true;
    return true;
}

[[nodiscard]] bool IsValidHealthEventName(
    const std::wstring_view value) noexcept {
    constexpr std::wstring_view prefix(
        command_line::UpdateHealthEventNamePrefix,
        std::size(command_line::UpdateHealthEventNamePrefix) - 1);
    constexpr std::size_t MaximumEventNameCharacters = 240;
    if (value.size() <= prefix.size() ||
        value.size() > MaximumEventNameCharacters ||
        value.substr(0, prefix.size()) != prefix) {
        return false;
    }

    const std::wstring_view uniquePart = value.substr(prefix.size());
    if (uniquePart.front() == L'.' || uniquePart.back() == L'.') {
        return false;
    }
    for (const wchar_t character : uniquePart) {
        const bool alphaNumeric =
            (character >= L'0' && character <= L'9') ||
            (character >= L'A' && character <= L'Z') ||
            (character >= L'a' && character <= L'z');
        if (!alphaNumeric && character != L'.' && character != L'_' &&
            character != L'-') {
            return false;
        }
    }
    return true;
}

[[nodiscard]] bool AssignHealthEventName(
    const std::wstring_view value,
    bool* seen,
    std::wstring* destination,
    CommandLineOptions* options) noexcept {
    if (seen == nullptr || destination == nullptr || options == nullptr) {
        return false;
    }
    if (*seen) {
        SetError(options, L"--update-health-event 参数不能重复。");
        return false;
    }
    if (!IsValidHealthEventName(value)) {
        SetError(options, L"更新健康事件名称无效。");
        return false;
    }
    try {
        destination->assign(value);
        *seen = true;
        return true;
    } catch (...) {
        SetError(options, L"无法保存更新健康事件名称。");
        return false;
    }
}

}  // namespace

CommandLineOptions ParseCommandLine() noexcept {
    CommandLineOptions options;
    int argumentCount = 0;
    LPWSTR* arguments = ::CommandLineToArgvW(::GetCommandLineW(), &argumentCount);
    if (arguments == nullptr) {
        SetError(&options, L"无法解析启动参数。");
        return options;
    }

    bool targetSeen = false;
    bool parentProcessSeen = false;
    bool cleanupExecutableSeen = false;
    bool bootstrapProcessSeen = false;
    bool healthEventSeen = false;

    for (int index = 1; index < argumentCount && options.valid; ++index) {
        const std::wstring_view argument(arguments[index]);
        if (EqualsInsensitive(argument, startup::AutoStartArgument)) {
            options.launchedAtStartup = true;
            continue;
        }
        if (EqualsInsensitive(argument, command_line::ApplyUpdateArgument)) {
            if (options.applyUpdate) {
                SetError(&options, L"--apply-update 参数不能重复。");
            } else {
                options.applyUpdate = true;
            }
            continue;
        }
        if (StartsWithInsensitive(argument, command_line::TargetExecutablePrefix)) {
            static_cast<void>(AssignPath(
                argument.substr(std::size(command_line::TargetExecutablePrefix) - 1),
                &targetSeen,
                &options.targetExecutable,
                &options));
            continue;
        }
        if (StartsWithInsensitive(argument, command_line::ParentProcessIdPrefix)) {
            static_cast<void>(AssignProcessId(
                argument.substr(std::size(command_line::ParentProcessIdPrefix) - 1),
                &parentProcessSeen,
                &options.parentProcessId,
                &options));
            continue;
        }
        if (StartsWithInsensitive(argument, command_line::CleanupUpdatePrefix)) {
            static_cast<void>(AssignPath(
                argument.substr(std::size(command_line::CleanupUpdatePrefix) - 1),
                &cleanupExecutableSeen,
                &options.cleanupUpdateExecutable,
                &options));
            continue;
        }
        if (StartsWithInsensitive(
                argument,
                command_line::UpdateBootstrapProcessIdPrefix)) {
            static_cast<void>(AssignProcessId(
                argument.substr(
                    std::size(command_line::UpdateBootstrapProcessIdPrefix) - 1),
                &bootstrapProcessSeen,
                &options.updateBootstrapProcessId,
                &options));
            continue;
        }
        if (StartsWithInsensitive(
                argument,
                command_line::UpdateHealthEventPrefix)) {
            static_cast<void>(AssignHealthEventName(
                argument.substr(
                    std::size(command_line::UpdateHealthEventPrefix) - 1),
                &healthEventSeen,
                &options.updateHealthEventName,
                &options));
            continue;
        }
        if (EqualsInsensitive(argument, L"--update-health-event")) {
            SetError(&options, L"--update-health-event 缺少事件名称。");
        }
    }
    ::LocalFree(arguments);

    if (!options.valid) {
        return options;
    }
    if (options.applyUpdate) {
        if (!targetSeen || !parentProcessSeen) {
            SetError(
                &options,
                L"更新模式必须同时提供 --target-exe 和 --parent-pid。");
        } else if (cleanupExecutableSeen || bootstrapProcessSeen ||
                   healthEventSeen) {
            SetError(&options, L"更新安装参数与更新清理参数不能同时使用。");
        }
        return options;
    }

    if (targetSeen || parentProcessSeen) {
        SetError(&options, L"--target-exe 和 --parent-pid 只能用于更新模式。");
    } else if (cleanupExecutableSeen != bootstrapProcessSeen ||
               cleanupExecutableSeen != healthEventSeen) {
        SetError(
            &options,
            L"更新清理必须同时提供 --cleanup-update、--update-bootstrap-pid 和 --update-health-event。");
    }
    return options;
}

}  // namespace qrec::app
