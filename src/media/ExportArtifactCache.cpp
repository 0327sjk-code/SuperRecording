#include "media/ExportArtifactCache.h"

#include "common/ProductInfo.h"
#include "common/Win32Helpers.h"
#include "media/ExportQuality.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <condition_variable>
#include <cwctype>
#include <exception>
#include <format>
#include <fstream>
#include <limits>
#include <mutex>
#include <system_error>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace qrec {
namespace {

using namespace std::chrono_literals;

constexpr std::wstring_view kArtifactPrefix = L"qrec-artifact-";
constexpr std::wstring_view kStagingPrefix = L"qrec-cache-staging-";
constexpr std::wstring_view kCacheVersion =
    L"qrec-export-cache-v10-quality-percent";
constexpr auto kArtifactRetention = std::chrono::hours(24 * 7);
constexpr auto kStagingRetention = std::chrono::hours(24);

std::atomic_uint64_t gStagingSequence{0};

struct SourceIdentity final {
    std::wstring accessPath;
    std::wstring normalizedPath;
    std::uint64_t size{};
    std::uint64_t lastWriteTime{};
    std::uint64_t volumeSerialNumber{};
    std::uint64_t fileId{};
    std::uint64_t changeTime{};
};

struct FileFingerprint final {
    std::uint64_t size{};
    std::uint64_t lastWriteTime{};
};

template <typename Callback>
class ScopeExit final {
public:
    explicit ScopeExit(Callback callback) noexcept
        : callback_(std::move(callback)) {}

    ~ScopeExit() noexcept {
        if (!active_) {
            return;
        }
        try {
            callback_();
        } catch (...) {
            // Cleanup guards must not throw during stack unwinding.
        }
    }

    ScopeExit(const ScopeExit&) = delete;
    ScopeExit& operator=(const ScopeExit&) = delete;

    void Release() noexcept { active_ = false; }

private:
    Callback callback_;
    bool active_{true};
};

[[nodiscard]] std::wstring CanonicalExistingPath(
    const std::filesystem::path& sourcePath,
    std::error_code* error) {
    std::error_code localError;
    std::filesystem::path normalized = std::filesystem::weakly_canonical(
        sourcePath,
        localError);
    if (localError) {
        localError.clear();
        normalized = std::filesystem::absolute(sourcePath, localError).lexically_normal();
    }
    if (localError) {
        if (error != nullptr) {
            *error = localError;
        }
        return {};
    }

    if (error != nullptr) {
        error->clear();
    }
    return normalized.native();
}

[[nodiscard]] bool ReadSourceIdentity(
    const std::filesystem::path& sourcePath,
    SourceIdentity* identity,
    std::wstring* errorMessage) noexcept {
    if (identity == nullptr || sourcePath.empty()) {
        if (errorMessage != nullptr) {
            *errorMessage = L"源录屏路径为空。";
        }
        return false;
    }

    try {
        std::error_code pathError;
        const std::wstring accessPath = CanonicalExistingPath(sourcePath, &pathError);
        if (pathError || accessPath.empty()) {
            if (errorMessage != nullptr) {
                *errorMessage = L"无法规范化源录屏路径：" + sourcePath.wstring();
            }
            return false;
        }

        HANDLE sourceHandle = ::CreateFileW(
            accessPath.c_str(),
            FILE_READ_ATTRIBUTES,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            nullptr,
            OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL,
            nullptr);
        if (sourceHandle == INVALID_HANDLE_VALUE) {
            if (errorMessage != nullptr) {
                *errorMessage = L"无法读取源录屏身份：" + win32::FormatLastError();
            }
            return false;
        }
        ScopeExit closeSourceHandle([sourceHandle] {
            ::CloseHandle(sourceHandle);
        });

        BY_HANDLE_FILE_INFORMATION fileInformation{};
        FILE_BASIC_INFO basicInformation{};
        if (::GetFileInformationByHandle(
                sourceHandle,
                &fileInformation) == FALSE ||
            ::GetFileInformationByHandleEx(
                sourceHandle,
                FileBasicInfo,
                &basicInformation,
                sizeof(basicInformation)) == FALSE ||
            (fileInformation.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
            if (errorMessage != nullptr) {
                *errorMessage = L"无法读取源录屏文件标识：" +
                    win32::FormatLastError();
            }
            return false;
        }

        ULARGE_INTEGER size{};
        size.HighPart = fileInformation.nFileSizeHigh;
        size.LowPart = fileInformation.nFileSizeLow;
        ULARGE_INTEGER lastWriteTime{};
        lastWriteTime.HighPart = fileInformation.ftLastWriteTime.dwHighDateTime;
        lastWriteTime.LowPart = fileInformation.ftLastWriteTime.dwLowDateTime;
        ULARGE_INTEGER fileId{};
        fileId.HighPart = fileInformation.nFileIndexHigh;
        fileId.LowPart = fileInformation.nFileIndexLow;

        identity->accessPath = accessPath;
        identity->normalizedPath = accessPath;
        std::transform(
            identity->normalizedPath.begin(),
            identity->normalizedPath.end(),
            identity->normalizedPath.begin(),
            [](const wchar_t character) {
                return static_cast<wchar_t>(std::towlower(character));
            });
        identity->size = size.QuadPart;
        identity->lastWriteTime = lastWriteTime.QuadPart;
        identity->volumeSerialNumber = fileInformation.dwVolumeSerialNumber;
        identity->fileId = fileId.QuadPart;
        identity->changeTime = static_cast<std::uint64_t>(
            basicInformation.ChangeTime.QuadPart);
        return true;
    } catch (const std::exception&) {
        if (errorMessage != nullptr) {
            *errorMessage = L"读取源录屏身份时发生异常。";
        }
        return false;
    } catch (...) {
        if (errorMessage != nullptr) {
            *errorMessage = L"读取源录屏身份时发生未知异常。";
        }
        return false;
    }
}

[[nodiscard]] bool ReadFileFingerprint(
    const std::filesystem::path& path,
    FileFingerprint* fingerprint) noexcept {
    if (fingerprint == nullptr || path.empty()) {
        return false;
    }
    WIN32_FILE_ATTRIBUTE_DATA attributes{};
    if (::GetFileAttributesExW(
            path.c_str(),
            GetFileExInfoStandard,
            &attributes) == FALSE ||
        (attributes.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
        return false;
    }
    ULARGE_INTEGER size{};
    size.HighPart = attributes.nFileSizeHigh;
    size.LowPart = attributes.nFileSizeLow;
    ULARGE_INTEGER lastWriteTime{};
    lastWriteTime.HighPart = attributes.ftLastWriteTime.dwHighDateTime;
    lastWriteTime.LowPart = attributes.ftLastWriteTime.dwLowDateTime;
    fingerprint->size = size.QuadPart;
    fingerprint->lastWriteTime = lastWriteTime.QuadPart;
    return true;
}

[[nodiscard]] std::wstring SerializeKey(const ExportArtifactCacheKey& key) {
    return std::format(
        L"{}|{}:{}|{}|{}|{}|{}|{}|{}|{}:{}|{}|{}|{}|{}|{}|{}|{}|{}|{}|{}",
        kCacheVersion,
        key.normalizedSourcePath.size(),
        key.normalizedSourcePath,
        key.sourceSize,
        key.sourceLastWriteTime,
        key.sourceVolumeSerialNumber,
        key.sourceFileId,
        key.sourceChangeTime,
        key.includeSystemAudio ? 1 : 0,
        key.normalizedAudioPath.size(),
        key.normalizedAudioPath,
        key.audioSize,
        key.audioLastWriteTime,
        key.audioVolumeSerialNumber,
        key.audioFileId,
        key.audioChangeTime,
        key.trimStartMilliseconds,
        key.trimEndMilliseconds,
        static_cast<unsigned int>(key.format),
        key.playbackSpeedTenths,
        key.qualityPercent);
}

[[nodiscard]] std::wstring SerializeManifest(
    const std::wstring_view serializedKey,
    const FileFingerprint& fingerprint) {
    return std::format(
        L"{}|artifactSize={}|artifactLastWriteTime={}",
        serializedKey,
        fingerprint.size,
        fingerprint.lastWriteTime);
}

[[nodiscard]] std::uint64_t StableHash(
    const std::wstring_view text,
    const std::uint64_t seed) noexcept {
    constexpr std::uint64_t prime = 1'099'511'628'211ULL;
    std::uint64_t hash = seed;
    for (const wchar_t character : text) {
        const auto value = static_cast<std::uint32_t>(character);
        hash ^= value & 0xFFU;
        hash *= prime;
        hash ^= (value >> 8U) & 0xFFU;
        hash *= prime;
        hash ^= (value >> 16U) & 0xFFU;
        hash *= prime;
        hash ^= (value >> 24U) & 0xFFU;
        hash *= prime;
    }
    return hash;
}

[[nodiscard]] std::uint64_t HashUnsignedValue(
    std::uint64_t hash,
    const std::uint64_t value) noexcept {
    constexpr std::uint64_t prime = 1'099'511'628'211ULL;
    for (unsigned int shift = 0; shift < 64; shift += 8) {
        hash ^= (value >> shift) & 0xFFULL;
        hash *= prime;
    }
    return hash;
}

[[nodiscard]] std::uint64_t StableKeyHash(
    const ExportArtifactCacheKey& key,
    const std::uint64_t seed) noexcept {
    std::uint64_t hash = StableHash(kCacheVersion, seed);
    hash = StableHash(key.normalizedSourcePath, hash);
    hash = HashUnsignedValue(hash, key.sourceSize);
    hash = HashUnsignedValue(hash, key.sourceLastWriteTime);
    hash = HashUnsignedValue(hash, key.sourceVolumeSerialNumber);
    hash = HashUnsignedValue(hash, key.sourceFileId);
    hash = HashUnsignedValue(hash, key.sourceChangeTime);
    hash = HashUnsignedValue(hash, key.includeSystemAudio ? 1ULL : 0ULL);
    hash = StableHash(key.normalizedAudioPath, hash);
    hash = HashUnsignedValue(hash, key.audioSize);
    hash = HashUnsignedValue(hash, key.audioLastWriteTime);
    hash = HashUnsignedValue(hash, key.audioVolumeSerialNumber);
    hash = HashUnsignedValue(hash, key.audioFileId);
    hash = HashUnsignedValue(hash, key.audioChangeTime);
    hash = HashUnsignedValue(
        hash,
        static_cast<std::uint64_t>(key.trimStartMilliseconds));
    hash = HashUnsignedValue(
        hash,
        static_cast<std::uint64_t>(key.trimEndMilliseconds));
    hash = HashUnsignedValue(hash, static_cast<std::uint64_t>(key.format));
    hash = HashUnsignedValue(
        hash,
        static_cast<std::uint64_t>(key.playbackSpeedTenths));
    return HashUnsignedValue(
        hash,
        static_cast<std::uint64_t>(key.qualityPercent));
}

[[nodiscard]] std::wstring NormalizeExtension(const std::wstring_view extension) {
    if (extension.empty()) {
        return {};
    }
    std::wstring normalized(extension);
    if (normalized.front() != L'.') {
        normalized.insert(normalized.begin(), L'.');
    }
    std::transform(
        normalized.begin(),
        normalized.end(),
        normalized.begin(),
        [](const wchar_t character) {
            return static_cast<wchar_t>(std::towlower(character));
        });
    if (normalized.find_first_of(L"\\/:") != std::wstring::npos) {
        return {};
    }
    return normalized;
}

[[nodiscard]] bool IsNonEmptyRegularFile(
    const std::filesystem::path& path) noexcept {
    std::error_code error;
    return std::filesystem::is_regular_file(path, error) && !error &&
        std::filesystem::file_size(path, error) > 0 && !error;
}

[[nodiscard]] std::uint32_t ReadBigEndian32(
    const std::array<unsigned char, 16>& bytes,
    const std::size_t offset) noexcept {
    return (static_cast<std::uint32_t>(bytes[offset]) << 24U) |
        (static_cast<std::uint32_t>(bytes[offset + 1]) << 16U) |
        (static_cast<std::uint32_t>(bytes[offset + 2]) << 8U) |
        static_cast<std::uint32_t>(bytes[offset + 3]);
}

[[nodiscard]] std::uint64_t ReadBigEndian64(
    const std::array<unsigned char, 16>& bytes,
    const std::size_t offset) noexcept {
    std::uint64_t value = 0;
    for (std::size_t index = 0; index < 8; ++index) {
        value = (value << 8U) | bytes[offset + index];
    }
    return value;
}

[[nodiscard]] bool ValidateMp4Structure(
    const std::filesystem::path& path,
    const std::uint64_t fileSize) noexcept {
    if (fileSize < 24) {
        return false;
    }
    try {
        std::ifstream input(path, std::ios::binary);
        if (!input) {
            return false;
        }

        bool hasFileType = false;
        bool hasMovie = false;
        bool hasMediaData = false;
        std::uint64_t position = 0;
        while (position < fileSize) {
            if (fileSize - position < 8 ||
                position > static_cast<std::uint64_t>(
                    std::numeric_limits<std::streamoff>::max())) {
                return false;
            }
            input.seekg(static_cast<std::streamoff>(position), std::ios::beg);
            std::array<unsigned char, 16> header{};
            input.read(reinterpret_cast<char*>(header.data()), 8);
            if (input.gcount() != 8) {
                return false;
            }

            std::uint64_t boxSize = ReadBigEndian32(header, 0);
            std::uint64_t headerSize = 8;
            if (boxSize == 1) {
                input.read(reinterpret_cast<char*>(header.data() + 8), 8);
                if (input.gcount() != 8) {
                    return false;
                }
                boxSize = ReadBigEndian64(header, 8);
                headerSize = 16;
            } else if (boxSize == 0) {
                boxSize = fileSize - position;
            }
            if (boxSize < headerSize || boxSize > fileSize - position) {
                return false;
            }

            const std::array<char, 4> type{
                static_cast<char>(header[4]),
                static_cast<char>(header[5]),
                static_cast<char>(header[6]),
                static_cast<char>(header[7])};
            hasFileType = hasFileType || type == std::array<char, 4>{'f', 't', 'y', 'p'};
            hasMovie = hasMovie || type == std::array<char, 4>{'m', 'o', 'o', 'v'};
            hasMediaData = hasMediaData || type == std::array<char, 4>{'m', 'd', 'a', 't'};
            position += boxSize;
        }
        return position == fileSize && hasFileType && hasMovie && hasMediaData;
    } catch (...) {
        return false;
    }
}

[[nodiscard]] bool ValidateGifStructure(
    const std::filesystem::path& path,
    const std::uint64_t fileSize) noexcept {
    if (fileSize < 14 || fileSize > static_cast<std::uint64_t>(
            std::numeric_limits<std::streamoff>::max())) {
        return false;
    }
    try {
        std::ifstream input(path, std::ios::binary);
        std::array<char, 6> signatureBytes{};
        input.read(
            signatureBytes.data(),
            static_cast<std::streamsize>(signatureBytes.size()));
        if (input.gcount() !=
            static_cast<std::streamsize>(signatureBytes.size())) {
            return false;
        }
        const std::string signature(signatureBytes.data(), signatureBytes.size());
        if (signature != "GIF89a" && signature != "GIF87a") {
            return false;
        }
        input.seekg(static_cast<std::streamoff>(fileSize - 1), std::ios::beg);
        char trailer = 0;
        input.read(&trailer, 1);
        return input.gcount() == 1 &&
            static_cast<unsigned char>(trailer) == 0x3B;
    } catch (...) {
        return false;
    }
}

[[nodiscard]] bool HasExpectedSignature(
    const std::filesystem::path& path,
    const std::wstring_view normalizedExtension) noexcept {
    if (!IsNonEmptyRegularFile(path)) {
        return false;
    }
    std::error_code sizeError;
    const std::uintmax_t size = std::filesystem::file_size(path, sizeError);
    if (sizeError) {
        return false;
    }
    const auto fileSize = static_cast<std::uint64_t>(size);
    if (normalizedExtension == L".mp4") {
        return ValidateMp4Structure(path, fileSize);
    }
    if (normalizedExtension == L".gif") {
        return ValidateGifStructure(path, fileSize);
    }
    return true;
}

[[nodiscard]] bool ManifestMatches(
    const std::filesystem::path& manifestPath,
    const std::wstring& serializedKey,
    const std::filesystem::path& artifactPath) noexcept {
    try {
        FileFingerprint fingerprint{};
        if (!ReadFileFingerprint(artifactPath, &fingerprint)) {
            return false;
        }
        const std::wstring expectedManifest = SerializeManifest(
            serializedKey,
            fingerprint);
        std::error_code sizeError;
        const std::uintmax_t fileSize = std::filesystem::file_size(
            manifestPath,
            sizeError);
        const std::uintmax_t expectedSize =
            expectedManifest.size() * sizeof(wchar_t);
        if (sizeError || fileSize != expectedSize) {
            return false;
        }

        std::ifstream input(manifestPath, std::ios::binary);
        if (!input) {
            return false;
        }
        std::wstring stored(expectedManifest.size(), L'\0');
        input.read(
            reinterpret_cast<char*>(stored.data()),
            static_cast<std::streamsize>(expectedSize));
        return input.good() && stored == expectedManifest;
    } catch (...) {
        return false;
    }
}

[[nodiscard]] bool WriteManifest(
    const std::filesystem::path& path,
    const std::wstring& serializedKey,
    std::wstring* errorMessage) noexcept {
    try {
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        if (!output) {
            if (errorMessage != nullptr) {
                *errorMessage = L"无法创建导出缓存索引。";
            }
            return false;
        }
        output.write(
            reinterpret_cast<const char*>(serializedKey.data()),
            static_cast<std::streamsize>(serializedKey.size() * sizeof(wchar_t)));
        output.flush();
        if (!output.good()) {
            if (errorMessage != nullptr) {
                *errorMessage = L"无法写入导出缓存索引。";
            }
            return false;
        }
        return true;
    } catch (...) {
        if (errorMessage != nullptr) {
            *errorMessage = L"写入导出缓存索引时发生异常。";
        }
        return false;
    }
}

[[nodiscard]] std::filesystem::path UniqueStagingPath(
    const std::filesystem::path& root,
    const std::wstring_view keyId,
    const std::wstring_view suffix) {
    const std::uint64_t sequence = gStagingSequence.fetch_add(
        1,
        std::memory_order_relaxed);
    return root / std::format(
        L"{}{}-{}-{}{}",
        kStagingPrefix,
        keyId,
        ::GetCurrentProcessId(),
        sequence,
        suffix);
}

void RemoveIfPresent(const std::filesystem::path& path) noexcept {
    std::error_code ignored;
    std::filesystem::remove(path, ignored);
}

[[nodiscard]] bool IsDirectoryWritable(
    const std::filesystem::path& directory,
    std::wstring* errorMessage) noexcept {
    std::wstring directoryError;
    if (!win32::EnsureDirectory(directory, &directoryError)) {
        if (errorMessage != nullptr) {
            *errorMessage = std::move(directoryError);
        }
        return false;
    }

    const std::filesystem::path probePath = UniqueStagingPath(
        directory,
        L"write-probe",
        L".tmp");
    HANDLE probe = ::CreateFileW(
        probePath.c_str(),
        GENERIC_WRITE | DELETE,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr,
        CREATE_NEW,
        FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE,
        nullptr);
    if (probe == INVALID_HANDLE_VALUE) {
        if (errorMessage != nullptr) {
            *errorMessage = L"导出缓存目录不可写：" + win32::FormatLastError();
        }
        return false;
    }
    ::CloseHandle(probe);
    return true;
}

[[nodiscard]] std::filesystem::path PreferredRootForKey(
    const ExportArtifactCacheKey& key) {
    const std::filesystem::path sourceDirectory =
        std::filesystem::path(
            key.sourceAccessPath.empty()
                ? key.normalizedSourcePath
                : key.sourceAccessPath).parent_path();
    if (_wcsicmp(
            sourceDirectory.filename().c_str(),
            product::RecordingCacheDirectoryName) == 0 ||
        _wcsicmp(
            sourceDirectory.filename().c_str(),
            product::LegacyRecordingCacheDirectoryName) == 0) {
        return sourceDirectory;
    }
    return sourceDirectory / product::RecordingCacheDirectoryName;
}

void CleanupCacheDirectory(const std::filesystem::path& root) noexcept {
    try {
        std::error_code iteratorError;
        const auto now = std::filesystem::file_time_type::clock::now();
        for (std::filesystem::directory_iterator iterator(root, iteratorError), end;
             !iteratorError && iterator != end;
             iterator.increment(iteratorError)) {
            const std::filesystem::directory_entry& entry = *iterator;
            std::error_code typeError;
            if (!entry.is_regular_file(typeError) || typeError) {
                continue;
            }
            const std::wstring fileName = entry.path().filename().wstring();
            const bool staging = fileName.starts_with(kStagingPrefix);
            const bool artifact = fileName.starts_with(kArtifactPrefix);
            if (!staging && !artifact) {
                continue;
            }
            std::error_code timeError;
            const auto writeTime = entry.last_write_time(timeError);
            if (timeError) {
                continue;
            }
            const auto retention = staging ? kStagingRetention : kArtifactRetention;
            if (now - writeTime > retention) {
                RemoveIfPresent(entry.path());
            }
        }
    } catch (...) {
        // Cache cleanup is best-effort and must never block an export.
    }
}

struct CopyContext final {
    std::stop_token stopToken;
    const ExportArtifactCache::CopyProgressCallback* progress{};
};

DWORD CALLBACK CopyProgress(
    const LARGE_INTEGER totalFileSize,
    const LARGE_INTEGER totalBytesTransferred,
    LARGE_INTEGER,
    LARGE_INTEGER,
    DWORD,
    DWORD,
    HANDLE,
    HANDLE,
    LPVOID contextValue) {
    const auto* context = static_cast<const CopyContext*>(contextValue);
    if (context == nullptr || context->stopToken.stop_requested()) {
        return PROGRESS_CANCEL;
    }
    try {
        if (context->progress != nullptr && *context->progress) {
            (*context->progress)(
                static_cast<std::uint64_t>(totalBytesTransferred.QuadPart),
                static_cast<std::uint64_t>(totalFileSize.QuadPart));
        }
    } catch (...) {
        return PROGRESS_CANCEL;
    }
    return PROGRESS_CONTINUE;
}

[[nodiscard]] ExportArtifactCacheResult CancelledResult(
    std::wstring keyId) {
    ExportArtifactCacheResult result{};
    result.cancelled = true;
    result.nativeError = HRESULT_FROM_WIN32(ERROR_CANCELLED);
    result.errorMessage = L"导出缓存操作已取消。";
    result.keyId = std::move(keyId);
    return result;
}

}  // namespace

struct ExportArtifactCache::Impl final {
    enum class EntryState : std::uint8_t {
        Building,
        Ready,
        Failed,
    };

    struct Entry final {
        EntryState state{EntryState::Building};
        std::condition_variable_any changed;
    };

    struct KeyHash final {
        [[nodiscard]] std::size_t operator()(
            const ExportArtifactCacheKey& key) const noexcept {
            return static_cast<std::size_t>(StableKeyHash(
                key,
                1'469'598'103'934'665'603ULL));
        }
    };

    std::filesystem::path fallbackRoot;
    std::mutex mutex;
    std::unordered_map<
        ExportArtifactCacheKey,
        std::shared_ptr<Entry>,
        KeyHash> entries;
    std::mutex maintenanceMutex;
    std::unordered_set<std::wstring> maintainedRoots;
};

ExportArtifactCache& ExportArtifactCache::Shared() {
    static ExportArtifactCache cache;
    return cache;
}

ExportArtifactCache::ExportArtifactCache()
    : impl_(std::make_unique<Impl>()) {
    impl_->fallbackRoot = win32::LocalAppDataDirectory() / L"ExportCache";
    std::wstring ignoredError;
    if (win32::EnsureDirectory(impl_->fallbackRoot, &ignoredError)) {
        CleanupCacheDirectory(impl_->fallbackRoot);
        impl_->maintainedRoots.insert(impl_->fallbackRoot.wstring());
    }
}

ExportArtifactCache::~ExportArtifactCache() = default;

std::optional<ExportArtifactCacheKey> ExportArtifactCache::BuildKey(
    const std::filesystem::path& sourcePath,
    const std::chrono::milliseconds trimStart,
    const std::chrono::milliseconds trimEnd,
    const OutputFormat format,
    std::wstring* errorMessage) noexcept {
    SourceIdentity identity{};
    if (!ReadSourceIdentity(sourcePath, &identity, errorMessage)) {
        return std::nullopt;
    }

    ExportArtifactCacheKey key{};
    key.sourceAccessPath = std::move(identity.accessPath);
    key.normalizedSourcePath = std::move(identity.normalizedPath);
    key.sourceSize = identity.size;
    key.sourceLastWriteTime = identity.lastWriteTime;
    key.sourceVolumeSerialNumber = identity.volumeSerialNumber;
    key.sourceFileId = identity.fileId;
    key.sourceChangeTime = identity.changeTime;
    key.trimStartMilliseconds = trimStart.count();
    key.trimEndMilliseconds = trimEnd.count();
    key.format = format;
    return key;
}

std::optional<ExportArtifactCacheKey> ExportArtifactCache::BuildKey(
    const ExportRequest& request,
    std::wstring* errorMessage) noexcept {
    if (request.playbackSpeedTenths < 1 ||
        request.playbackSpeedTenths > 30) {
        if (errorMessage != nullptr) {
            *errorMessage = L"导出倍速必须在 0.1× 到 3.0× 之间。";
        }
        return std::nullopt;
    }
    if (!media::ExportQuality::IsValid(request.qualityPercent)) {
        if (errorMessage != nullptr) {
            *errorMessage = L"导出画质必须在 25% 到 100% 之间。";
        }
        return std::nullopt;
    }
    std::optional<ExportArtifactCacheKey> key = BuildKey(
        request.recording.sourcePath,
        request.trimStart,
        request.trimEnd,
        request.format,
        errorMessage);
    if (key.has_value()) {
        key->playbackSpeedTenths = request.playbackSpeedTenths;
        key->qualityPercent = request.qualityPercent;
    }
    if (!key.has_value() || !request.includeSystemAudio) {
        return key;
    }

    if (request.format != OutputFormat::Mp4 ||
        !request.recording.systemAudio.available ||
        request.recording.systemAudio.sourcePath.empty()) {
        if (errorMessage != nullptr) {
            *errorMessage = L"本次录屏没有可用于 MP4 导出的电脑声音。";
        }
        return std::nullopt;
    }

    SourceIdentity audioIdentity{};
    if (!ReadSourceIdentity(
            request.recording.systemAudio.sourcePath,
            &audioIdentity,
            errorMessage)) {
        return std::nullopt;
    }
    key->includeSystemAudio = true;
    key->audioAccessPath = std::move(audioIdentity.accessPath);
    key->normalizedAudioPath = std::move(audioIdentity.normalizedPath);
    key->audioSize = audioIdentity.size;
    key->audioLastWriteTime = audioIdentity.lastWriteTime;
    key->audioVolumeSerialNumber = audioIdentity.volumeSerialNumber;
    key->audioFileId = audioIdentity.fileId;
    key->audioChangeTime = audioIdentity.changeTime;
    return key;
}

std::wstring ExportArtifactCache::KeyId(const ExportArtifactCacheKey& key) {
    const std::uint64_t first = StableKeyHash(
        key,
        1'469'598'103'934'665'603ULL);
    const std::uint64_t second = StableKeyHash(
        key,
        10'995'116'282'111ULL);
    return std::format(L"{:016x}{:016x}", first, second);
}

bool ExportArtifactCache::SourceMatchesKey(
    const ExportArtifactCacheKey& key) noexcept {
    SourceIdentity identity{};
    if (!ReadSourceIdentity(
            std::filesystem::path(
                key.sourceAccessPath.empty()
                    ? key.normalizedSourcePath
                    : key.sourceAccessPath),
            &identity,
            nullptr)) {
        return false;
    }
    const bool sourceMatches =
        identity.normalizedPath == key.normalizedSourcePath &&
        identity.size == key.sourceSize &&
        identity.lastWriteTime == key.sourceLastWriteTime &&
        identity.volumeSerialNumber == key.sourceVolumeSerialNumber &&
        identity.fileId == key.sourceFileId &&
        identity.changeTime == key.sourceChangeTime;
    if (!sourceMatches || !key.includeSystemAudio) {
        return sourceMatches;
    }

    SourceIdentity audioIdentity{};
    if (!ReadSourceIdentity(
            std::filesystem::path(
                key.audioAccessPath.empty()
                    ? key.normalizedAudioPath
                    : key.audioAccessPath),
            &audioIdentity,
            nullptr)) {
        return false;
    }
    return audioIdentity.normalizedPath == key.normalizedAudioPath &&
        audioIdentity.size == key.audioSize &&
        audioIdentity.lastWriteTime == key.audioLastWriteTime &&
        audioIdentity.volumeSerialNumber == key.audioVolumeSerialNumber &&
        audioIdentity.fileId == key.audioFileId &&
        audioIdentity.changeTime == key.audioChangeTime;
}

ExportArtifactCacheResult ExportArtifactCache::GetOrCreate(
    const ExportArtifactCacheKey& key,
    const std::wstring_view extension,
    const std::stop_token stopToken,
    const Generator& generator) {
    ExportArtifactCacheResult result{};
    result.keyId = KeyId(key);
    const std::wstring normalizedExtension = NormalizeExtension(extension);
    if (normalizedExtension.empty() || !generator) {
        result.nativeError = E_INVALIDARG;
        result.errorMessage = L"导出缓存参数无效。";
        return result;
    }
    if (stopToken.stop_requested()) {
        return CancelledResult(result.keyId);
    }

    std::wstring directoryError;
    std::filesystem::path cacheRoot = PreferredRootForKey(key);
    if (!IsDirectoryWritable(cacheRoot, &directoryError)) {
        cacheRoot = impl_->fallbackRoot;
        if (!IsDirectoryWritable(cacheRoot, &directoryError)) {
            result.nativeError = E_ACCESSDENIED;
            result.errorMessage = std::move(directoryError);
            return result;
        }
    }
    {
        std::scoped_lock maintenanceLock(impl_->maintenanceMutex);
        if (impl_->maintainedRoots.insert(cacheRoot.wstring()).second) {
            CleanupCacheDirectory(cacheRoot);
        }
    }
    if (!SourceMatchesKey(key)) {
        result.nativeError = HRESULT_FROM_WIN32(ERROR_FILE_INVALID);
        result.errorMessage = L"源录屏在导出前已发生变化。";
        return result;
    }

    const std::filesystem::path artifactPath = cacheRoot /
        (std::wstring(kArtifactPrefix) + result.keyId + normalizedExtension);
    const std::filesystem::path manifestPath = cacheRoot /
        (std::wstring(kArtifactPrefix) + result.keyId + L".key");
    const std::wstring serializedKey = SerializeKey(key);

    std::shared_ptr<Impl::Entry> entry;
    bool isBuilder = false;
    for (;;) {
        std::unique_lock lock(impl_->mutex);
        const auto existing = impl_->entries.find(key);
        if (existing == impl_->entries.end()) {
            entry = std::make_shared<Impl::Entry>();
            impl_->entries.emplace(key, entry);
            isBuilder = true;
            break;
        }

        entry = existing->second;
        if (entry->state == Impl::EntryState::Building) {
            result.waitedForBuilder = true;
            const auto waitStarted = std::chrono::steady_clock::now();
            const bool stateChanged = entry->changed.wait(
                lock,
                stopToken,
                [&entry] {
                    return entry->state != Impl::EntryState::Building;
                });
            result.builderWait += std::chrono::duration_cast<
                std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - waitStarted);
            if (!stateChanged) {
                ExportArtifactCacheResult cancelled =
                    CancelledResult(result.keyId);
                cancelled.waitedForBuilder = result.waitedForBuilder;
                cancelled.builderWait = result.builderWait;
                return cancelled;
            }
            continue;
        }
        if (entry->state == Impl::EntryState::Failed) {
            impl_->entries.erase(existing);
            continue;
        }

        lock.unlock();
        if (HasExpectedSignature(artifactPath, normalizedExtension) &&
            ManifestMatches(manifestPath, serializedKey, artifactPath) &&
            SourceMatchesKey(key)) {
            result.success = true;
            result.cacheHit = true;
            result.nativeError = S_OK;
            result.artifactPath = artifactPath;
            return result;
        }

        lock.lock();
        if (entry->state == Impl::EntryState::Ready) {
            entry->state = Impl::EntryState::Building;
            isBuilder = true;
            break;
        }
    }

    if (!isBuilder || entry == nullptr) {
        result.nativeError = E_UNEXPECTED;
        result.errorMessage = L"导出缓存状态异常。";
        return result;
    }

    bool entryFinished = false;
    auto finishEntry = [this, &key, &entry, &entryFinished](
                           const bool succeeded) {
        if (entryFinished) {
            return;
        }
        {
            std::scoped_lock lock(impl_->mutex);
            entry->state = succeeded
                ? Impl::EntryState::Ready
                : Impl::EntryState::Failed;
            if (!succeeded) {
                const auto existing = impl_->entries.find(key);
                if (existing != impl_->entries.end() &&
                    existing->second == entry) {
                    impl_->entries.erase(existing);
                }
            }
        }
        entryFinished = true;
        entry->changed.notify_all();
    };
    ScopeExit entryFailureGuard([&finishEntry] {
        finishEntry(false);
    });

    if (HasExpectedSignature(artifactPath, normalizedExtension) &&
        ManifestMatches(manifestPath, serializedKey, artifactPath) &&
        SourceMatchesKey(key)) {
        result.success = true;
        result.cacheHit = true;
        result.nativeError = S_OK;
        result.artifactPath = artifactPath;
        finishEntry(true);
        return result;
    }
    if (stopToken.stop_requested()) {
        finishEntry(false);
        return CancelledResult(result.keyId);
    }

    const std::filesystem::path stagingPath = UniqueStagingPath(
        cacheRoot,
        result.keyId,
        normalizedExtension);
    const std::filesystem::path manifestStagingPath = UniqueStagingPath(
        cacheRoot,
        result.keyId,
        L".key");
    RemoveIfPresent(stagingPath);
    RemoveIfPresent(manifestStagingPath);

    HRESULT generationResult = E_FAIL;
    const auto generationStarted = std::chrono::steady_clock::now();
    try {
        generationResult = generator(stagingPath, stopToken, &result.errorMessage);
    } catch (const std::exception& exception) {
        result.errorMessage = L"生成导出缓存时发生异常：";
        const std::string message = exception.what();
        result.errorMessage.append(message.begin(), message.end());
        generationResult = E_FAIL;
    } catch (...) {
        result.errorMessage = L"生成导出缓存时发生未知异常。";
        generationResult = E_FAIL;
    }
    result.generationElapsed = std::chrono::duration_cast<
        std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - generationStarted);

    if (stopToken.stop_requested() ||
        generationResult == HRESULT_FROM_WIN32(ERROR_CANCELLED)) {
        RemoveIfPresent(stagingPath);
        RemoveIfPresent(manifestStagingPath);
        finishEntry(false);
        return CancelledResult(result.keyId);
    }
    if (FAILED(generationResult)) {
        RemoveIfPresent(stagingPath);
        RemoveIfPresent(manifestStagingPath);
        result.nativeError = generationResult;
        if (result.errorMessage.empty()) {
            result.errorMessage = L"无法生成导出缓存：" +
                win32::FormatError(generationResult);
        }
        finishEntry(false);
        return result;
    }
    if (!HasExpectedSignature(stagingPath, normalizedExtension)) {
        RemoveIfPresent(stagingPath);
        result.nativeError = HRESULT_FROM_WIN32(ERROR_FILE_INVALID);
        result.errorMessage = L"生成的导出缓存为空或无效。";
        finishEntry(false);
        return result;
    }
    FileFingerprint stagingFingerprint{};
    if (!ReadFileFingerprint(stagingPath, &stagingFingerprint)) {
        RemoveIfPresent(stagingPath);
        result.nativeError = HRESULT_FROM_WIN32(ERROR_FILE_INVALID);
        result.errorMessage = L"无法读取生成成片的完整性信息。";
        finishEntry(false);
        return result;
    }
    if (!SourceMatchesKey(key)) {
        RemoveIfPresent(stagingPath);
        result.nativeError = HRESULT_FROM_WIN32(ERROR_FILE_INVALID);
        result.errorMessage = L"生成期间源录屏已发生变化，已放弃缓存。";
        finishEntry(false);
        return result;
    }
    const std::wstring serializedManifest = SerializeManifest(
        serializedKey,
        stagingFingerprint);
    if (!WriteManifest(
            manifestStagingPath,
            serializedManifest,
            &result.errorMessage)) {
        RemoveIfPresent(stagingPath);
        RemoveIfPresent(manifestStagingPath);
        result.nativeError = E_FAIL;
        finishEntry(false);
        return result;
    }
    if (stopToken.stop_requested()) {
        RemoveIfPresent(stagingPath);
        RemoveIfPresent(manifestStagingPath);
        finishEntry(false);
        return CancelledResult(result.keyId);
    }

    if (::MoveFileExW(
            stagingPath.c_str(),
            artifactPath.c_str(),
            MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) == FALSE) {
        const DWORD moveError = ::GetLastError();
        RemoveIfPresent(stagingPath);
        RemoveIfPresent(manifestStagingPath);
        result.nativeError = HRESULT_FROM_WIN32(moveError);
        result.errorMessage = L"无法原子提交导出缓存：" +
            win32::FormatError(result.nativeError);
        finishEntry(false);
        return result;
    }
    if (::MoveFileExW(
            manifestStagingPath.c_str(),
            manifestPath.c_str(),
            MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) == FALSE) {
        const DWORD moveError = ::GetLastError();
        RemoveIfPresent(artifactPath);
        RemoveIfPresent(manifestStagingPath);
        result.nativeError = HRESULT_FROM_WIN32(moveError);
        result.errorMessage = L"无法提交导出缓存索引：" +
            win32::FormatError(result.nativeError);
        finishEntry(false);
        return result;
    }

    if (!HasExpectedSignature(artifactPath, normalizedExtension) ||
        !ManifestMatches(manifestPath, serializedKey, artifactPath)) {
        RemoveIfPresent(artifactPath);
        RemoveIfPresent(manifestPath);
        result.nativeError = HRESULT_FROM_WIN32(ERROR_FILE_INVALID);
        result.errorMessage = L"导出缓存提交后的完整性复核失败。";
        finishEntry(false);
        return result;
    }

    result.success = true;
    result.cacheHit = false;
    result.nativeError = S_OK;
    result.artifactPath = artifactPath;
    result.errorMessage.clear();
    finishEntry(true);
    entryFailureGuard.Release();
    return result;
}

HRESULT ExportArtifactCache::Materialize(
    const std::filesystem::path& artifactPath,
    const std::filesystem::path& destinationPath,
    const std::stop_token stopToken,
    MediaArtifactDelivery* delivery,
    const CopyProgressCallback& progress) {
    if (artifactPath.empty() || destinationPath.empty() || delivery == nullptr) {
        return E_INVALIDARG;
    }
    *delivery = MediaArtifactDelivery::None;
    if (stopToken.stop_requested()) {
        return HRESULT_FROM_WIN32(ERROR_CANCELLED);
    }

    const DWORD existingAttributes = ::GetFileAttributesW(destinationPath.c_str());
    if (existingAttributes != INVALID_FILE_ATTRIBUTES) {
        return HRESULT_FROM_WIN32(ERROR_FILE_EXISTS);
    }

    if (::CreateHardLinkW(
            destinationPath.c_str(),
            artifactPath.c_str(),
            nullptr) != FALSE) {
        if (stopToken.stop_requested()) {
            RemoveIfPresent(destinationPath);
            return HRESULT_FROM_WIN32(ERROR_CANCELLED);
        }
        *delivery = MediaArtifactDelivery::HardLinked;
        if (progress) {
            std::error_code sizeError;
            const std::uintmax_t size = std::filesystem::file_size(
                artifactPath,
                sizeError);
            if (!sizeError) {
                try {
                    progress(size, size);
                } catch (...) {
                    RemoveIfPresent(destinationPath);
                    *delivery = MediaArtifactDelivery::None;
                    return E_ABORT;
                }
            }
        }
        if (stopToken.stop_requested()) {
            RemoveIfPresent(destinationPath);
            *delivery = MediaArtifactDelivery::None;
            return HRESULT_FROM_WIN32(ERROR_CANCELLED);
        }
        return S_OK;
    }
    const DWORD hardLinkError = ::GetLastError();
    if (hardLinkError == ERROR_FILE_EXISTS ||
        hardLinkError == ERROR_ALREADY_EXISTS) {
        return HRESULT_FROM_WIN32(hardLinkError);
    }

    if (stopToken.stop_requested()) {
        return HRESULT_FROM_WIN32(ERROR_CANCELLED);
    }
    CopyContext context{stopToken, &progress};
    BOOL cancel = FALSE;
    if (::CopyFileExW(
            artifactPath.c_str(),
            destinationPath.c_str(),
            CopyProgress,
            &context,
            &cancel,
            COPY_FILE_FAIL_IF_EXISTS | COPY_FILE_RESTARTABLE) == FALSE) {
        const DWORD copyError = ::GetLastError();
        if (copyError != ERROR_FILE_EXISTS &&
            copyError != ERROR_ALREADY_EXISTS) {
            RemoveIfPresent(destinationPath);
        }
        return HRESULT_FROM_WIN32(copyError);
    }
    if (stopToken.stop_requested()) {
        RemoveIfPresent(destinationPath);
        return HRESULT_FROM_WIN32(ERROR_CANCELLED);
    }
    *delivery = MediaArtifactDelivery::Copied;
    return S_OK;
}

const std::filesystem::path& ExportArtifactCache::RootDirectory() const noexcept {
    return impl_->fallbackRoot;
}

}  // namespace qrec
