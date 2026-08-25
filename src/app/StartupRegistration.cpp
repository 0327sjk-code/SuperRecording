#include "app/StartupRegistration.h"

#include <algorithm>
#include <limits>
#include <utility>
#include <vector>

namespace qrec::startup {
namespace {

class ScopedRegistryKey final {
public:
    explicit ScopedRegistryKey(HKEY key = nullptr) noexcept
        : key_(key) {}

    ~ScopedRegistryKey() {
        if (key_ != nullptr) {
            ::RegCloseKey(key_);
        }
    }

    ScopedRegistryKey(const ScopedRegistryKey&) = delete;
    ScopedRegistryKey& operator=(const ScopedRegistryKey&) = delete;

    [[nodiscard]] HKEY Get() const noexcept { return key_; }

private:
    HKEY key_{};
};

class CurrentUserRunRegistry final : public Registry::Interface {
public:
    explicit CurrentUserRunRegistry(const wchar_t* valueName) noexcept
        : valueName_(valueName) {}

    [[nodiscard]] LSTATUS QueryCommand(std::wstring& command) const noexcept override {
        try {
            command.clear();
            HKEY rawKey = nullptr;
            const LSTATUS openResult = ::RegOpenKeyExW(
                HKEY_CURRENT_USER,
                RunRegistrySubKey,
                0,
                KEY_QUERY_VALUE,
                &rawKey);
            if (openResult != ERROR_SUCCESS) {
                return openResult;
            }
            const ScopedRegistryKey key(rawKey);

            DWORD type = REG_NONE;
            DWORD byteCount = 0;
            LSTATUS queryResult = ::RegQueryValueExW(
                key.Get(),
                valueName_,
                nullptr,
                &type,
                nullptr,
                &byteCount);
            if (queryResult != ERROR_SUCCESS) {
                return queryResult;
            }
            if (type != REG_SZ) {
                return ERROR_UNSUPPORTED_TYPE;
            }

            for (int attempt = 0; attempt < 3; ++attempt) {
                const std::size_t characterCount =
                    std::max<std::size_t>(1, byteCount / sizeof(wchar_t) + 1);
                std::vector<wchar_t> buffer(characterCount, L'\0');
                DWORD currentType = REG_NONE;
                DWORD currentByteCount = static_cast<DWORD>(
                    std::min<std::size_t>(
                        buffer.size() * sizeof(wchar_t),
                        std::numeric_limits<DWORD>::max()));
                queryResult = ::RegQueryValueExW(
                    key.Get(),
                    valueName_,
                    nullptr,
                    &currentType,
                    reinterpret_cast<BYTE*>(buffer.data()),
                    &currentByteCount);
                if (queryResult == ERROR_MORE_DATA) {
                    byteCount = currentByteCount;
                    continue;
                }
                if (queryResult != ERROR_SUCCESS) {
                    return queryResult;
                }
                if (currentType != REG_SZ) {
                    return ERROR_UNSUPPORTED_TYPE;
                }
                buffer.back() = L'\0';
                command.assign(buffer.data());
                return ERROR_SUCCESS;
            }
            return ERROR_MORE_DATA;
        } catch (...) {
            command.clear();
            return ERROR_NOT_ENOUGH_MEMORY;
        }
    }

    [[nodiscard]] LSTATUS WriteCommand(const std::wstring_view command) noexcept override {
        if (command.size() >=
            (std::numeric_limits<DWORD>::max() / sizeof(wchar_t))) {
            return ERROR_INVALID_DATA;
        }

        HKEY rawKey = nullptr;
        const LSTATUS createResult = ::RegCreateKeyExW(
            HKEY_CURRENT_USER,
            RunRegistrySubKey,
            0,
            nullptr,
            REG_OPTION_NON_VOLATILE,
            KEY_SET_VALUE,
            nullptr,
            &rawKey,
            nullptr);
        if (createResult != ERROR_SUCCESS) {
            return createResult;
        }
        const ScopedRegistryKey key(rawKey);
        const DWORD byteCount = static_cast<DWORD>(
            (command.size() + 1) * sizeof(wchar_t));
        return ::RegSetValueExW(
            key.Get(),
            valueName_,
            0,
            REG_SZ,
            reinterpret_cast<const BYTE*>(command.data()),
            byteCount);
    }

    [[nodiscard]] LSTATUS DeleteCommand() noexcept override {
        HKEY rawKey = nullptr;
        const LSTATUS openResult = ::RegOpenKeyExW(
            HKEY_CURRENT_USER,
            RunRegistrySubKey,
            0,
            KEY_SET_VALUE,
            &rawKey);
        if (openResult != ERROR_SUCCESS) {
            return openResult;
        }
        const ScopedRegistryKey key(rawKey);
        return ::RegDeleteValueW(key.Get(), valueName_);
    }

private:
    const wchar_t* valueName_{};
};

[[nodiscard]] RegistrationResult Failure(
    const RegistrationOperation operation,
    const DWORD nativeError,
    std::wstring command = {}) noexcept {
    RegistrationResult result;
    result.operation = operation;
    result.nativeError = nativeError;
    result.command = std::move(command);
    return result;
}

}  // namespace

bool BuildQuotedExecutableCommand(
    const std::filesystem::path& executablePath,
    std::wstring* command,
    DWORD* error) noexcept {
    if (command != nullptr) {
        command->clear();
    }
    if (error != nullptr) {
        *error = ERROR_SUCCESS;
    }

    try {
        if (executablePath.empty() || !executablePath.is_absolute()) {
            if (error != nullptr) {
                *error = ERROR_BAD_PATHNAME;
            }
            return false;
        }

        const std::wstring normalized = executablePath.lexically_normal().wstring();
        if (normalized.empty() ||
            normalized.find(L'\0') != std::wstring::npos ||
            normalized.find(L'"') != std::wstring::npos) {
            if (error != nullptr) {
                *error = ERROR_INVALID_NAME;
            }
            return false;
        }

        constexpr std::size_t maximumCommandCharactersIncludingNull = 32'767;
        constexpr std::size_t quotingAndSeparatorCharacters = 3;
        const std::wstring_view argument(AutoStartArgument);
        const std::size_t maximumExecutableCharacters =
            maximumCommandCharactersIncludingNull - 1 - quotingAndSeparatorCharacters -
            argument.size();
        if (normalized.size() > maximumExecutableCharacters) {
            if (error != nullptr) {
                *error = ERROR_FILENAME_EXCED_RANGE;
            }
            return false;
        }

        if (command != nullptr) {
            command->reserve(normalized.size() + quotingAndSeparatorCharacters + argument.size());
            command->push_back(L'"');
            command->append(normalized);
            command->push_back(L'"');
            command->push_back(L' ');
            command->append(argument);
        }
        return true;
    } catch (...) {
        if (error != nullptr) {
            *error = ERROR_NOT_ENOUGH_MEMORY;
        }
        return false;
    }
}

std::filesystem::path CurrentExecutablePath(DWORD* error) noexcept {
    if (error != nullptr) {
        *error = ERROR_SUCCESS;
    }

    try {
        std::vector<wchar_t> buffer(512, L'\0');
        constexpr std::size_t maximumCharacters = 32'768;
        while (buffer.size() <= maximumCharacters) {
            ::SetLastError(ERROR_SUCCESS);
            const DWORD length = ::GetModuleFileNameW(
                nullptr,
                buffer.data(),
                static_cast<DWORD>(buffer.size()));
            if (length == 0) {
                if (error != nullptr) {
                    const DWORD nativeError = ::GetLastError();
                    *error = nativeError == ERROR_SUCCESS ? ERROR_GEN_FAILURE : nativeError;
                }
                return {};
            }
            if (length < buffer.size()) {
                const std::filesystem::path path(std::wstring(buffer.data(), length));
                if (!path.is_absolute()) {
                    if (error != nullptr) {
                        *error = ERROR_BAD_PATHNAME;
                    }
                    return {};
                }
                return path.lexically_normal();
            }
            if (buffer.size() == maximumCharacters) {
                break;
            }
            buffer.resize(std::min(maximumCharacters, buffer.size() * 2), L'\0');
        }
    } catch (...) {
        if (error != nullptr) {
            *error = ERROR_NOT_ENOUGH_MEMORY;
        }
        return {};
    }

    if (error != nullptr) {
        *error = ERROR_FILENAME_EXCED_RANGE;
    }
    return {};
}

RegistrationResult Reconcile(
    Registry::Interface& registry,
    const bool enabled,
    const std::filesystem::path& executablePath) noexcept {
    if (!enabled) {
        const LSTATUS deleteResult = registry.DeleteCommand();
        if (deleteResult == ERROR_FILE_NOT_FOUND || deleteResult == ERROR_PATH_NOT_FOUND) {
            RegistrationResult result;
            result.success = true;
            return result;
        }
        if (deleteResult != ERROR_SUCCESS) {
            return Failure(RegistrationOperation::DeleteRegistry, deleteResult);
        }
        RegistrationResult result;
        result.success = true;
        result.changed = true;
        result.operation = RegistrationOperation::DeleteRegistry;
        return result;
    }

    std::wstring expectedCommand;
    DWORD commandError = ERROR_SUCCESS;
    if (!BuildQuotedExecutableCommand(executablePath, &expectedCommand, &commandError)) {
        return Failure(
            RegistrationOperation::ValidateExecutable,
            commandError == ERROR_SUCCESS ? ERROR_INVALID_PARAMETER : commandError);
    }

    std::wstring currentCommand;
    const LSTATUS queryResult = registry.QueryCommand(currentCommand);
    if (queryResult == ERROR_SUCCESS && currentCommand == expectedCommand) {
        RegistrationResult result;
        result.success = true;
        result.command = std::move(expectedCommand);
        return result;
    }
    if (queryResult != ERROR_SUCCESS &&
        queryResult != ERROR_FILE_NOT_FOUND &&
        queryResult != ERROR_PATH_NOT_FOUND &&
        queryResult != ERROR_UNSUPPORTED_TYPE) {
        return Failure(
            RegistrationOperation::QueryRegistry,
            queryResult,
            std::move(expectedCommand));
    }

    const LSTATUS writeResult = registry.WriteCommand(expectedCommand);
    if (writeResult != ERROR_SUCCESS) {
        return Failure(
            RegistrationOperation::WriteRegistry,
            writeResult,
            std::move(expectedCommand));
    }

    RegistrationResult result;
    result.success = true;
    result.changed = true;
    result.operation = RegistrationOperation::WriteRegistry;
    result.command = std::move(expectedCommand);
    return result;
}

RegistrationResult ReconcileCurrentUser(const bool enabled) noexcept {
    CurrentUserRunRegistry registry(RunRegistryValueName);
    RegistrationResult result;
    if (!enabled) {
        result = Reconcile(registry, false, {});
    } else {
        DWORD pathError = ERROR_SUCCESS;
        const std::filesystem::path executablePath = CurrentExecutablePath(&pathError);
        if (executablePath.empty()) {
            return Failure(
                RegistrationOperation::ResolveExecutable,
                pathError == ERROR_SUCCESS ? ERROR_BAD_PATHNAME : pathError);
        }
        result = Reconcile(registry, true, executablePath);
    }
    if (!result.success) {
        return result;
    }

    CurrentUserRunRegistry legacyRegistry(LegacyRunRegistryValueName);
    const LSTATUS legacyDeleteResult = legacyRegistry.DeleteCommand();
    if (legacyDeleteResult == ERROR_SUCCESS) {
        result.changed = true;
        return result;
    }
    if (legacyDeleteResult == ERROR_FILE_NOT_FOUND ||
        legacyDeleteResult == ERROR_PATH_NOT_FOUND) {
        return result;
    }
    if (enabled) {
        result.legacyCleanupError = static_cast<DWORD>(legacyDeleteResult);
        return result;
    }
    return Failure(
        RegistrationOperation::DeleteLegacyRegistry,
        static_cast<DWORD>(legacyDeleteResult),
        std::move(result.command));
}

}  // namespace qrec::startup
