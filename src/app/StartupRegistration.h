#pragma once

#include "common/ProductInfo.h"

#include <windows.h>

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

namespace qrec::startup {

inline constexpr wchar_t RunRegistrySubKey[] =
    L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
inline constexpr const wchar_t* RunRegistryValueName = product::Name;
inline constexpr const wchar_t* LegacyRunRegistryValueName = product::LegacyName;
inline constexpr wchar_t AutoStartArgument[] = L"--autostart";

enum class RegistrationOperation : std::uint8_t {
    None,
    ResolveExecutable,
    ValidateExecutable,
    QueryRegistry,
    WriteRegistry,
    DeleteRegistry,
    DeleteLegacyRegistry,
};

struct RegistrationResult final {
    bool success{};
    bool changed{};
    RegistrationOperation operation{RegistrationOperation::None};
    DWORD nativeError{ERROR_SUCCESS};
    DWORD legacyCleanupError{ERROR_SUCCESS};
    std::wstring command;
};

class Registry final {
public:
    class Interface {
    public:
        virtual ~Interface() = default;
        [[nodiscard]] virtual LSTATUS QueryCommand(std::wstring& command) const noexcept = 0;
        [[nodiscard]] virtual LSTATUS WriteCommand(std::wstring_view command) noexcept = 0;
        [[nodiscard]] virtual LSTATUS DeleteCommand() noexcept = 0;
    };
};

[[nodiscard]] bool BuildQuotedExecutableCommand(
    const std::filesystem::path& executablePath,
    std::wstring* command,
    DWORD* error = nullptr) noexcept;

[[nodiscard]] std::filesystem::path CurrentExecutablePath(
    DWORD* error = nullptr) noexcept;

[[nodiscard]] RegistrationResult Reconcile(
    Registry::Interface& registry,
    bool enabled,
    const std::filesystem::path& executablePath) noexcept;

[[nodiscard]] RegistrationResult ReconcileCurrentUser(bool enabled) noexcept;

}  // namespace qrec::startup
