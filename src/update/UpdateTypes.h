#pragma once

#include "update/SemanticVersion.h"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>

namespace qrec::update {

enum class UpdateErrorCode : std::uint8_t {
    None,
    Cancelled,
    InvalidConfiguration,
    InvalidUrl,
    Network,
    HttpStatus,
    ResponseTooLarge,
    InvalidVersion,
    FileSystem,
    InvalidExecutable,
    NoUpdateAvailable,
    WorkerStartFailed,
    OutOfMemory,
    Unexpected,
};

struct UpdateFailure final {
    UpdateErrorCode code{UpdateErrorCode::None};
    std::uint32_t nativeCode{};
    std::uint32_t httpStatus{};
    std::wstring message;

    [[nodiscard]] bool HasError() const noexcept {
        return code != UpdateErrorCode::None;
    }
};

struct VersionFetchResult final {
    std::optional<SemanticVersion> version;
    UpdateFailure failure;

    [[nodiscard]] bool Succeeded() const noexcept {
        return version.has_value() && !failure.HasError();
    }
};

struct ExecutableDownloadResult final {
    std::filesystem::path filePath;
    std::uint64_t downloadedBytes{};
    bool reusedCachedFile{};
    UpdateFailure failure;

    [[nodiscard]] bool Succeeded() const noexcept {
        return !filePath.empty() && !failure.HasError();
    }
};

enum class UpdatePhase : std::uint8_t {
    Idle,
    Checking,
    UpdateAvailable,
    UpToDate,
    Downloading,
    ReadyToInstall,
    Cancelled,
    Failed,
};

struct UpdateSnapshot final {
    UpdatePhase phase{UpdatePhase::Idle};
    SemanticVersion currentVersion;
    std::optional<SemanticVersion> latestVersion;
    std::filesystem::path downloadedFile;
    std::uint64_t downloadedBytes{};
    std::optional<std::uint64_t> totalBytes;
    UpdateFailure failure;
};

}  // namespace qrec::update
