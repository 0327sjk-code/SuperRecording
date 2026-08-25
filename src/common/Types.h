#pragma once

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <string>

namespace qrec {

struct IntRect final {
    int left{};
    int top{};
    int right{};
    int bottom{};

    [[nodiscard]] int Width() const noexcept { return std::max(0, right - left); }
    [[nodiscard]] int Height() const noexcept { return std::max(0, bottom - top); }
    [[nodiscard]] bool IsValid(int minimumSize = 16) const noexcept {
        return Width() >= minimumSize && Height() >= minimumSize;
    }
};

enum class OutputFormat : std::uint8_t {
    Mp4,
    Gif,
};

enum class RecordingState : std::uint8_t {
    Idle,
    Selecting,
    Recording,
    Paused,
    Finalizing,
    Editing,
};

struct AppSettings final {
    int framesPerSecond{60};
    std::filesystem::path saveDirectory;
    OutputFormat defaultFormat{OutputFormat::Mp4};
    bool includeCursor{true};
    bool keepEditorOpenAfterExport{true};
};

struct RecordingStats final {
    std::chrono::milliseconds activeDuration{};
    std::uint64_t encodedFrames{};
    std::uint64_t droppedFrames{};
};

struct SystemAudioRecording final {
    std::filesystem::path sourcePath;
    bool available{};
    std::uint32_t sampleRate{};
    std::uint16_t channels{};
    std::chrono::milliseconds duration{};
    std::wstring statusMessage;
};

struct RecordingResult final {
    std::filesystem::path sourcePath;
    IntRect region{};
    int framesPerSecond{60};
    std::uint32_t width{};
    std::uint32_t height{};
    std::chrono::milliseconds duration{};
    SystemAudioRecording systemAudio;
};

struct ExportRequest final {
    RecordingResult recording;
    std::chrono::milliseconds trimStart{};
    std::chrono::milliseconds trimEnd{};
    OutputFormat format{OutputFormat::Mp4};
    bool includeSystemAudio{};
    std::filesystem::path destinationPath;
};

struct ExportProgress final {
    double fraction{};
    std::wstring phase;
};

}  // namespace qrec
