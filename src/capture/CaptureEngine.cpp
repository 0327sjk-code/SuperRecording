#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "CaptureEngine.h"

#include "DesktopDuplicator.h"
#include "SystemAudioCapture.h"
#include "../media/Mp4Writer.h"

#include <windows.h>
#include <objbase.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <future>
#include <mutex>
#include <system_error>
#include <thread>
#include <utility>

#pragma comment(lib, "ole32.lib")

namespace qrec::capture {
namespace {

using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;

inline constexpr std::int64_t kMediaFoundationTicksPerSecond = 10'000'000;
inline constexpr auto kStatsCallbackInterval = 250ms;
inline constexpr auto kInitialFrameTimeout = 2s;
inline constexpr std::uint32_t kRecordingKeyframeIntervalMilliseconds = 100;

void ClearError(CaptureError* error) {
    if (error != nullptr) {
        *error = {};
    }
}

void AssignError(CaptureError* destination, const CaptureError& source) {
    if (destination != nullptr) {
        *destination = source;
    }
}

[[nodiscard]] CaptureError MakeError(
    const CaptureErrorCode code,
    std::wstring message,
    const long nativeCode = 0) {
    return CaptureError{code, nativeCode, std::move(message)};
}

[[nodiscard]] CaptureErrorCode MapDesktopInitializationError(
    const DesktopDuplicatorError error) noexcept {
    switch (error) {
        case DesktopDuplicatorError::InvalidRegion:
            return CaptureErrorCode::InvalidArgument;
        case DesktopDuplicatorError::DisplayNotFound:
            return CaptureErrorCode::DisplayNotFound;
        case DesktopDuplicatorError::CrossDisplayRegion:
            return CaptureErrorCode::CrossDisplayRegionUnsupported;
        case DesktopDuplicatorError::UnsupportedRotation:
            return CaptureErrorCode::UnsupportedDisplayRotation;
        case DesktopDuplicatorError::GraphicsInitialization:
        case DesktopDuplicatorError::DuplicationInitialization:
            return CaptureErrorCode::GraphicsInitializationFailed;
        case DesktopDuplicatorError::FrameAcquisition:
            return CaptureErrorCode::CaptureFailed;
        case DesktopDuplicatorError::None:
        default:
            return CaptureErrorCode::GraphicsInitializationFailed;
    }
}

[[nodiscard]] std::int64_t TimestampForFrame(
    const std::uint64_t frameIndex,
    const int framesPerSecond) noexcept {
    const std::uint64_t wholeSeconds = frameIndex / static_cast<std::uint64_t>(framesPerSecond);
    const std::uint64_t remainingFrames =
        frameIndex % static_cast<std::uint64_t>(framesPerSecond);
    return static_cast<std::int64_t>(
        wholeSeconds * kMediaFoundationTicksPerSecond +
        remainingFrames * kMediaFoundationTicksPerSecond /
            static_cast<std::uint64_t>(framesPerSecond));
}

[[nodiscard]] std::filesystem::path SystemAudioPathFor(
    const std::filesystem::path& videoPath) {
    return videoPath.parent_path() /
        (videoPath.stem().wstring() + L".system-audio.m4a");
}

[[nodiscard]] std::wstring SystemAudioErrorText(
    const SystemAudioCaptureError& error) {
    if (!error.message.empty()) {
        return error.message;
    }
    if (error.nativeCode != 0) {
        return L"系统音频捕获失败（错误码 " +
            std::to_wstring(error.nativeCode) + L"）。";
    }
    return L"未捕获到电脑声音。";
}

void RemoveFileBestEffort(const std::filesystem::path& path) noexcept {
    if (path.empty()) {
        return;
    }
    std::error_code ignored;
    std::filesystem::remove(path, ignored);
}

[[nodiscard]] bool IsNonEmptyFile(const std::filesystem::path& path) noexcept {
    std::error_code error;
    return std::filesystem::is_regular_file(path, error) && !error &&
        std::filesystem::file_size(path, error) > 0 && !error;
}

template <typename Callback, typename Value>
void InvokeSafely(const Callback& callback, const Value& value) noexcept {
    if (!callback) {
        return;
    }
    try {
        callback(value);
    } catch (...) {
        // 外部 UI 回调不能破坏录制线程与 MP4 封装。
    }
}

}  // namespace

struct CaptureEngine::Impl final {
    mutable std::mutex stateMutex;
    std::mutex lifecycleMutex;
    std::condition_variable stateChanged;
    std::thread worker;
    SystemAudioCapture systemAudioCapture;

    RecordingState state{RecordingState::Idle};
    RecordingStats stats{};
    IntRect actualRegion{};
    bool starting{};
    bool stopRequested{};
    bool pauseRequested{};
    std::uint64_t resumeGeneration{};
    Clock::time_point sessionStarted{};
    Clock::time_point pauseStarted{};
    Clock::duration accumulatedPause{};
    std::optional<RecordingResult> completedResult;
    CaptureError terminalError{};
    std::wstring systemAudioDiagnostic;
    bool systemAudioReliable{};

    [[nodiscard]] RecordingStats CurrentStatsUnlocked() const noexcept {
        RecordingStats current = stats;
        if (sessionStarted == Clock::time_point{} || state == RecordingState::Idle) {
            return current;
        }

        const Clock::time_point end =
            pauseRequested ? pauseStarted : Clock::now();
        Clock::duration elapsed = end - sessionStarted - accumulatedPause;
        if (elapsed < Clock::duration::zero()) {
            elapsed = Clock::duration::zero();
        }
        current.activeDuration = std::max(
            current.activeDuration,
            std::chrono::duration_cast<std::chrono::milliseconds>(elapsed));
        return current;
    }

    void StoreTerminalError(const CaptureError& error) {
        std::scoped_lock lock(stateMutex);
        terminalError = error;
    }

    void StoreSystemAudioDiagnostic(
        std::wstring message,
        const bool reliable = false) {
        std::scoped_lock lock(stateMutex);
        systemAudioDiagnostic = std::move(message);
        systemAudioReliable = reliable;
    }

    [[nodiscard]] bool ShouldStop() const {
        std::scoped_lock lock(stateMutex);
        return stopRequested;
    }

    void Run(
        const CaptureConfig config,
        const CaptureCallbacks callbacks,
        std::promise<bool> startupResult) noexcept {
        const HRESULT comResult = ::CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        const bool comInitialized = SUCCEEDED(comResult);
        struct ComShutdown final {
            bool initialized{};
            ~ComShutdown() {
                if (initialized) {
                    ::CoUninitialize();
                }
            }
        } comShutdown{comInitialized};

        if (!comInitialized) {
            const CaptureError error = MakeError(
                CaptureErrorCode::GraphicsInitializationFailed,
                L"初始化录制线程 COM 环境失败。",
                static_cast<long>(comResult));
            {
                std::scoped_lock lock(stateMutex);
                terminalError = error;
                starting = false;
                state = RecordingState::Idle;
            }
            startupResult.set_value(false);
            stateChanged.notify_all();
            return;
        }

        DesktopDuplicator duplicator;
        DesktopDuplicatorOptions duplicationOptions{config.region, config.includeCursor};
        std::wstring nativeMessage;
        long nativeError = 0;
        DesktopDuplicatorError duplicationError = DesktopDuplicatorError::None;
        if (!duplicator.Initialize(
                duplicationOptions,
                nativeMessage,
                nativeError,
                &duplicationError)) {
            const CaptureError error = MakeError(
                MapDesktopInitializationError(duplicationError),
                std::move(nativeMessage),
                nativeError);
            {
                std::scoped_lock lock(stateMutex);
                terminalError = error;
                starting = false;
                state = RecordingState::Idle;
            }
            startupResult.set_value(false);
            stateChanged.notify_all();
            return;
        }

        media::Mp4Writer writer;
        const media::Mp4WriterConfig writerConfig{
            config.outputPath,
            static_cast<std::uint32_t>(config.region.Width()),
            static_cast<std::uint32_t>(config.region.Height()),
            config.framesPerSecond,
            0,
            true,
            kRecordingKeyframeIntervalMilliseconds,
        };
        if (!writer.Open(writerConfig, nativeMessage, nativeError)) {
            const CaptureError error = MakeError(
                CaptureErrorCode::EncoderInitializationFailed,
                std::move(nativeMessage),
                nativeError);
            {
                std::scoped_lock lock(stateMutex);
                terminalError = error;
                starting = false;
                state = RecordingState::Idle;
            }
            startupResult.set_value(false);
            stateChanged.notify_all();
            return;
        }

        const std::filesystem::path systemAudioPath =
            SystemAudioPathFor(config.outputPath);
        RemoveFileBestEffort(systemAudioPath);
        SystemAudioCaptureError systemAudioStartError;
        SystemAudioCaptureCallbacks systemAudioCallbacks;
        systemAudioCallbacks.onError = [this](
            const SystemAudioCaptureError& audioError) {
            StoreSystemAudioDiagnostic(
                L"未捕获到电脑声音：" + SystemAudioErrorText(audioError));
        };
        const bool systemAudioStarted = systemAudioCapture.Start(
            SystemAudioCaptureConfig{
                systemAudioPath,
                48'000,
                2,
                0,
                SystemAudioEndpointRole::Multimedia,
            },
            std::move(systemAudioCallbacks),
            &systemAudioStartError);
        if (!systemAudioStarted) {
            RemoveFileBestEffort(systemAudioPath);
            StoreSystemAudioDiagnostic(
                L"未捕获到电脑声音：" +
                SystemAudioErrorText(systemAudioStartError));
        } else {
            StoreSystemAudioDiagnostic(
                L"电脑声音已捕获，可在编辑器中选择加入 MP4。",
                true);
        }

        {
            std::scoped_lock lock(stateMutex);
            sessionStarted = Clock::now();
            pauseStarted = {};
            accumulatedPause = Clock::duration::zero();
            stats = {};
            state = RecordingState::Recording;
            starting = false;
        }
        startupResult.set_value(true);
        stateChanged.notify_all();

        const auto framePeriod = std::chrono::nanoseconds(
            1'000'000'000LL / config.framesPerSecond);
        Clock::time_point nextFrameDeadline = Clock::now();
        Clock::time_point firstFrameWaitStarted = nextFrameDeadline;
        Clock::time_point nextStatsCallback = nextFrameDeadline;
        std::uint64_t timelineFrameIndex = 0;
        std::uint64_t observedResumeGeneration = 0;
        std::int64_t encodedDuration100Nanoseconds = 0;
        DesktopFrame latestFrame;
        CaptureError runtimeError{};

        while (true) {
            {
                std::unique_lock lock(stateMutex);
                if (resumeGeneration != observedResumeGeneration) {
                    observedResumeGeneration = resumeGeneration;
                    nextFrameDeadline = Clock::now();
                    nextStatsCallback = nextFrameDeadline;
                }
                if (pauseRequested && !stopRequested) {
                    stateChanged.wait(lock, [this] {
                        return stopRequested || !pauseRequested;
                    });
                    nextFrameDeadline = Clock::now();
                    nextStatsCallback = nextFrameDeadline;
                    observedResumeGeneration = resumeGeneration;
                }
                if (stopRequested) {
                    if (timelineFrameIndex == 0 && !runtimeError) {
                        runtimeError = MakeError(
                            CaptureErrorCode::CaptureFailed,
                            L"录制在首帧写入前已结束。");
                    }
                    break;
                }
                if (stateChanged.wait_until(lock, nextFrameDeadline, [this] {
                        return stopRequested || pauseRequested;
                    })) {
                    continue;
                }
            }

            const std::uint32_t acquireTimeout = latestFrame.bgra.empty() ? 100U : 0U;
            FrameAcquireStatus acquireStatus = duplicator.AcquireFrame(
                latestFrame,
                acquireTimeout,
                nativeMessage,
                nativeError);
            if (acquireStatus == FrameAcquireStatus::AccessLost) {
                bool recovered = false;
                DesktopDuplicatorError reinitializeError = DesktopDuplicatorError::None;
                for (int attempt = 0; attempt < 3 && !ShouldStop(); ++attempt) {
                    if (attempt != 0) {
                        std::this_thread::sleep_for(100ms);
                    }
                    if (duplicator.Initialize(
                            duplicationOptions,
                            nativeMessage,
                            nativeError,
                            &reinitializeError)) {
                        recovered = true;
                        latestFrame = {};
                        firstFrameWaitStarted = Clock::now();
                        break;
                    }
                }
                if (recovered) {
                    nextFrameDeadline = Clock::now();
                    continue;
                }
                runtimeError = MakeError(
                    CaptureErrorCode::DesktopAccessLost,
                    L"桌面复制访问丢失且无法恢复：" + nativeMessage,
                    nativeError);
                break;
            }
            if (acquireStatus == FrameAcquireStatus::Failed) {
                runtimeError = MakeError(
                    CaptureErrorCode::CaptureFailed,
                    std::move(nativeMessage),
                    nativeError);
                break;
            }
            if (latestFrame.bgra.empty()) {
                if (Clock::now() - firstFrameWaitStarted >= kInitialFrameTimeout) {
                    runtimeError = MakeError(
                        CaptureErrorCode::CaptureFailed,
                        L"在超时时间内未获得首个桌面帧。");
                    break;
                }
                nextFrameDeadline = Clock::now();
                continue;
            }

            const std::int64_t timestamp = TimestampForFrame(
                timelineFrameIndex,
                config.framesPerSecond);
            const std::int64_t sampleEnd = TimestampForFrame(
                timelineFrameIndex + 1,
                config.framesPerSecond);
            const std::int64_t sampleDuration = sampleEnd - timestamp;
            if (!writer.WriteBgraFrame(
                    latestFrame.bgra,
                    latestFrame.stride,
                    timestamp,
                    sampleDuration,
                    nativeMessage,
                    nativeError)) {
                runtimeError = MakeError(
                    CaptureErrorCode::EncodeFailed,
                    std::move(nativeMessage),
                    nativeError);
                break;
            }

            encodedDuration100Nanoseconds = sampleEnd;
            ++timelineFrameIndex;
            RecordingStats statsSnapshot{};
            {
                std::scoped_lock lock(stateMutex);
                ++stats.encodedFrames;
                stats.activeDuration = std::chrono::milliseconds(
                    encodedDuration100Nanoseconds / 10'000);
                statsSnapshot = stats;
            }

            nextFrameDeadline += framePeriod;
            const Clock::time_point afterEncoding = Clock::now();
            if (afterEncoding > nextFrameDeadline) {
                const auto behind = afterEncoding - nextFrameDeadline;
                const auto skipped = static_cast<std::uint64_t>(
                    behind / framePeriod);
                if (skipped != 0) {
                    timelineFrameIndex += skipped;
                    nextFrameDeadline += framePeriod * skipped;
                    std::scoped_lock lock(stateMutex);
                    stats.droppedFrames += skipped;
                    statsSnapshot = stats;
                }
            }

            if (afterEncoding >= nextStatsCallback) {
                InvokeSafely(callbacks.onStats, statsSnapshot);
                nextStatsCallback = afterEncoding + kStatsCallbackInterval;
            }
        }

        SystemAudioRecording systemAudioRecording{};
        if (systemAudioStarted) {
            SystemAudioCaptureError systemAudioStopError;
            const std::optional<SystemAudioRecordingResult> audioResult =
                systemAudioCapture.Stop(&systemAudioStopError);
            bool audioReliable = false;
            std::wstring audioDiagnostic;
            {
                std::scoped_lock lock(stateMutex);
                audioReliable = systemAudioReliable;
                audioDiagnostic = systemAudioDiagnostic;
            }
            if (audioReliable && audioResult.has_value() &&
                audioResult->encodedFrames != 0 &&
                IsNonEmptyFile(audioResult->outputPath)) {
                systemAudioRecording.sourcePath = audioResult->outputPath;
                systemAudioRecording.available = true;
                systemAudioRecording.sampleRate = audioResult->sampleRate;
                systemAudioRecording.channels = audioResult->channelCount;
                systemAudioRecording.duration = audioResult->duration;
                systemAudioRecording.statusMessage =
                    L"捕获系统正在播放的声音";
            } else {
                RemoveFileBestEffort(systemAudioPath);
                systemAudioRecording.statusMessage = !audioReliable &&
                    !audioDiagnostic.empty()
                    ? audioDiagnostic
                    : systemAudioStopError
                    ? L"未捕获到电脑声音：" +
                        SystemAudioErrorText(systemAudioStopError)
                    : L"未捕获到电脑声音";
            }
        } else {
            std::scoped_lock lock(stateMutex);
            systemAudioRecording.statusMessage = systemAudioDiagnostic.empty()
                ? L"未捕获到电脑声音"
                : systemAudioDiagnostic;
        }

        std::wstring finalizeMessage;
        long finalizeNativeError = 0;
        if (!writer.Finalize(finalizeMessage, finalizeNativeError) && !runtimeError) {
            runtimeError = MakeError(
                CaptureErrorCode::FinalizeFailed,
                std::move(finalizeMessage),
                finalizeNativeError);
        }

        std::optional<RecordingResult> result;
        if (!runtimeError) {
            result = RecordingResult{
                config.outputPath,
                config.region,
                config.framesPerSecond,
                static_cast<std::uint32_t>(config.region.Width()),
                static_cast<std::uint32_t>(config.region.Height()),
                std::chrono::milliseconds(encodedDuration100Nanoseconds / 10'000),
                std::move(systemAudioRecording),
            };
        } else {
            RemoveFileBestEffort(systemAudioPath);
        }

        RecordingStats finalStats{};
        {
            std::scoped_lock lock(stateMutex);
            stats.activeDuration = std::chrono::milliseconds(
                encodedDuration100Nanoseconds / 10'000);
            finalStats = stats;
            completedResult = result;
            terminalError = runtimeError;
            pauseRequested = false;
            stopRequested = false;
            starting = false;
            state = RecordingState::Idle;
        }
        stateChanged.notify_all();

        InvokeSafely(callbacks.onStats, finalStats);
        if (runtimeError) {
            InvokeSafely(callbacks.onError, runtimeError);
        } else if (result) {
            InvokeSafely(callbacks.onCompleted, *result);
        }
    }
};

CaptureEngine::CaptureEngine() : impl_(std::make_unique<Impl>()) {}

CaptureEngine::~CaptureEngine() {
    static_cast<void>(Stop(nullptr));
}

IntRect CaptureEngine::NormalizeToEvenRegion(IntRect region) noexcept {
    const int width = region.Width();
    const int height = region.Height();
    if (width > 0 && width % 2 != 0) {
        --region.right;
    }
    if (height > 0 && height % 2 != 0) {
        --region.bottom;
    }
    return region;
}

bool CaptureEngine::Start(
    const CaptureConfig& requestedConfig,
    CaptureCallbacks callbacks,
    CaptureError* error) {
    std::scoped_lock lifecycleLock(impl_->lifecycleMutex);
    ClearError(error);

    {
        std::scoped_lock lock(impl_->stateMutex);
        if (impl_->state != RecordingState::Idle || impl_->starting) {
            AssignError(
                error,
                MakeError(CaptureErrorCode::InvalidState, L"已有录制任务正在运行。"));
            return false;
        }
    }
    if (impl_->worker.joinable()) {
        impl_->worker.join();
    }

    CaptureConfig config = requestedConfig;
    config.region = NormalizeToEvenRegion(config.region);
    if (!config.region.IsValid() || config.outputPath.empty() ||
        (config.framesPerSecond != 30 && config.framesPerSecond != 60)) {
        AssignError(
            error,
            MakeError(
                CaptureErrorCode::InvalidArgument,
                L"录制区域、输出路径或帧率无效；帧率只能为 30 或 60。"));
        return false;
    }

    {
        std::scoped_lock lock(impl_->stateMutex);
        impl_->actualRegion = config.region;
        impl_->stats = {};
        impl_->completedResult.reset();
        impl_->terminalError = {};
        impl_->systemAudioDiagnostic.clear();
        impl_->systemAudioReliable = false;
        impl_->stopRequested = false;
        impl_->pauseRequested = false;
        impl_->resumeGeneration = 0;
        impl_->starting = true;
        impl_->sessionStarted = {};
        impl_->pauseStarted = {};
        impl_->accumulatedPause = Clock::duration::zero();
    }

    std::promise<bool> startupPromise;
    std::future<bool> startupFuture = startupPromise.get_future();
    try {
        impl_->worker = std::thread(
            [implementation = impl_.get(),
             config,
             callbacks = std::move(callbacks),
             promise = std::move(startupPromise)]() mutable {
                implementation->Run(config, callbacks, std::move(promise));
            });
    } catch (...) {
        const CaptureError creationError = MakeError(
            CaptureErrorCode::GraphicsInitializationFailed,
            L"创建录制线程失败。");
        {
            std::scoped_lock lock(impl_->stateMutex);
            impl_->starting = false;
            impl_->terminalError = creationError;
        }
        AssignError(error, creationError);
        return false;
    }

    const bool started = startupFuture.get();
    if (!started) {
        if (impl_->worker.joinable()) {
            impl_->worker.join();
        }
        std::scoped_lock lock(impl_->stateMutex);
        AssignError(error, impl_->terminalError);
        return false;
    }
    return true;
}

bool CaptureEngine::Start(
    const IntRect region,
    const std::filesystem::path& outputPath,
    const int framesPerSecond,
    const bool includeCursor,
    CaptureCallbacks callbacks,
    CaptureError* error) {
    return Start(
        CaptureConfig{region, outputPath, framesPerSecond, includeCursor},
        std::move(callbacks),
        error);
}

bool CaptureEngine::Pause(CaptureError* error) {
    ClearError(error);
    {
        std::scoped_lock lock(impl_->stateMutex);
        if (impl_->state != RecordingState::Recording || impl_->pauseRequested) {
            AssignError(
                error,
                MakeError(CaptureErrorCode::InvalidState, L"当前录制状态不能暂停。"));
            return false;
        }

        impl_->pauseRequested = true;
        impl_->pauseStarted = Clock::now();
        impl_->state = RecordingState::Paused;
        impl_->stateChanged.notify_all();
    }

    if (impl_->systemAudioCapture.State() ==
        SystemAudioCaptureState::Capturing) {
        SystemAudioCaptureError audioError;
        if (!impl_->systemAudioCapture.Pause(&audioError)) {
            impl_->StoreSystemAudioDiagnostic(
                L"电脑声音暂停同步失败：" + SystemAudioErrorText(audioError));
        }
    }
    return true;
}

bool CaptureEngine::Resume(CaptureError* error) {
    ClearError(error);
    {
        std::scoped_lock lock(impl_->stateMutex);
        if (impl_->state != RecordingState::Paused || !impl_->pauseRequested) {
            AssignError(
                error,
                MakeError(CaptureErrorCode::InvalidState, L"当前录制状态不能继续。"));
            return false;
        }
    }

    if (impl_->systemAudioCapture.State() == SystemAudioCaptureState::Paused) {
        SystemAudioCaptureError audioError;
        if (!impl_->systemAudioCapture.Resume(&audioError)) {
            impl_->StoreSystemAudioDiagnostic(
                L"电脑声音继续同步失败：" + SystemAudioErrorText(audioError));
        }
    }

    {
        std::scoped_lock lock(impl_->stateMutex);
        impl_->accumulatedPause += Clock::now() - impl_->pauseStarted;
        impl_->pauseRequested = false;
        ++impl_->resumeGeneration;
        impl_->state = RecordingState::Recording;
        impl_->stateChanged.notify_all();
    }
    return true;
}

std::optional<RecordingResult> CaptureEngine::Stop(CaptureError* error) {
    std::scoped_lock lifecycleLock(impl_->lifecycleMutex);
    ClearError(error);

    {
        std::scoped_lock lock(impl_->stateMutex);
        if (impl_->state != RecordingState::Idle || impl_->starting) {
            if (impl_->pauseRequested) {
                impl_->accumulatedPause += Clock::now() - impl_->pauseStarted;
            }
            impl_->pauseRequested = false;
            impl_->stopRequested = true;
            impl_->state = RecordingState::Finalizing;
            impl_->stateChanged.notify_all();
        }
    }

    if (impl_->worker.joinable()) {
        if (impl_->worker.get_id() == std::this_thread::get_id()) {
            AssignError(
                error,
                MakeError(
                    CaptureErrorCode::InvalidState,
                    L"录制回调线程只能请求停止，最终结果请通过完成回调接收。"));
            return std::nullopt;
        }
        impl_->worker.join();
    }

    std::scoped_lock lock(impl_->stateMutex);
    if (impl_->terminalError) {
        AssignError(error, impl_->terminalError);
    }
    return impl_->completedResult;
}

RecordingState CaptureEngine::State() const noexcept {
    std::scoped_lock lock(impl_->stateMutex);
    return impl_->state;
}

RecordingStats CaptureEngine::Stats() const noexcept {
    std::scoped_lock lock(impl_->stateMutex);
    return impl_->CurrentStatsUnlocked();
}

IntRect CaptureEngine::ActualRegion() const noexcept {
    std::scoped_lock lock(impl_->stateMutex);
    return impl_->actualRegion;
}

}  // namespace qrec::capture
