#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>

namespace qrec::capture {

using SystemAudioQpcPosition = std::uint64_t;

// 与 WASAPI GetBuffer 的 QPC 时间戳使用相同的 100ns 单位。
[[nodiscard]] std::optional<SystemAudioQpcPosition>
QuerySystemAudioQpcPosition100Nanoseconds() noexcept;

enum class SystemAudioEndpointRole : std::uint8_t {
    Console,
    Multimedia,
    Communications,
};

enum class SystemAudioCaptureState : std::uint8_t {
    Idle,
    Starting,
    Capturing,
    Paused,
    Finalizing,
};

enum class SystemAudioCaptureErrorCode : std::uint8_t {
    None,
    InvalidArgument,
    InvalidState,
    ThreadCreationFailed,
    ComInitializationFailed,
    EndpointEnumerationFailed,
    DeviceActivationFailed,
    FormatUnsupported,
    EventInitializationFailed,
    CaptureServiceUnavailable,
    EncoderInitializationFailed,
    DeviceInvalidated,
    CaptureFailed,
    EncodeFailed,
    FinalizeFailed,
};

struct SystemAudioCaptureError final {
    SystemAudioCaptureErrorCode code{SystemAudioCaptureErrorCode::None};
    long nativeCode{};
    std::wstring message;

    [[nodiscard]] explicit operator bool() const noexcept {
        return code != SystemAudioCaptureErrorCode::None;
    }
};

struct SystemAudioCaptureConfig final {
    std::filesystem::path outputPath;
    std::uint32_t sampleRate{48'000};
    std::uint16_t channelCount{2};
    // Bits per second. Zero selects the AAC writer recommendation.
    std::uint32_t averageBitrate{};
    SystemAudioEndpointRole endpointRole{SystemAudioEndpointRole::Multimedia};
};

struct SystemAudioCaptureStats final {
    std::chrono::milliseconds activeDuration{};
    std::uint64_t encodedFrames{};
    std::uint64_t silentFrames{};
    std::uint64_t discardedFramesDuringPause{};
    std::uint64_t discontinuityCount{};
};

struct SystemAudioRecordingResult final {
    std::filesystem::path outputPath;
    std::chrono::milliseconds duration{};
    std::uint32_t sampleRate{};
    std::uint16_t channelCount{};
    std::uint32_t averageBitrate{};
    std::uint64_t encodedFrames{};
    std::uint64_t silentFrames{};
};

struct SystemAudioCaptureCallbacks final {
    // 回调在系统音频线程触发；UI 必须自行投递到窗口线程。
    std::function<void(const SystemAudioCaptureStats&)> onStats;
    std::function<void(const SystemAudioRecordingResult&)> onCompleted;
    std::function<void(const SystemAudioCaptureError&)> onError;
};

class SystemAudioCapture final {
public:
    SystemAudioCapture();
    ~SystemAudioCapture();

    SystemAudioCapture(const SystemAudioCapture&) = delete;
    SystemAudioCapture& operator=(const SystemAudioCapture&) = delete;

    [[nodiscard]] bool Start(
        const SystemAudioCaptureConfig& config,
        SystemAudioCaptureCallbacks callbacks = {},
        SystemAudioCaptureError* error = nullptr);

    [[nodiscard]] bool Start(
        const SystemAudioCaptureConfig& config,
        std::optional<SystemAudioQpcPosition> timelineStartQpc,
        SystemAudioCaptureCallbacks callbacks = {},
        SystemAudioCaptureError* error = nullptr);

    // 先启动 WASAPI 并保留首帧前的短暂预卷；首次 Resume(boundaryQpc)
    // 会把该共享边界作为音频时间线的零点。
    [[nodiscard]] bool StartPrepared(
        const SystemAudioCaptureConfig& config,
        SystemAudioCaptureCallbacks callbacks = {},
        SystemAudioCaptureError* error = nullptr);

    // Pause/Resume 在返回前等待捕获线程确认；暂停期间只保留有界预卷，
    // 恢复时按共享 QPC 裁掉前缀并保留跨边界样本。
    [[nodiscard]] bool Pause(SystemAudioCaptureError* error = nullptr);
    [[nodiscard]] bool Resume(SystemAudioCaptureError* error = nullptr);

    [[nodiscard]] bool Pause(
        std::optional<SystemAudioQpcPosition> boundaryQpc,
        SystemAudioCaptureError* error = nullptr);
    [[nodiscard]] bool Pause(
        std::optional<SystemAudioQpcPosition> boundaryQpc,
        std::chrono::nanoseconds exactTimelineDuration,
        SystemAudioCaptureError* error = nullptr);
    [[nodiscard]] bool Resume(
        std::optional<SystemAudioQpcPosition> boundaryQpc,
        SystemAudioCaptureError* error = nullptr);

    // 阻塞至 AAC/M4A 尾部完成；系统音频失败不会操作或停止视频捕获器。
    [[nodiscard]] std::optional<SystemAudioRecordingResult> Stop(
        SystemAudioCaptureError* error = nullptr);

    // minimumDuration 使用最终视频的有效时间线；捕获端会用静音补齐缺失区间。
    [[nodiscard]] std::optional<SystemAudioRecordingResult> Stop(
        std::chrono::nanoseconds minimumDuration,
        SystemAudioCaptureError* error = nullptr);

    // 使用共享停止 QPC 裁掉边界外的包，并把音轨精确封口到视频时间线。
    [[nodiscard]] std::optional<SystemAudioRecordingResult> Stop(
        std::optional<SystemAudioQpcPosition> boundaryQpc,
        std::chrono::nanoseconds exactDuration,
        SystemAudioCaptureError* error = nullptr);

    [[nodiscard]] SystemAudioCaptureState State() const noexcept;
    [[nodiscard]] SystemAudioCaptureStats Stats() const noexcept;
    [[nodiscard]] SystemAudioCaptureError LastError() const;

private:
    [[nodiscard]] bool StartInternal(
        const SystemAudioCaptureConfig& config,
        std::optional<SystemAudioQpcPosition> timelineStartQpc,
        bool prepared,
        SystemAudioCaptureCallbacks callbacks,
        SystemAudioCaptureError* error);

    [[nodiscard]] bool PauseInternal(
        std::optional<SystemAudioQpcPosition> boundaryQpc,
        std::optional<std::chrono::nanoseconds> exactTimelineDuration,
        SystemAudioCaptureError* error);

    [[nodiscard]] std::optional<SystemAudioRecordingResult> StopInternal(
        std::optional<SystemAudioQpcPosition> boundaryQpc,
        std::optional<std::chrono::nanoseconds> exactDuration,
        SystemAudioCaptureError* error);

    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace qrec::capture
