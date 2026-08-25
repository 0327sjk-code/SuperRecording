#include "update/detail/SelfUpdatePath.h"

#include <windows.h>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace qrec::update::detail {
namespace {

constexpr std::wstring_view ProductExecutableName = L"SuperRecording.exe";
constexpr std::wstring_view UpdateRootDirectoryName = L"SuperRecording-Update";

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

private:
    HANDLE handle_{};
};

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

[[nodiscard]] bool ValidateExecutableSyntax(
    const std::filesystem::path& path,
    const bool requireProductFileName,
    DWORD* error) {
    if (path.empty() || !path.is_absolute() || !path.has_root_name() ||
        !path.has_root_directory()) {
        if (error != nullptr) {
            *error = ERROR_BAD_PATHNAME;
        }
        return false;
    }

    const std::wstring& nativePath = path.native();
    if (nativePath.find(L'\0') != std::wstring::npos ||
        nativePath.find(L'"') != std::wstring::npos ||
        !EqualsInsensitive(path.extension().native(), L".exe") ||
        (requireProductFileName &&
         !EqualsInsensitive(path.filename().native(), ProductExecutableName))) {
        if (error != nullptr) {
            *error = ERROR_INVALID_NAME;
        }
        return false;
    }
    return true;
}

[[nodiscard]] bool ValidateDirectoryExists(
    const std::filesystem::path& directory,
    DWORD* error) noexcept {
    const DWORD attributes = ::GetFileAttributesW(directory.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES) {
        if (error != nullptr) {
            *error = ::GetLastError();
        }
        return false;
    }
    if ((attributes & FILE_ATTRIBUTE_DIRECTORY) == 0) {
        if (error != nullptr) {
            *error = ERROR_DIRECTORY;
        }
        return false;
    }
    return true;
}

[[nodiscard]] bool ValidateOwnedUpdateDirectoryChain(
    const std::filesystem::path& directory,
    const std::filesystem::path& updateRoot,
    DWORD* error) {
    std::filesystem::path current = directory.lexically_normal();
    for (;;) {
        const DWORD attributes = ::GetFileAttributesW(current.c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES) {
            if (error != nullptr) {
                *error = ::GetLastError();
            }
            return false;
        }
        if ((attributes & FILE_ATTRIBUTE_DIRECTORY) == 0 ||
            (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
            if (error != nullptr) {
                *error = ERROR_CANT_ACCESS_FILE;
            }
            return false;
        }
        if (PathsEqualInsensitive(current, updateRoot)) {
            return true;
        }
        const std::filesystem::path parent = current.parent_path();
        if (parent.empty() || parent == current) {
            if (error != nullptr) {
                *error = ERROR_BAD_PATHNAME;
            }
            return false;
        }
        current = parent;
    }
}

[[nodiscard]] bool ValidateRegularFileWithoutReparsePoint(
    const std::filesystem::path& file,
    DWORD* error) noexcept {
    const DWORD attributes = ::GetFileAttributesW(file.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES) {
        if (error != nullptr) {
            *error = ::GetLastError();
        }
        return false;
    }
    if ((attributes & FILE_ATTRIBUTE_DIRECTORY) != 0 ||
        (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
        if (error != nullptr) {
            *error = ERROR_BAD_EXE_FORMAT;
        }
        return false;
    }
    return true;
}

[[nodiscard]] bool QueryFileSize(
    const std::filesystem::path& path,
    std::uint64_t* size,
    DWORD* error) noexcept {
    const ScopedHandle file(::CreateFileW(
        path.c_str(),
        FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT,
        nullptr));
    if (file.Get() == INVALID_HANDLE_VALUE) {
        if (error != nullptr) {
            *error = ::GetLastError();
        }
        return false;
    }

    LARGE_INTEGER nativeSize{};
    if (::GetFileSizeEx(file.Get(), &nativeSize) == FALSE ||
        nativeSize.QuadPart <= 0) {
        if (error != nullptr) {
            const DWORD nativeError = ::GetLastError();
            *error = nativeError == ERROR_SUCCESS
                ? ERROR_BAD_EXE_FORMAT
                : nativeError;
        }
        return false;
    }
    if (size != nullptr) {
        *size = static_cast<std::uint64_t>(nativeSize.QuadPart);
    }
    return true;
}

[[nodiscard]] std::filesystem::path TemporaryUpdateRoot() {
    std::vector<wchar_t> buffer(MAX_PATH + 1, L'\0');
    for (;;) {
        const DWORD length = ::GetTempPathW(
            static_cast<DWORD>(buffer.size()),
            buffer.data());
        if (length == 0) {
            return {};
        }
        if (length < buffer.size()) {
            return (std::filesystem::path(std::wstring(buffer.data(), length)) /
                    UpdateRootDirectoryName)
                .lexically_normal();
        }
        if (length >= 32'767) {
            return {};
        }
        buffer.resize(static_cast<std::size_t>(length) + 1, L'\0');
    }
}

[[nodiscard]] bool IsStrictChildPath(
    const std::filesystem::path& candidate,
    const std::filesystem::path& root) noexcept {
    auto candidateIterator = candidate.begin();
    const auto candidateEnd = candidate.end();
    for (auto rootIterator = root.begin(); rootIterator != root.end();
         ++rootIterator) {
        if (candidateIterator == candidateEnd ||
            !EqualsInsensitive(
                rootIterator->native(),
                candidateIterator->native())) {
            return false;
        }
        ++candidateIterator;
    }
    return candidateIterator != candidateEnd;
}

}  // namespace

std::filesystem::path CurrentExecutablePath(DWORD* error) {
    if (error != nullptr) {
        *error = ERROR_SUCCESS;
    }

    std::vector<wchar_t> buffer(512, L'\0');
    constexpr std::size_t MaximumPathCharacters = 32'768;
    while (buffer.size() <= MaximumPathCharacters) {
        ::SetLastError(ERROR_SUCCESS);
        const DWORD length = ::GetModuleFileNameW(
            nullptr,
            buffer.data(),
            static_cast<DWORD>(buffer.size()));
        if (length == 0) {
            if (error != nullptr) {
                const DWORD nativeError = ::GetLastError();
                *error = nativeError == ERROR_SUCCESS
                    ? ERROR_GEN_FAILURE
                    : nativeError;
            }
            return {};
        }
        if (length < buffer.size()) {
            return std::filesystem::path(std::wstring(buffer.data(), length))
                .lexically_normal();
        }
        if (buffer.size() == MaximumPathCharacters) {
            break;
        }
        buffer.resize(
            std::min(MaximumPathCharacters, buffer.size() * 2),
            L'\0');
    }

    if (error != nullptr) {
        *error = ERROR_FILENAME_EXCED_RANGE;
    }
    return {};
}

bool ValidateExecutablePath(
    const std::filesystem::path& executable,
    const bool requireProductFileName,
    DWORD* error) {
    if (error != nullptr) {
        *error = ERROR_SUCCESS;
    }
    return ValidateExecutableSyntax(
               executable,
               requireProductFileName,
               error) &&
        ValidateRegularFileWithoutReparsePoint(executable, error) &&
        ValidateDirectoryExists(executable.parent_path(), error);
}

bool PathsEqualInsensitive(
    const std::filesystem::path& left,
    const std::filesystem::path& right) noexcept {
    return EqualsInsensitive(left.native(), right.native());
}

std::filesystem::path BackupPathFor(
    const std::filesystem::path& targetExecutable) {
    return targetExecutable.parent_path() /
        (targetExecutable.stem().native() + L".old" +
         targetExecutable.extension().native());
}

bool VerifyInstalledCopy(
    const std::filesystem::path& source,
    const std::filesystem::path& target,
    DWORD* error) noexcept {
    std::uint64_t sourceSize = 0;
    std::uint64_t targetSize = 0;
    return QueryFileSize(source, &sourceSize, error) &&
        QueryFileSize(target, &targetSize, error) && sourceSize == targetSize;
}

bool NormalizeInstalledExecutableAttributes(
    const std::filesystem::path& executable,
    DWORD* error) noexcept {
    if (error != nullptr) {
        *error = ERROR_SUCCESS;
    }

    const DWORD attributes = ::GetFileAttributesW(executable.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES) {
        if (error != nullptr) {
            *error = ::GetLastError();
        }
        return false;
    }
    if ((attributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) != 0) {
        if (error != nullptr) {
            *error = ERROR_CANT_ACCESS_FILE;
        }
        return false;
    }
    if ((attributes & FILE_ATTRIBUTE_TEMPORARY) == 0) {
        return true;
    }

    DWORD normalizedAttributes = attributes & ~FILE_ATTRIBUTE_TEMPORARY;
    if (normalizedAttributes == 0) {
        normalizedAttributes = FILE_ATTRIBUTE_NORMAL;
    }
    if (::SetFileAttributesW(
            executable.c_str(), normalizedAttributes) == FALSE) {
        if (error != nullptr) {
            *error = ::GetLastError();
        }
        return false;
    }

    const DWORD verifiedAttributes = ::GetFileAttributesW(executable.c_str());
    if (verifiedAttributes == INVALID_FILE_ATTRIBUTES ||
        (verifiedAttributes & FILE_ATTRIBUTE_TEMPORARY) != 0) {
        if (error != nullptr) {
            *error = verifiedAttributes == INVALID_FILE_ATTRIBUTES
                ? ::GetLastError()
                : ERROR_INVALID_DATA;
        }
        return false;
    }
    return true;
}

bool IsOwnedTemporaryUpdateExecutable(
    const std::filesystem::path& executable,
    DWORD* error) {
    const std::filesystem::path updateRoot = TemporaryUpdateRoot();
    return !updateRoot.empty() &&
        ValidateExecutablePath(executable, true, error) &&
        IsStrictChildPath(executable.parent_path(), updateRoot) &&
        ValidateOwnedUpdateDirectoryChain(
            executable.parent_path(),
            updateRoot,
            error);
}

void RemoveFileBestEffort(const std::filesystem::path& path) noexcept {
    if (::DeleteFileW(path.c_str()) == FALSE) {
        static_cast<void>(::MoveFileExW(
            path.c_str(),
            nullptr,
            MOVEFILE_DELAY_UNTIL_REBOOT));
    }
}

}  // namespace qrec::update::detail
