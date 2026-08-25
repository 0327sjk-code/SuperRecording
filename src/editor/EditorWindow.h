#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "common/Types.h"

#include <windows.h>

#include <filesystem>
#include <functional>
#include <memory>
#include <string>

namespace qrec {

struct EditorExportResult final {
    bool success{};
    bool copiedToClipboard{};
    OutputFormat format{OutputFormat::Mp4};
    std::filesystem::path finalPath;
    std::wstring errorMessage;
};

struct EditorWindowCallbacks final {
    std::function<void(const AppSettings&)> settingsChanged;
    std::function<void(const EditorExportResult&)> exportCompleted;
    std::function<void(const std::wstring&)> diagnostic;
    std::function<void()> closed;
};

class EditorWindow final {
public:
    explicit EditorWindow(HINSTANCE instance, HWND owner = nullptr);
    ~EditorWindow();

    EditorWindow(const EditorWindow&) = delete;
    EditorWindow& operator=(const EditorWindow&) = delete;

    [[nodiscard]] bool Open(
        RecordingResult recording,
        AppSettings settings,
        EditorWindowCallbacks callbacks,
        std::wstring* errorMessage = nullptr);
    [[nodiscard]] bool Close();
    void CloseForShutdown() noexcept;

    [[nodiscard]] HWND WindowHandle() const noexcept;
    [[nodiscard]] bool IsOpen() const noexcept;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace qrec
