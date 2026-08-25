#include "config/ConfigStore.h"

#include "common/Win32Helpers.h"

#include <windows.h>

#include <array>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace qrec {
namespace {

constexpr wchar_t kSection[] = L"Recorder";

bool WriteValue(
    const std::filesystem::path& path,
    const wchar_t* key,
    const std::wstring& value) {
    return ::WritePrivateProfileStringW(kSection, key, value.c_str(), path.c_str()) != FALSE;
}

}  // namespace

ConfigStore::ConfigStore()
    : filePath_(win32::LocalAppDataDirectory() / L"settings.ini") {
    legacySettingsMigrated_ = MigrateLegacySettingsIfNeeded();
}

ConfigStore::ConfigStore(std::filesystem::path filePath)
    : filePath_(std::move(filePath)) {}

AppSettings ConfigStore::Load() const {
    AppSettings settings;
    const int storedFps = ::GetPrivateProfileIntW(
        kSection, L"FramesPerSecond", 60, filePath_.c_str());
    settings.framesPerSecond = storedFps == 30 ? 30 : 60;

    std::array<wchar_t, 32768> pathBuffer{};
    ::GetPrivateProfileStringW(
        kSection,
        L"SaveDirectory",
        win32::DefaultVideoDirectory().c_str(),
        pathBuffer.data(),
        static_cast<DWORD>(pathBuffer.size()),
        filePath_.c_str());
    settings.saveDirectory = pathBuffer.data();

    std::array<wchar_t, 16> formatBuffer{};
    ::GetPrivateProfileStringW(
        kSection,
        L"DefaultFormat",
        L"MP4",
        formatBuffer.data(),
        static_cast<DWORD>(formatBuffer.size()),
        filePath_.c_str());
    settings.defaultFormat = _wcsicmp(formatBuffer.data(), L"GIF") == 0
        ? OutputFormat::Gif
        : OutputFormat::Mp4;
    settings.includeCursor = ::GetPrivateProfileIntW(
        kSection, L"IncludeCursor", 1, filePath_.c_str()) != 0;
    std::array<wchar_t, 8> keepEditorOpenBuffer{};
    ::GetPrivateProfileStringW(
        kSection,
        L"KeepEditorOpenAfterExport",
        L"1",
        keepEditorOpenBuffer.data(),
        static_cast<DWORD>(keepEditorOpenBuffer.size()),
        filePath_.c_str());
    // Only an explicitly persisted "0" disables the safe default. Missing,
    // truncated, or manually corrupted values keep the editor open.
    settings.keepEditorOpenAfterExport =
        std::wstring_view(keepEditorOpenBuffer.data()) != L"0";

    std::wstring ignoredError;
    static_cast<void>(win32::EnsureDirectory(settings.saveDirectory, &ignoredError));
    return settings;
}

bool ConfigStore::Save(const AppSettings& settings) const {
    std::wstring ignoredError;
    if (!win32::EnsureDirectory(filePath_.parent_path(), &ignoredError) ||
        !win32::EnsureDirectory(settings.saveDirectory, &ignoredError)) {
        return false;
    }

    const bool fpsWritten = WriteValue(
        filePath_, L"FramesPerSecond",
        settings.framesPerSecond == 30 ? L"30" : L"60");
    const bool directoryWritten = WriteValue(
        filePath_, L"SaveDirectory", settings.saveDirectory.wstring());
    const bool formatWritten = WriteValue(
        filePath_, L"DefaultFormat",
        settings.defaultFormat == OutputFormat::Gif ? L"GIF" : L"MP4");
    const bool cursorWritten = WriteValue(
        filePath_, L"IncludeCursor", settings.includeCursor ? L"1" : L"0");
    const bool keepEditorOpenWritten = WriteValue(
        filePath_,
        L"KeepEditorOpenAfterExport",
        settings.keepEditorOpenAfterExport ? L"1" : L"0");
    return fpsWritten && directoryWritten && formatWritten && cursorWritten &&
        keepEditorOpenWritten;
}

bool ConfigStore::LoadStartupEnabled() const {
    return ::GetPrivateProfileIntW(
        kSection,
        L"StartWithWindows",
        1,
        filePath_.c_str()) != 0;
}

bool ConfigStore::SaveStartupEnabled(const bool enabled) const {
    std::wstring ignoredError;
    if (!win32::EnsureDirectory(filePath_.parent_path(), &ignoredError)) {
        return false;
    }
    return WriteValue(
        filePath_,
        L"StartWithWindows",
        enabled ? L"1" : L"0");
}

HotkeyBinding ConfigStore::LoadRecordingHotkey() const {
    const HotkeyBinding fallback = DefaultHotkeyBinding();
    const std::wstring fallbackValue = SerializeHotkeyBinding(fallback);
    std::array<wchar_t, 96> valueBuffer{};
    ::GetPrivateProfileStringW(
        kSection,
        L"RecordingHotkey",
        fallbackValue.c_str(),
        valueBuffer.data(),
        static_cast<DWORD>(valueBuffer.size()),
        filePath_.c_str());
    const std::optional<HotkeyBinding> parsed = ParseHotkeyBinding(valueBuffer.data());
    return parsed.value_or(fallback);
}

bool ConfigStore::SaveRecordingHotkey(const HotkeyBinding& binding) const {
    if (!IsValidHotkeyBinding(binding)) {
        return false;
    }
    std::wstring ignoredError;
    if (!win32::EnsureDirectory(filePath_.parent_path(), &ignoredError)) {
        return false;
    }
    const std::wstring serialized = SerializeHotkeyBinding(binding);
    if (serialized.empty() ||
        !WriteValue(filePath_, L"RecordingHotkey", serialized)) {
        return false;
    }
    // The cache-flush form can report FALSE even after the value has already
    // been committed. Durability is proven by the write result and readback.
    static_cast<void>(
        ::WritePrivateProfileStringW(nullptr, nullptr, nullptr, filePath_.c_str()));
    return LoadRecordingHotkey() == binding;
}

bool ConfigStore::SaveKeepEditorOpenAfterExport(const bool enabled) const {
    std::wstring ignoredError;
    if (!win32::EnsureDirectory(filePath_.parent_path(), &ignoredError) ||
        !WriteValue(
            filePath_,
            L"KeepEditorOpenAfterExport",
            enabled ? L"1" : L"0")) {
        return false;
    }

    static_cast<void>(
        ::WritePrivateProfileStringW(nullptr, nullptr, nullptr, filePath_.c_str()));

    std::array<wchar_t, 8> valueBuffer{};
    ::GetPrivateProfileStringW(
        kSection,
        L"KeepEditorOpenAfterExport",
        L"",
        valueBuffer.data(),
        static_cast<DWORD>(valueBuffer.size()),
        filePath_.c_str());
    return std::wstring_view(valueBuffer.data()) == (enabled ? L"1" : L"0");
}

bool ConfigStore::MigrateLegacySettingsIfNeeded() const noexcept {
    try {
        const DWORD currentAttributes = ::GetFileAttributesW(filePath_.c_str());
        if (currentAttributes != INVALID_FILE_ATTRIBUTES) {
            return false;
        }
        const DWORD currentError = ::GetLastError();
        if (currentError != ERROR_FILE_NOT_FOUND &&
            currentError != ERROR_PATH_NOT_FOUND) {
            return false;
        }

        const std::filesystem::path legacyPath =
            win32::LegacyLocalAppDataDirectory() / L"settings.ini";
        const DWORD legacyAttributes = ::GetFileAttributesW(legacyPath.c_str());
        if (legacyAttributes == INVALID_FILE_ATTRIBUTES ||
            (legacyAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0 ||
            (legacyAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
            return false;
        }

        std::wstring ignoredError;
        if (!win32::EnsureDirectory(filePath_.parent_path(), &ignoredError)) {
            return false;
        }

        std::filesystem::path temporaryPath = filePath_;
        temporaryPath += L".migrating";
        static_cast<void>(::DeleteFileW(temporaryPath.c_str()));
        if (::CopyFileW(legacyPath.c_str(), temporaryPath.c_str(), TRUE) == FALSE) {
            return false;
        }
        if (::MoveFileExW(
                temporaryPath.c_str(),
                filePath_.c_str(),
                MOVEFILE_WRITE_THROUGH) == FALSE) {
            static_cast<void>(::DeleteFileW(temporaryPath.c_str()));
            return false;
        }
        return true;
    } catch (...) {
        return false;
    }
}

}  // namespace qrec
