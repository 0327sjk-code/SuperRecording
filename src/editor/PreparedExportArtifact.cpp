#include "editor/PreparedExportArtifact.h"

#include "media/ExportArtifactCache.h"
#include "media/MediaExporter.h"

#include <windows.h>

#include <utility>

namespace qrec {
namespace {

struct ArtifactFingerprint final {
    std::uint64_t size{};
    std::uint64_t lastWriteTime{};
};

[[nodiscard]] bool ReadFingerprint(
    const std::filesystem::path& path,
    ArtifactFingerprint* const fingerprint) noexcept {
    if (path.empty() || fingerprint == nullptr) {
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
    ULARGE_INTEGER writeTime{};
    writeTime.HighPart = attributes.ftLastWriteTime.dwHighDateTime;
    writeTime.LowPart = attributes.ftLastWriteTime.dwLowDateTime;
    if (size.QuadPart == 0) {
        return false;
    }
    fingerprint->size = size.QuadPart;
    fingerprint->lastWriteTime = writeTime.QuadPart;
    return true;
}

}  // namespace

struct PreparedExportArtifact::Impl final {
    std::optional<ExportArtifactCacheKey> requestKey;
    std::filesystem::path artifactPath;
    ArtifactFingerprint artifactFingerprint;
};

PreparedExportArtifact::PreparedExportArtifact()
    : impl_(std::make_unique<Impl>()) {}

PreparedExportArtifact::~PreparedExportArtifact() = default;

void PreparedExportArtifact::Clear() noexcept {
    impl_->requestKey.reset();
    impl_->artifactPath.clear();
    impl_->artifactFingerprint = {};
}

bool PreparedExportArtifact::Store(
    const ExportRequest& request,
    const MediaExportResult& result) noexcept {
    Clear();
    if (!result.success || result.outputPath.empty()) {
        return false;
    }
    std::wstring keyError;
    std::optional<ExportArtifactCacheKey> key =
        ExportArtifactCache::BuildKey(request, &keyError);
    if (!key.has_value()) {
        return false;
    }
    const std::wstring keyId = ExportArtifactCache::KeyId(*key);
    if (!result.cacheKey.empty() && result.cacheKey != keyId) {
        return false;
    }
    ArtifactFingerprint fingerprint{};
    if (!ReadFingerprint(result.outputPath, &fingerprint)) {
        return false;
    }
    impl_->requestKey = std::move(key);
    impl_->artifactPath = result.outputPath;
    impl_->artifactFingerprint = fingerprint;
    return true;
}

std::optional<std::filesystem::path> PreparedExportArtifact::Resolve(
    const ExportRequest& request) const noexcept {
    if (!impl_->requestKey.has_value() || impl_->artifactPath.empty()) {
        return std::nullopt;
    }
    std::optional<ExportArtifactCacheKey> currentKey =
        ExportArtifactCache::BuildKey(request, nullptr);
    if (!currentKey.has_value() || !(*currentKey == *impl_->requestKey) ||
        !ExportArtifactCache::SourceMatchesKey(*impl_->requestKey)) {
        return std::nullopt;
    }
    ArtifactFingerprint currentFingerprint{};
    if (!ReadFingerprint(impl_->artifactPath, &currentFingerprint) ||
        currentFingerprint.size != impl_->artifactFingerprint.size ||
        currentFingerprint.lastWriteTime !=
            impl_->artifactFingerprint.lastWriteTime) {
        return std::nullopt;
    }
    return impl_->artifactPath;
}

std::uint64_t PreparedExportArtifact::OutputBytes() const noexcept {
    return impl_->artifactFingerprint.size;
}

}  // namespace qrec
