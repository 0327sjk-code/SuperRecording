#include "media/InstantArtifactDelivery.h"

#include "common/Win32Helpers.h"

#include <system_error>

namespace qrec::media {
namespace {

[[nodiscard]] std::uint64_t FileSizeOrZero(
    const std::filesystem::path& path) noexcept {
    std::error_code error;
    const std::uintmax_t size = std::filesystem::file_size(path, error);
    return error ? 0 : static_cast<std::uint64_t>(size);
}

[[nodiscard]] bool IsHardLinkUnavailable(const DWORD error) noexcept {
    return error == ERROR_NOT_SAME_DEVICE ||
        error == ERROR_INVALID_FUNCTION ||
        error == ERROR_NOT_SUPPORTED;
}

}  // namespace

InstantDeliveryResult InstantArtifactDelivery::TryHardLink(
    const std::filesystem::path& artifactPath,
    const std::filesystem::path& destinationPath) noexcept {
    const auto started = std::chrono::steady_clock::now();
    InstantDeliveryResult result{};
    result.outputPath = destinationPath;

    std::error_code sourceError;
    if (artifactPath.empty() || destinationPath.empty() ||
        !std::filesystem::is_regular_file(artifactPath, sourceError) ||
        sourceError || FileSizeOrZero(artifactPath) == 0) {
        result.nativeError = HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND);
        result.errorMessage = L"已准备成片不存在或无法读取。";
    } else if (::CreateHardLinkW(
                   destinationPath.c_str(),
                   artifactPath.c_str(),
                   nullptr) != FALSE) {
        result.outcome = InstantDeliveryOutcome::Delivered;
        result.delivery = MediaArtifactDelivery::HardLinked;
        result.outputBytes = FileSizeOrZero(destinationPath);
        result.nativeError = S_OK;
    } else {
        const DWORD nativeError = ::GetLastError();
        result.nativeError = HRESULT_FROM_WIN32(nativeError);
        if (IsHardLinkUnavailable(nativeError)) {
            result.outcome = InstantDeliveryOutcome::NotApplicable;
        } else {
            result.outcome = InstantDeliveryOutcome::Failed;
            result.errorMessage = L"无法即时提交已准备成片：" +
                win32::FormatError(result.nativeError);
        }
    }

    result.elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now() - started);
    return result;
}

}  // namespace qrec::media
