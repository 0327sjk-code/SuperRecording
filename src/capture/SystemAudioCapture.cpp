#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "SystemAudioCapture.h"

#include "../media/AacAudioWriter.h"

#include <windows.h>

#include <audioclient.h>
#include <avrt.h>
#include <ksmedia.h>
#include <mmdeviceapi.h>
#include <mmreg.h>
#include <objbase.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <future>
#include <limits>
#include <mutex>
#include <new>
#include <span>
#include <thread>
#include <utility>
#include <vector>

#pragma comment(lib, "avrt.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "uuid.lib")

namespace qrec::capture {
namespace {

using Microsoft::WRL::ComPtr;
using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;

inline constexpr std::int64_t kMediaFoundationTicksPerSecond = 10'000'000;
inline constexpr std::int64_t kHundredNanosecondsPerMillisecond = 10'000;
inline constexpr std::uint32_t kDefaultSampleRate = 48'000;
inline constexpr std::uint32_t kAlternateSampleRate = 44'100;
inline constexpr std::uint16_t kMinimumChannelCount = 1;
inline constexpr std::uint16_t kMaximumChannelCount = 2;
inline constexpr std::uint16_t kFloatBitsPerSample = 32;
inline constexpr std::uint32_t kBitsPerByte = 8;
inline constexpr std::uint32_t kRequestedBufferMilliseconds = 100;
inline constexpr std::uint32_t kSilenceChunkMilliseconds = 100;
inline constexpr auto kCaptureWaitTimeout = 250ms;
inline constexpr auto kStatsCallbackInterval = 250ms;

void ClearError(SystemAudioCaptureError* error) {
    if (error != nullptr) {
        *error = {};
    }
}

void AssignError(
    SystemAudioCaptureError* destination,
    const SystemAudioCaptureError& source) {
    if (destination != nullptr) {
        *destination = source;
    }
}

[[nodiscard]] SystemAudioCaptureError MakeError(
    const SystemAudioCaptureErrorCode code,
    std::wstring message,
    const long nativeCode = 0) {
    return SystemAudioCaptureError{code, nativeCode, std::move(message)};
}

[[nodiscard]] bool IsDeviceInvalidationResult(const HRESULT result) noexcept {
    return result == AUDCLNT_E_DEVICE_INVALIDATED ||
           result == AUDCLNT_E_RESOURCES_INVALIDATED ||
           result == AUDCLNT_E_SERVICE_NOT_RUNNING ||
           result == AUDCLNT_E_ENDPOINT_CREATE_FAILED;
}

[[nodiscard]] SystemAudioCaptureError MakeWasapiError(
    const SystemAudioCaptureErrorCode fallbackCode,
    std::wstring message,
    const HRESULT result) {
    const SystemAudioCaptureErrorCode code = IsDeviceInvalidationResult(result)
                                                 ? SystemAudioCaptureErrorCode::DeviceInvalidated
                                                 : fallbackCode;
    return MakeError(code, std::move(message), static_cast<long>(result));
}

[[nodiscard]] std::int64_t FrameTimestamp(
    const std::uint64_t frameIndex,
    const std::uint32_t sampleRate) noexcept {
    const std::uint64_t wholeSeconds = frameIndex / sampleRate;
    const std::uint64_t remainingFrames = frameIndex % sampleRate;
    const std::uint64_t maximumWholeSeconds =
        static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) /
        static_cast<std::uint64_t>(kMediaFoundationTicksPerSecond);
    if (wholeSeconds > maximumWholeSeconds) {
        return std::numeric_limits<std::int64_t>::max();
    }
    return static_cast<std::int64_t>(
        wholeSeconds *
            static_cast<std::uint64_t>(kMediaFoundationTicksPerSecond) +
        remainingFrames *
            static_cast<std::uint64_t>(kMediaFoundationTicksPerSecond) /
            sampleRate);
}

[[nodiscard]] std::chrono::milliseconds DurationForFrames(
    const std::uint64_t frameCount,
    const std::uint32_t sampleRate) noexcept {
    return std::chrono::milliseconds(
        FrameTimestamp(frameCount, sampleRate) /
        kHundredNanosecondsPerMillisecond);
}

template <typename Callback, typename Value>
void InvokeSafely(const Callback& callback, const Value& value) noexcept {
    if (!callback) {
        return;
    }
    try {
        callback(value);
    } catch (...) {
        // 外部 UI 回调不能破坏 WASAPI 捕获与 M4A 封装。
    }
}

class UniqueHandle final {
public:
    UniqueHandle() = default;
    explicit UniqueHandle(const HANDLE handle) noexcept : handle_(handle) {}
    ~UniqueHandle() {
        Reset();
    }

    UniqueHandle(const UniqueHandle&) = delete;
    UniqueHandle& operator=(const UniqueHandle&) = delete;

    UniqueHandle(UniqueHandle&& other) noexcept
        : handle_(std::exchange(other.handle_, nullptr)) {}

    UniqueHandle& operator=(UniqueHandle&& other) noexcept {
        if (this != &other) {
            Reset(std::exchange(other.handle_, nullptr));
        }
        return *this;
    }

    void Reset(const HANDLE handle = nullptr) noexcept {
        if (handle_ != nullptr && handle_ != INVALID_HANDLE_VALUE) {
            static_cast<void>(::CloseHandle(handle_));
        }
        handle_ = handle;
    }

    [[nodiscard]] HANDLE Get() const noexcept {
        return handle_;
    }

    [[nodiscard]] explicit operator bool() const noexcept {
        return handle_ != nullptr && handle_ != INVALID_HANDLE_VALUE;
    }

private:
    HANDLE handle_{};
};

class ComApartment final {
public:
    ComApartment() noexcept
        : result_(::CoInitializeEx(nullptr, COINIT_MULTITHREADED)),
          initialized_(SUCCEEDED(result_)) {}

    ~ComApartment() {
        if (initialized_) {
            ::CoUninitialize();
        }
    }

    ComApartment(const ComApartment&) = delete;
    ComApartment& operator=(const ComApartment&) = delete;

    [[nodiscard]] HRESULT Result() const noexcept {
        return result_;
    }

    [[nodiscard]] explicit operator bool() const noexcept {
        return initialized_;
    }

private:
    HRESULT result_{};
    bool initialized_{};
};

class MmcssRegistration final {
public:
    MmcssRegistration() noexcept {
        handle_ = ::AvSetMmThreadCharacteristicsW(L"Audio", &taskIndex_);
    }

    ~MmcssRegistration() {
        if (handle_ != nullptr) {
            static_cast<void>(::AvRevertMmThreadCharacteristics(handle_));
        }
    }

    MmcssRegistration(const MmcssRegistration&) = delete;
    MmcssRegistration& operator=(const MmcssRegistration&) = delete;

private:
    HANDLE handle_{};
    DWORD taskIndex_{};
};

[[nodiscard]] ERole ToNativeEndpointRole(
    const SystemAudioEndpointRole role) noexcept {
    switch (role) {
        case SystemAudioEndpointRole::Console:
            return eConsole;
        case SystemAudioEndpointRole::Communications:
            return eCommunications;
        case SystemAudioEndpointRole::Multimedia:
        default:
            return eMultimedia;
    }
}

[[nodiscard]] WAVEFORMATEXTENSIBLE MakeCaptureFormat(
    const SystemAudioCaptureConfig& config) noexcept {
    WAVEFORMATEXTENSIBLE format{};
    format.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
    format.Format.nChannels = config.channelCount;
    format.Format.nSamplesPerSec = config.sampleRate;
    format.Format.wBitsPerSample = kFloatBitsPerSample;
    format.Format.nBlockAlign = static_cast<WORD>(
        static_cast<std::uint32_t>(config.channelCount) *
        kFloatBitsPerSample / kBitsPerByte);
    format.Format.nAvgBytesPerSec =
        format.Format.nSamplesPerSec * format.Format.nBlockAlign;
    format.Format.cbSize = static_cast<WORD>(
        sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX));
    format.Samples.wValidBitsPerSample = kFloatBitsPerSample;
    format.dwChannelMask = config.channelCount == 1
                               ? SPEAKER_FRONT_CENTER
                               : SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT;
    format.SubFormat = KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;
    return format;
}

struct WasapiPacket final {
    std::vector<std::byte> samples;
    std::uint32_t frameCount{};
    DWORD flags{};
    std::uint64_t devicePosition{};
};

class WasapiLoopbackSession final {
public:
    WasapiLoopbackSession() = default;
    ~WasapiLoopbackSession() {
        static_cast<void>(Stop());
    }

    WasapiLoopbackSession(const WasapiLoopbackSession&) = delete;
    WasapiLoopbackSession& operator=(const WasapiLoopbackSession&) = delete;

    [[nodiscard]] HRESULT Initialize(
        const SystemAudioCaptureConfig& config,
        SystemAudioCaptureErrorCode& errorCode) noexcept {
        errorCode = SystemAudioCaptureErrorCode::EndpointEnumerationFailed;

        ComPtr<IMMDeviceEnumerator> enumerator;
        HRESULT result = ::CoCreateInstance(
            __uuidof(MMDeviceEnumerator),
            nullptr,
            CLSCTX_INPROC_SERVER,
            IID_PPV_ARGS(&enumerator));
        if (FAILED(result)) {
            return result;
        }

        ComPtr<IMMDevice> endpoint;
        result = enumerator->GetDefaultAudioEndpoint(
            eRender,
            ToNativeEndpointRole(config.endpointRole),
            &endpoint);
        if (FAILED(result)) {
            return result;
        }

        errorCode = SystemAudioCaptureErrorCode::DeviceActivationFailed;
        result = endpoint->Activate(
            __uuidof(IAudioClient),
            CLSCTX_INPROC_SERVER,
            nullptr,
            reinterpret_cast<void**>(audioClient_.GetAddressOf()));
        if (FAILED(result)) {
            return result;
        }

        sampleEvent_.Reset(::CreateEventW(nullptr, FALSE, FALSE, nullptr));
        if (!sampleEvent_) {
            errorCode = SystemAudioCaptureErrorCode::EventInitializationFailed;
            return HRESULT_FROM_WIN32(::GetLastError());
        }

        const WAVEFORMATEXTENSIBLE format = MakeCaptureFormat(config);
        const DWORD streamFlags =
            AUDCLNT_STREAMFLAGS_LOOPBACK |
            AUDCLNT_STREAMFLAGS_EVENTCALLBACK |
            AUDCLNT_STREAMFLAGS_NOPERSIST |
            AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM |
            AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;
        const REFERENCE_TIME requestedBufferDuration =
            static_cast<REFERENCE_TIME>(kRequestedBufferMilliseconds) *
            kHundredNanosecondsPerMillisecond;

        result = audioClient_->Initialize(
            AUDCLNT_SHAREMODE_SHARED,
            streamFlags,
            requestedBufferDuration,
            0,
            &format.Format,
            nullptr);
        if (result == AUDCLNT_E_UNSUPPORTED_FORMAT) {
            errorCode = SystemAudioCaptureErrorCode::FormatUnsupported;
            return result;
        }
        if (FAILED(result)) {
            return result;
        }

        result = audioClient_->SetEventHandle(sampleEvent_.Get());
        if (FAILED(result)) {
            errorCode = SystemAudioCaptureErrorCode::EventInitializationFailed;
            return result;
        }

        errorCode = SystemAudioCaptureErrorCode::CaptureServiceUnavailable;
        result = audioClient_->GetService(IID_PPV_ARGS(&captureClient_));
        if (FAILED(result)) {
            return result;
        }

        blockAlignment_ = format.Format.nBlockAlign;
        return S_OK;
    }

    [[nodiscard]] HRESULT Start() noexcept {
        if (!audioClient_ || !captureClient_ || !sampleEvent_) {
            return E_UNEXPECTED;
        }
        const HRESULT result = audioClient_->Start();
        started_ = SUCCEEDED(result);
        return result;
    }

    [[nodiscard]] HRESULT Stop() noexcept {
        if (!started_ || !audioClient_) {
            return S_OK;
        }
        started_ = false;
        return audioClient_->Stop();
    }

    [[nodiscard]] HANDLE SampleEvent() const noexcept {
        return sampleEvent_.Get();
    }

    [[nodiscard]] HRESULT ReadNextPacket(
        WasapiPacket& packet,
        bool& hasPacket) noexcept {
        hasPacket = false;
        packet.frameCount = 0;
        packet.flags = 0;
        packet.devicePosition = 0;
        packet.samples.clear();

        UINT32 availableFrames = 0;
        HRESULT result = captureClient_->GetNextPacketSize(&availableFrames);
        if (FAILED(result) || availableFrames == 0) {
            return result;
        }

        BYTE* source = nullptr;
        UINT32 frameCount = 0;
        DWORD flags = 0;
        UINT64 devicePosition = 0;
        UINT64 qpcPosition = 0;
        result = captureClient_->GetBuffer(
            &source,
            &frameCount,
            &flags,
            &devicePosition,
            &qpcPosition);
        if (FAILED(result)) {
            return result;
        }

        struct BufferRelease final {
            IAudioCaptureClient* client{};
            UINT32 frameCount{};
            bool active{true};

            [[nodiscard]] HRESULT Release() noexcept {
                if (!active) {
                    return S_OK;
                }
                active = false;
                return client->ReleaseBuffer(frameCount);
            }

            ~BufferRelease() {
                if (active) {
                    static_cast<void>(client->ReleaseBuffer(frameCount));
                }
            }
        } bufferRelease{captureClient_.Get(), frameCount};

        packet.frameCount = frameCount;
        packet.flags = flags;
        packet.devicePosition = devicePosition;
        const bool silent = (flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0;
        const std::uint64_t byteCount64 =
            static_cast<std::uint64_t>(frameCount) * blockAlignment_;
        if (byteCount64 > std::numeric_limits<std::size_t>::max()) {
            return E_OUTOFMEMORY;
        }
        if (!silent) {
            if (source == nullptr) {
                return E_POINTER;
            }
            try {
                packet.samples.resize(static_cast<std::size_t>(byteCount64));
            } catch (const std::bad_alloc&) {
                return E_OUTOFMEMORY;
            } catch (...) {
                return E_FAIL;
            }
            std::memcpy(packet.samples.data(), source, packet.samples.size());
        }

        result = bufferRelease.Release();
        if (FAILED(result)) {
            return result;
        }
        hasPacket = true;
        return S_OK;
    }

private:
    ComPtr<IAudioClient> audioClient_;
    ComPtr<IAudioCaptureClient> captureClient_;
    UniqueHandle sampleEvent_;
    std::uint32_t blockAlignment_{};
    bool started_{};
};

class AudioTimelineWriter final {
public:
    AudioTimelineWriter(
        media::AacAudioWriter& writer,
        const std::uint32_t sampleRate) noexcept
        : writer_(writer), sampleRate_(sampleRate) {}

    [[nodiscard]] bool ProcessPacket(
        const WasapiPacket& packet,
        const bool discard,
        SystemAudioCaptureError& error) noexcept {
        const bool timestampReliable =
            (packet.flags & AUDCLNT_BUFFERFLAGS_TIMESTAMP_ERROR) == 0;
        const bool discontinuity =
            (packet.flags & AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY) != 0;
        if (!timestampReliable || discontinuity) {
            hasDevicePosition_ = false;
            ++stats_.discontinuityCount;
        }

        if (discard) {
            stats_.discardedFramesDuringPause += packet.frameCount;
            UpdateDevicePosition(packet, timestampReliable);
            return true;
        }

        if (timestampReliable && hasDevicePosition_ &&
            packet.devicePosition > devicePositionEnd_) {
            const std::uint64_t gapFrames =
                packet.devicePosition - devicePositionEnd_;
            if (!WriteSilence(gapFrames, error)) {
                return false;
            }
        }

        const bool silent =
            (packet.flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0;
        if (!WriteFrames(
                packet.samples,
                packet.frameCount,
                silent,
                error)) {
            return false;
        }
        UpdateDevicePosition(packet, timestampReliable);
        return true;
    }

    void ResetDeviceBaseline() noexcept {
        hasDevicePosition_ = false;
        devicePositionEnd_ = 0;
    }

    [[nodiscard]] const SystemAudioCaptureStats& Stats() const noexcept {
        return stats_;
    }

private:
    void UpdateDevicePosition(
        const WasapiPacket& packet,
        const bool timestampReliable) noexcept {
        if (!timestampReliable ||
            packet.devicePosition >
                std::numeric_limits<std::uint64_t>::max() -
                    packet.frameCount) {
            hasDevicePosition_ = false;
            devicePositionEnd_ = 0;
            return;
        }
        devicePositionEnd_ = packet.devicePosition + packet.frameCount;
        hasDevicePosition_ = true;
    }

    [[nodiscard]] bool WriteSilence(
        std::uint64_t frameCount,
        SystemAudioCaptureError& error) noexcept {
        const std::uint64_t maximumChunkFrames = std::max<std::uint64_t>(
            1,
            static_cast<std::uint64_t>(sampleRate_) *
                kSilenceChunkMilliseconds / 1'000U);
        while (frameCount != 0) {
            const std::uint32_t chunkFrames = static_cast<std::uint32_t>(
                std::min(frameCount, maximumChunkFrames));
            if (!WriteFrames({}, chunkFrames, true, error)) {
                return false;
            }
            frameCount -= chunkFrames;
        }
        return true;
    }

    [[nodiscard]] bool WriteFrames(
        const std::span<const std::byte> samples,
        const std::uint32_t frameCount,
        const bool silent,
        SystemAudioCaptureError& error) noexcept {
        if (frameCount == 0) {
            return true;
        }
        const std::int64_t timestamp =
            FrameTimestamp(stats_.encodedFrames, sampleRate_);
        const std::int64_t sampleEnd = FrameTimestamp(
            stats_.encodedFrames + frameCount,
            sampleRate_);
        if (timestamp == std::numeric_limits<std::int64_t>::max() ||
            sampleEnd <= timestamp) {
            error = MakeError(
                SystemAudioCaptureErrorCode::EncodeFailed,
                L"系统音频连续时间戳超出支持范围。",
                static_cast<long>(E_INVALIDARG));
            return false;
        }

        std::wstring writerMessage;
        long writerError = 0;
        const bool succeeded = silent
                                   ? writer_.WriteSilentFrames(
                                         frameCount,
                                         timestamp,
                                         sampleEnd - timestamp,
                                         writerMessage,
                                         writerError)
                                   : writer_.WriteInterleavedFrames(
                                         samples,
                                         frameCount,
                                         timestamp,
                                         sampleEnd - timestamp,
                                         writerMessage,
                                         writerError);
        if (!succeeded) {
            error = MakeError(
                SystemAudioCaptureErrorCode::EncodeFailed,
                std::move(writerMessage),
                writerError);
            return false;
        }

        stats_.encodedFrames += frameCount;
        if (silent) {
            stats_.silentFrames += frameCount;
        }
        stats_.activeDuration = DurationForFrames(
            stats_.encodedFrames,
            sampleRate_);
        return true;
    }

    media::AacAudioWriter& writer_;
    std::uint32_t sampleRate_{};
    SystemAudioCaptureStats stats_{};
    std::uint64_t devicePositionEnd_{};
    bool hasDevicePosition_{};
};

[[nodiscard]] bool DrainAvailablePackets(
    WasapiLoopbackSession& session,
    AudioTimelineWriter& timeline,
    const bool discard,
    SystemAudioCaptureError& error) noexcept {
    WasapiPacket packet;
    for (;;) {
        bool hasPacket = false;
        const HRESULT result = session.ReadNextPacket(packet, hasPacket);
        if (FAILED(result)) {
            error = MakeWasapiError(
                SystemAudioCaptureErrorCode::CaptureFailed,
                L"读取系统回环音频包失败。",
                result);
            return false;
        }
        if (!hasPacket) {
            return true;
        }
        if (!timeline.ProcessPacket(packet, discard, error)) {
            return false;
        }
    }
}

}  // namespace

struct SystemAudioCapture::Impl final {
    mutable std::mutex stateMutex;
    std::mutex lifecycleMutex;
    std::condition_variable stateChanged;
    std::thread worker;
    UniqueHandle controlEvent;

    SystemAudioCaptureState state{SystemAudioCaptureState::Idle};
    SystemAudioCaptureStats stats{};
    bool starting{};
    bool stopRequested{};
    bool requestedPaused{};
    std::uint64_t commandGeneration{};
    std::uint64_t appliedCommandGeneration{};
    std::optional<SystemAudioRecordingResult> completedResult;
    SystemAudioCaptureError terminalError{};

    struct ControlSnapshot final {
        bool stop{};
        bool paused{};
        std::uint64_t generation{};
    };

    [[nodiscard]] ControlSnapshot ReadControl() const {
        std::scoped_lock lock(stateMutex);
        return ControlSnapshot{
            stopRequested,
            requestedPaused,
            commandGeneration,
        };
    }

    void ApplyPauseCommand(
        const bool paused,
        const std::uint64_t generation) {
        {
            std::scoped_lock lock(stateMutex);
            requestedPaused = paused;
            appliedCommandGeneration = generation;
            if (!stopRequested) {
                state = paused ? SystemAudioCaptureState::Paused
                               : SystemAudioCaptureState::Capturing;
            }
        }
        stateChanged.notify_all();
    }

    void CompleteStartupFailure(
        const SystemAudioCaptureError& error,
        const SystemAudioCaptureCallbacks& callbacks,
        std::promise<bool>& startupResult) {
        {
            std::scoped_lock lock(stateMutex);
            terminalError = error;
            starting = false;
            state = SystemAudioCaptureState::Idle;
        }
        startupResult.set_value(false);
        stateChanged.notify_all();
        InvokeSafely(callbacks.onError, error);
    }

    void Run(
        const SystemAudioCaptureConfig config,
        const SystemAudioCaptureCallbacks callbacks,
        std::promise<bool> startupResult) noexcept {
        static_cast<void>(::SetThreadDescription(
            ::GetCurrentThread(),
            L"SuperRecording.SystemAudio"));
        const ComApartment comApartment;
        if (!comApartment) {
            CompleteStartupFailure(
                MakeError(
                    SystemAudioCaptureErrorCode::ComInitializationFailed,
                    L"初始化系统音频线程 COM 环境失败。",
                    static_cast<long>(comApartment.Result())),
                callbacks,
                startupResult);
            return;
        }
        const MmcssRegistration mmcssRegistration;

        WasapiLoopbackSession session;
        SystemAudioCaptureErrorCode initializationCode =
            SystemAudioCaptureErrorCode::EndpointEnumerationFailed;
        HRESULT nativeResult = session.Initialize(config, initializationCode);
        if (FAILED(nativeResult)) {
            CompleteStartupFailure(
                MakeWasapiError(
                    initializationCode,
                    L"初始化默认渲染设备回环捕获失败。",
                    nativeResult),
                callbacks,
                startupResult);
            return;
        }

        const std::uint32_t channelMask = config.channelCount == 1
                                              ? SPEAKER_FRONT_CENTER
                                              : SPEAKER_FRONT_LEFT |
                                                    SPEAKER_FRONT_RIGHT;
        media::AacAudioWriter writer;
        media::AacAudioWriterConfig writerConfig{};
        writerConfig.outputPath = config.outputPath;
        writerConfig.inputFormat = media::InterleavedAudioFormat{
            config.sampleRate,
            config.channelCount,
            media::InterleavedAudioEncoding::IeeeFloat,
            kFloatBitsPerSample,
            kFloatBitsPerSample,
            channelMask,
        };
        writerConfig.averageBitrate = config.averageBitrate;

        std::wstring writerMessage;
        long writerError = 0;
        if (!writer.Open(writerConfig, writerMessage, writerError)) {
            CompleteStartupFailure(
                MakeError(
                    SystemAudioCaptureErrorCode::EncoderInitializationFailed,
                    std::move(writerMessage),
                    writerError),
                callbacks,
                startupResult);
            return;
        }

        nativeResult = session.Start();
        if (FAILED(nativeResult)) {
            std::wstring ignoredMessage;
            long ignoredError = 0;
            static_cast<void>(writer.Finalize(ignoredMessage, ignoredError));
            CompleteStartupFailure(
                MakeWasapiError(
                    SystemAudioCaptureErrorCode::CaptureFailed,
                    L"启动系统回环音频流失败。",
                    nativeResult),
                callbacks,
                startupResult);
            return;
        }

        {
            std::scoped_lock lock(stateMutex);
            state = SystemAudioCaptureState::Capturing;
            starting = false;
            appliedCommandGeneration = commandGeneration;
        }
        startupResult.set_value(true);
        stateChanged.notify_all();

        AudioTimelineWriter timeline(writer, config.sampleRate);
        SystemAudioCaptureError runtimeError{};
        bool capturePaused = false;
        Clock::time_point nextStatsCallback =
            Clock::now() + kStatsCallbackInterval;

        const std::array<HANDLE, 2> waitHandles{
            controlEvent.Get(),
            session.SampleEvent(),
        };
        while (!runtimeError) {
            const ControlSnapshot control = ReadControl();
            if (control.stop) {
                break;
            }

            if (control.generation != appliedCommandGeneration) {
                if (!DrainAvailablePackets(
                        session,
                        timeline,
                        true,
                        runtimeError)) {
                    break;
                }
                capturePaused = control.paused;
                if (!capturePaused) {
                    timeline.ResetDeviceBaseline();
                }
                ApplyPauseCommand(capturePaused, control.generation);
            }

            const DWORD waitResult = ::WaitForMultipleObjects(
                static_cast<DWORD>(waitHandles.size()),
                waitHandles.data(),
                FALSE,
                static_cast<DWORD>(kCaptureWaitTimeout.count()));
            if (waitResult == WAIT_FAILED) {
                const DWORD lastError = ::GetLastError();
                runtimeError = MakeError(
                    SystemAudioCaptureErrorCode::CaptureFailed,
                    L"等待系统音频回环事件失败。",
                    static_cast<long>(HRESULT_FROM_WIN32(lastError)));
                break;
            }

            if (!DrainAvailablePackets(
                    session,
                    timeline,
                    capturePaused,
                    runtimeError)) {
                break;
            }

            const Clock::time_point now = Clock::now();
            if (now >= nextStatsCallback) {
                const SystemAudioCaptureStats snapshot = timeline.Stats();
                {
                    std::scoped_lock lock(stateMutex);
                    stats = snapshot;
                }
                InvokeSafely(callbacks.onStats, snapshot);
                nextStatsCallback = now + kStatsCallbackInterval;
            }
        }

        if (!runtimeError) {
            static_cast<void>(DrainAvailablePackets(
                session,
                timeline,
                capturePaused,
                runtimeError));
        }

        nativeResult = session.Stop();
        if (FAILED(nativeResult) && !runtimeError) {
            runtimeError = MakeWasapiError(
                SystemAudioCaptureErrorCode::CaptureFailed,
                L"停止系统回环音频流失败。",
                nativeResult);
        }

        SystemAudioCaptureError finalizeError{};
        if (!writer.Finalize(writerMessage, writerError)) {
            finalizeError = MakeError(
                SystemAudioCaptureErrorCode::FinalizeFailed,
                std::move(writerMessage),
                writerError);
            if (!runtimeError) {
                runtimeError = finalizeError;
            }
        }

        const SystemAudioCaptureStats finalStats = timeline.Stats();
        std::optional<SystemAudioRecordingResult> result;
        if (!runtimeError) {
            result = SystemAudioRecordingResult{
                config.outputPath,
                finalStats.activeDuration,
                config.sampleRate,
                config.channelCount,
                writerConfig.averageBitrate != 0
                    ? writerConfig.averageBitrate
                    : media::AacAudioWriter::RecommendBitrate(
                          config.channelCount),
                finalStats.encodedFrames,
                finalStats.silentFrames,
            };
        }

        {
            std::scoped_lock lock(stateMutex);
            stats = finalStats;
            completedResult = result;
            terminalError = runtimeError;
            stopRequested = false;
            requestedPaused = false;
            starting = false;
            state = SystemAudioCaptureState::Idle;
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

SystemAudioCapture::SystemAudioCapture() : impl_(std::make_unique<Impl>()) {}

SystemAudioCapture::~SystemAudioCapture() {
    static_cast<void>(Stop(nullptr));
}

bool SystemAudioCapture::Start(
    const SystemAudioCaptureConfig& config,
    SystemAudioCaptureCallbacks callbacks,
    SystemAudioCaptureError* error) {
    std::scoped_lock lifecycleLock(impl_->lifecycleMutex);
    ClearError(error);

    {
        std::scoped_lock lock(impl_->stateMutex);
        if (impl_->state != SystemAudioCaptureState::Idle || impl_->starting) {
            AssignError(
                error,
                MakeError(
                    SystemAudioCaptureErrorCode::InvalidState,
                    L"已有系统音频录制任务正在运行。"));
            return false;
        }
    }
    if (impl_->worker.joinable()) {
        impl_->worker.join();
    }

    const bool sampleRateSupported =
        config.sampleRate == kDefaultSampleRate ||
        config.sampleRate == kAlternateSampleRate;
    if (config.outputPath.empty() || !sampleRateSupported ||
        config.channelCount < kMinimumChannelCount ||
        config.channelCount > kMaximumChannelCount) {
        AssignError(
            error,
            MakeError(
                SystemAudioCaptureErrorCode::InvalidArgument,
                L"系统音频输出路径、采样率或声道数无效。"));
        return false;
    }

    impl_->controlEvent.Reset(::CreateEventW(nullptr, FALSE, FALSE, nullptr));
    if (!impl_->controlEvent) {
        AssignError(
            error,
            MakeError(
                SystemAudioCaptureErrorCode::EventInitializationFailed,
                L"创建系统音频控制事件失败。",
                static_cast<long>(HRESULT_FROM_WIN32(::GetLastError()))));
        return false;
    }

    {
        std::scoped_lock lock(impl_->stateMutex);
        impl_->state = SystemAudioCaptureState::Starting;
        impl_->stats = {};
        impl_->completedResult.reset();
        impl_->terminalError = {};
        impl_->stopRequested = false;
        impl_->requestedPaused = false;
        impl_->commandGeneration = 0;
        impl_->appliedCommandGeneration = 0;
        impl_->starting = true;
    }

    std::promise<bool> startupPromise;
    std::future<bool> startupFuture = startupPromise.get_future();
    try {
        impl_->worker = std::thread(
            [implementation = impl_.get(),
             config,
             callbacks = std::move(callbacks),
             promise = std::move(startupPromise)]() mutable {
                implementation->Run(
                    config,
                    callbacks,
                    std::move(promise));
            });
    } catch (...) {
        const SystemAudioCaptureError creationError = MakeError(
            SystemAudioCaptureErrorCode::ThreadCreationFailed,
            L"创建系统音频捕获线程失败。",
            static_cast<long>(E_OUTOFMEMORY));
        {
            std::scoped_lock lock(impl_->stateMutex);
            impl_->state = SystemAudioCaptureState::Idle;
            impl_->starting = false;
            impl_->terminalError = creationError;
        }
        AssignError(error, creationError);
        return false;
    }

    bool started = false;
    try {
        started = startupFuture.get();
    } catch (...) {
        const SystemAudioCaptureError startupError = MakeError(
            SystemAudioCaptureErrorCode::CaptureFailed,
            L"系统音频捕获线程在初始化期间异常退出。",
            static_cast<long>(E_UNEXPECTED));
        {
            std::scoped_lock lock(impl_->stateMutex);
            impl_->terminalError = startupError;
            impl_->starting = false;
            impl_->state = SystemAudioCaptureState::Idle;
        }
    }
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

bool SystemAudioCapture::Pause(SystemAudioCaptureError* error) {
    std::scoped_lock lifecycleLock(impl_->lifecycleMutex);
    ClearError(error);
    if (impl_->worker.joinable() &&
        impl_->worker.get_id() == std::this_thread::get_id()) {
        AssignError(
            error,
            MakeError(
                SystemAudioCaptureErrorCode::InvalidState,
                L"系统音频回调线程不能同步暂停自身。"));
        return false;
    }

    std::unique_lock lock(impl_->stateMutex);
    if (impl_->state != SystemAudioCaptureState::Capturing ||
        impl_->requestedPaused) {
        AssignError(
            error,
            MakeError(
                SystemAudioCaptureErrorCode::InvalidState,
                L"当前系统音频状态不能暂停。"));
        return false;
    }

    impl_->requestedPaused = true;
    const std::uint64_t generation = ++impl_->commandGeneration;
    static_cast<void>(::SetEvent(impl_->controlEvent.Get()));
    impl_->stateChanged.wait(lock, [this, generation] {
        return impl_->appliedCommandGeneration >= generation ||
               impl_->state == SystemAudioCaptureState::Idle ||
               impl_->state == SystemAudioCaptureState::Finalizing;
    });
    if (impl_->appliedCommandGeneration < generation) {
        AssignError(error, impl_->terminalError);
        return false;
    }
    return true;
}

bool SystemAudioCapture::Resume(SystemAudioCaptureError* error) {
    std::scoped_lock lifecycleLock(impl_->lifecycleMutex);
    ClearError(error);
    if (impl_->worker.joinable() &&
        impl_->worker.get_id() == std::this_thread::get_id()) {
        AssignError(
            error,
            MakeError(
                SystemAudioCaptureErrorCode::InvalidState,
                L"系统音频回调线程不能同步继续自身。"));
        return false;
    }

    std::unique_lock lock(impl_->stateMutex);
    if (impl_->state != SystemAudioCaptureState::Paused ||
        !impl_->requestedPaused) {
        AssignError(
            error,
            MakeError(
                SystemAudioCaptureErrorCode::InvalidState,
                L"当前系统音频状态不能继续。"));
        return false;
    }

    impl_->requestedPaused = false;
    const std::uint64_t generation = ++impl_->commandGeneration;
    static_cast<void>(::SetEvent(impl_->controlEvent.Get()));
    impl_->stateChanged.wait(lock, [this, generation] {
        return impl_->appliedCommandGeneration >= generation ||
               impl_->state == SystemAudioCaptureState::Idle ||
               impl_->state == SystemAudioCaptureState::Finalizing;
    });
    if (impl_->appliedCommandGeneration < generation) {
        AssignError(error, impl_->terminalError);
        return false;
    }
    return true;
}

std::optional<SystemAudioRecordingResult> SystemAudioCapture::Stop(
    SystemAudioCaptureError* error) {
    std::scoped_lock lifecycleLock(impl_->lifecycleMutex);
    ClearError(error);

    {
        std::scoped_lock lock(impl_->stateMutex);
        if (impl_->state != SystemAudioCaptureState::Idle || impl_->starting) {
            impl_->stopRequested = true;
            impl_->state = SystemAudioCaptureState::Finalizing;
            static_cast<void>(::SetEvent(impl_->controlEvent.Get()));
        }
    }

    if (impl_->worker.joinable()) {
        if (impl_->worker.get_id() == std::this_thread::get_id()) {
            AssignError(
                error,
                MakeError(
                    SystemAudioCaptureErrorCode::InvalidState,
                    L"系统音频回调线程只能请求停止；结果通过完成回调接收。"));
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

SystemAudioCaptureState SystemAudioCapture::State() const noexcept {
    std::scoped_lock lock(impl_->stateMutex);
    return impl_->state;
}

SystemAudioCaptureStats SystemAudioCapture::Stats() const noexcept {
    std::scoped_lock lock(impl_->stateMutex);
    return impl_->stats;
}

SystemAudioCaptureError SystemAudioCapture::LastError() const {
    std::scoped_lock lock(impl_->stateMutex);
    return impl_->terminalError;
}

}  // namespace qrec::capture
