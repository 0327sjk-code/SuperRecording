#pragma once

#include "common/HotkeyBinding.h"
#include "common/Types.h"

#include <filesystem>

namespace qrec {

class ConfigStore final {
public:
    ConfigStore();
    explicit ConfigStore(std::filesystem::path filePath);

    [[nodiscard]] AppSettings Load() const;
    [[nodiscard]] bool Save(const AppSettings& settings) const;
    [[nodiscard]] bool LoadStartupEnabled() const;
    [[nodiscard]] bool SaveStartupEnabled(bool enabled) const;
    [[nodiscard]] HotkeyBinding LoadRecordingHotkey() const;
    [[nodiscard]] bool SaveRecordingHotkey(const HotkeyBinding& binding) const;
    [[nodiscard]] bool SaveKeepEditorOpenAfterExport(bool enabled) const;
    [[nodiscard]] const std::filesystem::path& FilePath() const noexcept { return filePath_; }
    [[nodiscard]] bool LegacySettingsMigrated() const noexcept {
        return legacySettingsMigrated_;
    }

private:
    [[nodiscard]] bool MigrateLegacySettingsIfNeeded() const noexcept;

    std::filesystem::path filePath_;
    bool legacySettingsMigrated_{};
};

}  // namespace qrec
