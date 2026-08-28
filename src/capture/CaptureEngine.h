#pragma once

#include "../common/Types.h"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>

namespace qrec::capture {

enum class CaptureErrorCode : std::uint8_t {
    None,
    InvalidArgument,
    InvalidState,
    DisplayNotFound,
    CrossDisplayRegionUnsupported,
    UnsupportedDisplayRotation,
    GraphicsInitializationFailed,
    DesktopAccessLost,
    CaptureFailed,
    EncoderInitializationFailed,
    EncodeFailed,
    FinalizeFailed,
};

struct CaptureError final {
    CaptureErrorCode code{CaptureErrorCode::None};
    long nativeCode{};
    std::wstring message;

    [[nodiscard]] explicit operator bool() const noexcept {
        return code != CaptureErrorCode::None;
    }
};

struct CaptureConfig final {
    IntRect region{};
    std::filesystem::path outputPath;
    int framesPerSecond{60};
    bool includeCursor{true};
    int outputQualityPercent{100};
};

struct CaptureCallbacks final {
    // 回调在内部录制线程触发；调用方如需更新 UI，应投递到窗口线程。
    std::function<void(const RecordingStats&)> onStats;
    std::function<void(const RecordingResult&)> onCompleted;
    std::function<void(const CaptureError&)> onError;
};

class CaptureEngine final {
public:
    CaptureEngine();
    ~CaptureEngine();

    CaptureEngine(const CaptureEngine&) = delete;
    CaptureEngine& operator=(const CaptureEngine&) = delete;

    [[nodiscard]] bool Start(
        const CaptureConfig& config,
        CaptureCallbacks callbacks = {},
        CaptureError* error = nullptr);

    [[nodiscard]] bool Start(
        IntRect region,
        const std::filesystem::path& outputPath,
        int framesPerSecond,
        bool includeCursor,
        CaptureCallbacks callbacks = {},
        CaptureError* error = nullptr);

    [[nodiscard]] bool Pause(CaptureError* error = nullptr);
    [[nodiscard]] bool Resume(CaptureError* error = nullptr);

    // 阻塞至 H.264/MP4 尾部写入完成；成功时返回最终录制信息。
    [[nodiscard]] std::optional<RecordingResult> Stop(CaptureError* error = nullptr);

    [[nodiscard]] RecordingState State() const noexcept;
    [[nodiscard]] RecordingStats Stats() const noexcept;
    [[nodiscard]] IntRect ActualRegion() const noexcept;

    [[nodiscard]] static IntRect NormalizeToEvenRegion(IntRect region) noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace qrec::capture
