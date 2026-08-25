#include "app/CacheMaintenance.h"

#include "common/ProductInfo.h"
#include "common/Win32Helpers.h"

#include <algorithm>
#include <chrono>
#include <cwctype>
#include <string>
#include <system_error>

namespace qrec {
namespace {

using namespace std::chrono_literals;

constexpr auto kFinalizedRetention = 7 * 24h;
constexpr auto kPartialRetention = 24h;
constexpr auto kProbeRetention = 1h;

[[nodiscard]] std::wstring Lowercase(std::wstring value) {
    std::transform(value.begin(), value.end(), value.begin(), [](const wchar_t character) {
        return static_cast<wchar_t>(std::towlower(character));
    });
    return value;
}

[[nodiscard]] bool IsOwnedTemporaryFile(
    const std::filesystem::path& path,
    std::chrono::hours* retention) {
    if (retention == nullptr) {
        return false;
    }
    const std::wstring name = Lowercase(path.filename().wstring());
    const std::wstring extension = Lowercase(path.extension().wstring());
    if (name.starts_with(L".qrec-") && extension == L".tmp") {
        *retention = kProbeRetention;
        return true;
    }
    if (name.find(L".qrec-partial") != std::wstring::npos) {
        *retention = kPartialRetention;
        return extension == L".mp4" || extension == L".gif" ||
            extension == L".m4a";
    }
    if (extension == L".mp4" || extension == L".gif" ||
        extension == L".m4a") {
        *retention = kFinalizedRetention;
        return true;
    }
    return false;
}

void CleanupDirectory(
    const std::filesystem::path& directory,
    CacheCleanupReport* report) noexcept {
    if (report == nullptr || directory.empty()) {
        return;
    }
    std::error_code directoryError;
    if (!std::filesystem::is_directory(directory, directoryError)) {
        if (directoryError && directoryError != std::errc::no_such_file_or_directory) {
            ++report->failures;
        }
        return;
    }
    std::error_code iteratorError;
    std::filesystem::directory_iterator iterator(
        directory,
        std::filesystem::directory_options::skip_permission_denied,
        iteratorError);
    if (iteratorError) {
        ++report->failures;
        return;
    }

    const auto now = std::filesystem::file_time_type::clock::now();
    for (const std::filesystem::directory_entry& entry : iterator) {
        std::error_code statusError;
        const std::filesystem::file_status status = entry.symlink_status(statusError);
        if (statusError || !std::filesystem::is_regular_file(status) ||
            std::filesystem::is_symlink(status)) {
            if (statusError) {
                ++report->failures;
            }
            continue;
        }

        std::chrono::hours retention{};
        if (!IsOwnedTemporaryFile(entry.path(), &retention)) {
            continue;
        }
        std::error_code timeError;
        const auto modified = entry.last_write_time(timeError);
        if (timeError || modified > now || now - modified < retention) {
            if (timeError) {
                ++report->failures;
            }
            continue;
        }

        std::error_code sizeError;
        const std::uintmax_t size = entry.file_size(sizeError);
        std::error_code removeError;
        if (std::filesystem::remove(entry.path(), removeError)) {
            ++report->removedFiles;
            if (!sizeError) {
                report->releasedBytes += size;
            }
        } else if (removeError) {
            ++report->failures;
        }
    }
}

}  // namespace

CacheCleanupReport CacheMaintenance::Cleanup(
    const std::filesystem::path& saveDirectory) noexcept {
    CacheCleanupReport report{};
    try {
        CleanupDirectory(saveDirectory / product::RecordingCacheDirectoryName, &report);
        CleanupDirectory(saveDirectory / product::LegacyRecordingCacheDirectoryName, &report);
        const std::filesystem::path localRoot = win32::LocalAppDataDirectory();
        CleanupDirectory(localRoot / L"Temp", &report);
        CleanupDirectory(localRoot / L"Clipboard", &report);
    } catch (...) {
        ++report.failures;
    }
    return report;
}

}  // namespace qrec
