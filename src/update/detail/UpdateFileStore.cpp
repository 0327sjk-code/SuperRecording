#include "update/detail/UpdateFileStore.h"

#include "update/detail/UpdateError.h"

#include <windows.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <new>
#include <system_error>
#include <utility>

namespace qrec::update::detail {
namespace {

class FileHandle final {
public:
    explicit FileHandle(const HANDLE handle = INVALID_HANDLE_VALUE) noexcept
        : handle_(handle) {}
    ~FileHandle() { Close(); }
    FileHandle(const FileHandle&) = delete;
    FileHandle& operator=(const FileHandle&) = delete;
    [[nodiscard]] HANDLE Get() const noexcept { return handle_; }
    [[nodiscard]] explicit operator bool() const noexcept {
        return handle_ != nullptr && handle_ != INVALID_HANDLE_VALUE;
    }
    void Close() noexcept {
        if (*this) {
            static_cast<void>(::CloseHandle(handle_));
        }
        handle_ = INVALID_HANDLE_VALUE;
    }

private:
    HANDLE handle_{INVALID_HANDLE_VALUE};
};

class DeleteFileGuard final {
public:
    explicit DeleteFileGuard(std::filesystem::path path)
        : path_(std::move(path)) {}
    ~DeleteFileGuard() {
        if (active_ && !path_.empty()) {
            static_cast<void>(::DeleteFileW(path_.c_str()));
        }
    }
    DeleteFileGuard(const DeleteFileGuard&) = delete;
    DeleteFileGuard& operator=(const DeleteFileGuard&) = delete;
    void Release() noexcept { active_ = false; }

private:
    std::filesystem::path path_;
    bool active_{true};
};

bool ReadExact(
    const HANDLE file,
    void* const buffer,
    const DWORD byteCount) noexcept {
    DWORD bytesRead = 0;
    return ::ReadFile(file, buffer, byteCount, &bytesRead, nullptr) &&
        bytesRead == byteCount;
}

bool ValidatePortableExecutable(
    const std::filesystem::path& path,
    std::uint64_t* const fileBytes,
    UpdateFailure* const failure) {
    FileHandle file(::CreateFileW(
        path.c_str(),
        GENERIC_READ,
        FILE_SHARE_READ,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN,
        nullptr));
    if (!file) {
        if (failure != nullptr) {
            *failure = MakeNativeFailure(
                UpdateErrorCode::InvalidExecutable,
                L"Could not open the downloaded executable",
                ::GetLastError());
        }
        return false;
    }

    LARGE_INTEGER size{};
    if (!::GetFileSizeEx(file.Get(), &size)) {
        if (failure != nullptr) {
            *failure = MakeNativeFailure(
                UpdateErrorCode::InvalidExecutable,
                L"Could not determine the downloaded executable size",
                ::GetLastError());
        }
        return false;
    }
    if (size.QuadPart <= 0) {
        if (failure != nullptr) {
            *failure = MakeFailure(
                UpdateErrorCode::InvalidExecutable,
                L"The downloaded executable is empty.");
        }
        return false;
    }

    IMAGE_DOS_HEADER dosHeader{};
    if (!ReadExact(
            file.Get(), &dosHeader,
            static_cast<DWORD>(sizeof(dosHeader))) ||
        dosHeader.e_magic != IMAGE_DOS_SIGNATURE ||
        dosHeader.e_lfanew <= 0) {
        if (failure != nullptr) {
            *failure = MakeFailure(
                UpdateErrorCode::InvalidExecutable,
                L"The downloaded file is not a valid PE executable.");
        }
        return false;
    }
    const std::int64_t ntHeaderEnd =
        static_cast<std::int64_t>(dosHeader.e_lfanew) +
        static_cast<std::int64_t>(sizeof(DWORD)) +
        static_cast<std::int64_t>(sizeof(IMAGE_FILE_HEADER)) +
        static_cast<std::int64_t>(sizeof(WORD));
    if (ntHeaderEnd > size.QuadPart) {
        if (failure != nullptr) {
            *failure = MakeFailure(
                UpdateErrorCode::InvalidExecutable,
                L"The downloaded PE header is truncated.");
        }
        return false;
    }

    LARGE_INTEGER ntOffset{};
    ntOffset.QuadPart = dosHeader.e_lfanew;
    if (!::SetFilePointerEx(file.Get(), ntOffset, nullptr, FILE_BEGIN)) {
        if (failure != nullptr) {
            *failure = MakeNativeFailure(
                UpdateErrorCode::InvalidExecutable,
                L"Could not seek to the downloaded PE header",
                ::GetLastError());
        }
        return false;
    }
    DWORD signature = 0;
    IMAGE_FILE_HEADER fileHeader{};
    WORD optionalMagic = 0;
    if (!ReadExact(file.Get(), &signature, sizeof(signature)) ||
        !ReadExact(
            file.Get(), &fileHeader,
            static_cast<DWORD>(sizeof(fileHeader))) ||
        !ReadExact(file.Get(), &optionalMagic, sizeof(optionalMagic)) ||
        signature != IMAGE_NT_SIGNATURE ||
        fileHeader.Machine != IMAGE_FILE_MACHINE_AMD64 ||
        fileHeader.SizeOfOptionalHeader < sizeof(optionalMagic) ||
        optionalMagic != IMAGE_NT_OPTIONAL_HDR64_MAGIC) {
        if (failure != nullptr) {
            *failure = MakeFailure(
                UpdateErrorCode::InvalidExecutable,
                L"The downloaded file is not a valid x64 PE executable.");
        }
        return false;
    }
    if (fileBytes != nullptr) {
        *fileBytes = static_cast<std::uint64_t>(size.QuadPart);
    }
    return true;
}

std::optional<std::filesystem::path> BuildDestinationPath(
    const SemanticVersion& version,
    UpdateFailure* const failure) {
    std::array<wchar_t, 32'768> tempPath{};
    const DWORD pathLength = ::GetTempPathW(
        static_cast<DWORD>(tempPath.size()), tempPath.data());
    if (pathLength == 0 || pathLength >= tempPath.size()) {
        if (failure != nullptr) {
            *failure = MakeNativeFailure(
                UpdateErrorCode::FileSystem,
                L"Could not resolve the temporary directory",
                ::GetLastError());
        }
        return std::nullopt;
    }
    return std::filesystem::path(tempPath.data()) /
        L"SuperRecording-Update" /
        version.ToWString() /
        L"SuperRecording.exe";
}

ExecutableDownloadResult AcquireImpl(
    const SemanticVersion& version,
    const std::uint64_t maximumBytes,
    const HttpTransferOperation& transfer) {
    UpdateFailure pathFailure;
    const std::optional<std::filesystem::path> destination =
        BuildDestinationPath(version, &pathFailure);
    if (!destination.has_value()) {
        return {{}, 0, false, std::move(pathFailure)};
    }

    std::uint64_t cachedBytes = 0;
    UpdateFailure ignoredValidationFailure;
    if (ValidatePortableExecutable(
            *destination, &cachedBytes, &ignoredValidationFailure) &&
        cachedBytes <= maximumBytes) {
        return {*destination, cachedBytes, true, {}};
    }

    std::error_code directoryError;
    std::filesystem::create_directories(
        destination->parent_path(), directoryError);
    if (directoryError) {
        return {
            {}, 0, false,
            MakeFailure(
                UpdateErrorCode::FileSystem,
                L"Could not create the update download directory.",
                static_cast<std::uint32_t>(directoryError.value()))};
    }

    std::filesystem::path partialPath = *destination;
    partialPath += L".part";
    static_cast<void>(::DeleteFileW(partialPath.c_str()));
    DeleteFileGuard partialCleanup(partialPath);
    FileHandle partialFile(::CreateFileW(
        partialPath.c_str(),
        GENERIC_WRITE,
        0,
        nullptr,
        CREATE_ALWAYS,
        FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_SEQUENTIAL_SCAN,
        nullptr));
    if (!partialFile) {
        return {
            {}, 0, false,
            MakeNativeFailure(
                UpdateErrorCode::FileSystem,
                L"Could not create the partial update file",
                ::GetLastError())};
    }

    const HttpTransferResult transferResult = transfer(
        [&partialFile](
            const std::span<const std::byte> chunk,
            UpdateFailure* const failure) {
            if (chunk.size() > std::numeric_limits<DWORD>::max()) {
                if (failure != nullptr) {
                    *failure = MakeFailure(
                        UpdateErrorCode::FileSystem,
                        L"The update write chunk is too large.");
                }
                return false;
            }
            DWORD bytesWritten = 0;
            const bool wrote = ::WriteFile(
                partialFile.Get(), chunk.data(),
                static_cast<DWORD>(chunk.size()),
                &bytesWritten, nullptr) != FALSE;
            if (!wrote ||
                static_cast<std::size_t>(bytesWritten) != chunk.size()) {
                if (failure != nullptr) {
                    *failure = wrote
                        ? MakeFailure(
                            UpdateErrorCode::FileSystem,
                            L"The partial update file was only partially written.",
                            ERROR_WRITE_FAULT)
                        : MakeNativeFailure(
                            UpdateErrorCode::FileSystem,
                            L"Could not write the partial update file",
                            ::GetLastError());
                }
                return false;
            }
            return true;
        });
    if (transferResult.failure.HasError()) {
        return {{}, 0, false, transferResult.failure};
    }
    if (!::FlushFileBuffers(partialFile.Get())) {
        return {
            {}, 0, false,
            MakeNativeFailure(
                UpdateErrorCode::FileSystem,
                L"Could not flush the partial update file",
                ::GetLastError())};
    }
    partialFile.Close();

    std::uint64_t validatedBytes = 0;
    UpdateFailure validationFailure;
    if (!ValidatePortableExecutable(
            partialPath, &validatedBytes, &validationFailure) ||
        validatedBytes != transferResult.bytesTransferred) {
        if (!validationFailure.HasError()) {
            validationFailure = MakeFailure(
                UpdateErrorCode::InvalidExecutable,
                L"The downloaded executable size changed unexpectedly.");
        }
        return {{}, 0, false, std::move(validationFailure)};
    }
    if (!::MoveFileExW(
            partialPath.c_str(),
            destination->c_str(),
            MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        return {
            {}, 0, false,
            MakeNativeFailure(
                UpdateErrorCode::FileSystem,
                L"Could not publish the downloaded update",
                ::GetLastError())};
    }
    partialCleanup.Release();
    return {*destination, transferResult.bytesTransferred, false, {}};
}

}  // namespace

UpdateFileStore::UpdateFileStore(const std::uint64_t maximumBytes) noexcept
    : maximumBytes_(maximumBytes) {}

ExecutableDownloadResult UpdateFileStore::Acquire(
    const SemanticVersion& version,
    const HttpTransferOperation& transfer) const noexcept {
    try {
        if (maximumBytes_ == 0 || !transfer) {
            return {
                {}, 0, false,
                MakeFailure(
                    UpdateErrorCode::InvalidConfiguration,
                    L"The update file-store configuration is invalid.")};
        }
        return AcquireImpl(version, maximumBytes_, transfer);
    } catch (const std::bad_alloc&) {
        return {
            {}, 0, false,
            MakeFailure(
                UpdateErrorCode::OutOfMemory,
                L"Not enough memory to store the update executable.")};
    } catch (...) {
        return {
            {}, 0, false,
            MakeFailure(
                UpdateErrorCode::Unexpected,
                L"Unexpected failure while storing the update executable.")};
    }
}

}  // namespace qrec::update::detail
