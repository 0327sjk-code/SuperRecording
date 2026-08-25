#pragma once

#include <windows.h>

#include <filesystem>
#include <optional>

namespace qrec {

[[nodiscard]] std::optional<std::filesystem::path> PickSaveDirectory(
    HWND owner,
    const std::filesystem::path& currentDirectory);

}  // namespace qrec
