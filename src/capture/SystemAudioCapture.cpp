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
#include <deque>
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
inline constexpr std::uint32_t kMinimumAacSeedMilliseconds = 100;
inline constexpr auto kCaptureWaitTimeout = 250ms;
inline constexpr auto kStatsCallbackInterval = 250ms;
inline constexpr auto kSilenceCatchUpHoldback = kCaptureWaitTimeout;
inline constexpr auto kPendingPacketBudget =
    kSilenceCatchUpHoldback +
    2 * std::chrono::milliseconds(kRequestedBufferMilliseconds);
inline constexpr auto kMaximumPacketFutureSkew =
    2 * std::chrono::milliseconds(kRequestedBufferMilliseconds);

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

[[nodiscard]] std::uint64_t FramesForDurationCeiling(
    const std::chrono::nanoseconds duration,
    const std::uint32_t sampleRate) noexcept {
    if (duration <= std::chrono::nanoseconds::zero() || sampleRate == 0) {
        return 0;
    }

    constexpr std::uint64_t kNanosecondsPerSecond = 1'000'000'000ULL;
    const std::uint64_t nanoseconds =
        static_cast<std::uint64_t>(duration.count());
    const std::uint64_t wholeSeconds = nanoseconds / kNanosecondsPerSecond;
    const std::uint64_t remainingNanoseconds =
        nanoseconds % kNanosecondsPerSecond;
    const std::uint64_t maximumWholeSeconds =
        std::numeric_limits<std::uint64_t>::max() / sampleRate;
    if (wholeSeconds > maximumWholeSeconds) {
        return std::numeric_limits<std::uint64_t>::max();
    }

    const std::uint64_t wholeFrames = wholeSeconds * sampleRate;
    const std::uint64_t partialFrames =
        (remainingNanoseconds * sampleRate + kNanosecondsPerSecond - 1ULL) /
        kNanosecondsPerSecond;
    if (partialFrames >
        std::numeric_limits<std::uint64_t>::max() - wholeFrames) {
        return std::numeric_limits<std::uint64_t>::max();
    }
    return wholeFrames + partialFrames;
}

[[nodiscard]] std::uint64_t FramesForHundredNanoseconds(
    const std::uint64_t duration100Nanoseconds,
    const std::uint32_t sampleRate,
    const bool roundUp) noexcept {
    constexpr std::uint64_t kTicksPerSecond =
        static_cast<std::uint64_t>(kMediaFoundationTicksPerSecond);
    if (sampleRate == 0) {
        return std::numeric_limits<std::uint64_t>::max();
    }
    const std::uint64_t wholeSeconds =
        duration100Nanoseconds / kTicksPerSecond;
    const std::uint64_t remainingTicks =
        duration100Nanoseconds % kTicksPerSecond;
    const std::uint64_t maximumWholeSeconds =
        std::numeric_limits<std::uint64_t>::max() / sampleRate;
    if (wholeSeconds > maximumWholeSeconds) {
        return std::numeric_limits<std::uint64_t>::max();
    }

    const std::uint64_t wholeFrames = wholeSeconds * sampleRate;
    std::uint64_t partialNumerator = remainingTicks * sampleRate;
    if (roundUp && partialNumerator != 0) {
        partialNumerator += kTicksPerSecond - 1ULL;
    }
    const std::uint64_t partialFrames = partialNumerator / kTicksPerSecond;
    if (partialFrames >
        std::numeric_limits<std::uint64_t>::max() - wholeFrames) {
        return std::numeric_limits<std::uint64_t>::max();
    }
    return wholeFrames + partialFrames;
}

[[nodiscard]] std::uint64_t HundredNanosecondsForFramesCeiling(
    const std::uint64_t frameCount,
    const std::uint32_t sampleRate) noexcept {
    constexpr std::uint64_t kTicksPerSecond =
        static_cast<std::uint64_t>(kMediaFoundationTicksPerSecond);
    if (frameCount == 0 || sampleRate == 0) {
        return 0;
    }

    const std::uint64_t wholeSeconds = frameCount / sampleRate;
    const std::uint64_t remainingFrames = frameCount % sampleRate;
    if (wholeSeconds >
        std::numeric_limits<std::uint64_t>::max() / kTicksPerSecond) {
        return std::numeric_limits<std::uint64_t>::max();
    }
    const std::uint64_t wholeTicks = wholeSeconds * kTicksPerSecond;
    const std::uint64_t partialTicks =
        (remainingFrames * kTicksPerSecond + sampleRate - 1ULL) /
        sampleRate;
    if (partialTicks >
        std::numeric_limits<std::uint64_t>::max() - wholeTicks) {
        return std::numeric_limits<std::uint64_t>::max();
    }
    return wholeTicks + partialTicks;
}

[[nodiscard]] std::optional<std::uint64_t>
QueryPerformanceCounter100Nanoseconds() noexcept {
    LARGE_INTEGER counter{};
    LARGE_INTEGER frequency{};
    if (!::QueryPerformanceCounter(&counter) ||
        !::QueryPerformanceFrequency(&frequency) ||
        counter.QuadPart < 0 || frequency.QuadPart <= 0) {
        return std::nullopt;
    }

    constexpr std::uint64_t kTicksPerSecond =
        static_cast<std::uint64_t>(kMediaFoundationTicksPerSecond);
    const std::uint64_t counterValue =
        static_cast<std::uint64_t>(counter.QuadPart);
    const std::uint64_t frequencyValue =
        static_cast<std::uint64_t>(frequency.QuadPart);
    const std::uint64_t wholeSeconds = counterValue / frequencyValue;
    const std::uint64_t remainder = counterValue % frequencyValue;
    if (wholeSeconds >
            std::numeric_limits<std::uint64_t>::max() / kTicksPerSecond ||
        remainder >
            std::numeric_limits<std::uint64_t>::max() / kTicksPerSecond) {
        return std::nullopt;
    }

    const std::uint64_t wholeTicks = wholeSeconds * kTicksPerSecond;
    const std::uint64_t partialTicks =
        remainder * kTicksPerSecond / frequencyValue;
    if (partialTicks >
        std::numeric_limits<std::uint64_t>::max() - wholeTicks) {
        return std::nullopt;
    }
    return wholeTicks + partialTicks;
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
    std::uint64_t qpcPosition100Nanoseconds{};
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
        packet.qpcPosition100Nanoseconds = 0;
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
        packet.qpcPosition100Nanoseconds = qpcPosition;
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
        const std::uint32_t sampleRate,
        const std::uint32_t bytesPerFrame) noexcept
        : writer_(writer),
          sampleRate_(sampleRate),
          bytesPerFrame_(bytesPerFrame) {}

    [[nodiscard]] bool ProcessPacket(
        const WasapiPacket& packet,
        const bool discard,
        const std::optional<SystemAudioQpcPosition> endBoundaryQpc,
        SystemAudioCaptureError& error,
        const std::optional<std::uint64_t> maximumEncodedFrames =
            std::nullopt) noexcept {
        const bool timestampReliable =
            (packet.flags & AUDCLNT_BUFFERFLAGS_TIMESTAMP_ERROR) == 0;
        const bool discontinuity =
            (packet.flags & AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY) != 0;
        if (!timestampReliable || discontinuity) {
            ResetDeviceBaseline();
            alignmentRequired_ =
                segmentQpcStart100Nanoseconds_.has_value();
            ++stats_.discontinuityCount;
        }

        if (discard) {
            stats_.discardedFramesDuringPause += packet.frameCount;
            UpdateDevicePosition(packet, timestampReliable);
            return true;
        }

        if (maximumEncodedFrames.has_value() &&
            stats_.encodedFrames >= *maximumEncodedFrames) {
            UpdateDevicePosition(packet, timestampReliable);
            return true;
        }

        std::uint32_t availableFrameCount = packet.frameCount;
        if (endBoundaryQpc.has_value()) {
            if (!timestampReliable ||
                packet.qpcPosition100Nanoseconds == 0 ||
                packet.qpcPosition100Nanoseconds >= *endBoundaryQpc) {
                UpdateDevicePosition(packet, timestampReliable);
                return true;
            }

            const std::uint64_t boundaryFrames =
                FramesForHundredNanoseconds(
                    *endBoundaryQpc - packet.qpcPosition100Nanoseconds,
                    sampleRate_,
                    false);
            availableFrameCount = static_cast<std::uint32_t>(
                std::min<std::uint64_t>(
                    boundaryFrames,
                    packet.frameCount));
            if (availableFrameCount == 0) {
                UpdateDevicePosition(packet, timestampReliable);
                return true;
            }
        }

        std::uint64_t desiredStartFrame = stats_.encodedFrames;
        std::uint64_t framesToSkip = 0;
        bool alignedWithQpc = false;
        if (alignmentRequired_) {
            if (!timestampReliable ||
                !segmentQpcStart100Nanoseconds_.has_value() ||
                packet.qpcPosition100Nanoseconds == 0) {
                return true;
            }

            alignedWithQpc = true;
            if (packet.qpcPosition100Nanoseconds <
                *segmentQpcStart100Nanoseconds_) {
                framesToSkip = std::min<std::uint64_t>(
                    FramesForHundredNanoseconds(
                        *segmentQpcStart100Nanoseconds_ -
                            packet.qpcPosition100Nanoseconds,
                        sampleRate_,
                        true),
                    availableFrameCount);
                desiredStartFrame = segmentTimelineStartFrame_;
            } else {
                const std::uint64_t segmentOffset =
                    FramesForHundredNanoseconds(
                        packet.qpcPosition100Nanoseconds -
                            *segmentQpcStart100Nanoseconds_,
                        sampleRate_,
                        false);
                if (segmentOffset >
                    std::numeric_limits<std::uint64_t>::max() -
                        segmentTimelineStartFrame_) {
                    error = MakeError(
                        SystemAudioCaptureErrorCode::EncodeFailed,
                        L"系统音频 QPC 时间线超出支持范围。",
                        static_cast<long>(E_INVALIDARG));
                    return false;
                }
                desiredStartFrame =
                    segmentTimelineStartFrame_ + segmentOffset;
            }

            if (desiredStartFrame > stats_.encodedFrames) {
                if (maximumEncodedFrames.has_value()) {
                    desiredStartFrame = std::min(
                        desiredStartFrame,
                        *maximumEncodedFrames);
                }
                if (!WriteSilence(
                        desiredStartFrame - stats_.encodedFrames,
                        error)) {
                    return false;
                }
                if (maximumEncodedFrames.has_value() &&
                    stats_.encodedFrames >= *maximumEncodedFrames) {
                    UpdateDevicePosition(packet, timestampReliable);
                    return true;
                }
            } else if (desiredStartFrame < stats_.encodedFrames &&
                       framesToSkip < availableFrameCount) {
                const std::uint64_t remainingFrames =
                    availableFrameCount - framesToSkip;
                framesToSkip += std::min<std::uint64_t>(
                    stats_.encodedFrames - desiredStartFrame,
                    remainingFrames);
            }
        } else if (timestampReliable && hasDevicePosition_) {
            if (packet.devicePosition > devicePositionEnd_) {
                std::uint64_t gapFrames =
                    packet.devicePosition - devicePositionEnd_;
                const std::uint64_t maximumRecoverableGapFrames =
                    FramesForDurationCeiling(
                        std::chrono::duration_cast<std::chrono::nanoseconds>(
                            kPendingPacketBudget),
                        sampleRate_);
                if (gapFrames > maximumRecoverableGapFrames) {
                    // 未带 discontinuity 标记的异常设备位置不能触发无界
                    // 静音写入；下一包重新建立设备基线。
                    ResetDeviceBaseline();
                    ++stats_.discontinuityCount;
                    gapFrames = 0;
                }
                if (maximumEncodedFrames.has_value()) {
                    gapFrames = std::min(
                        gapFrames,
                        *maximumEncodedFrames - stats_.encodedFrames);
                }
                if (gapFrames != 0 && !WriteSilence(gapFrames, error)) {
                    return false;
                }
                if (maximumEncodedFrames.has_value() &&
                    stats_.encodedFrames >= *maximumEncodedFrames) {
                    UpdateDevicePosition(packet, timestampReliable);
                    return true;
                }
            } else if (packet.devicePosition < devicePositionEnd_) {
                framesToSkip = std::min<std::uint64_t>(
                    devicePositionEnd_ - packet.devicePosition,
                    availableFrameCount);
            }
        }

        if (framesToSkip >= availableFrameCount) {
            UpdateDevicePosition(packet, timestampReliable);
            return true;
        }

        const std::uint32_t skippedFrames =
            static_cast<std::uint32_t>(framesToSkip);
        std::uint32_t outputFrames =
            availableFrameCount - skippedFrames;
        if (maximumEncodedFrames.has_value()) {
            if (stats_.encodedFrames >= *maximumEncodedFrames) {
                UpdateDevicePosition(packet, timestampReliable);
                return true;
            }
            const std::uint64_t remainingFrames =
                *maximumEncodedFrames - stats_.encodedFrames;
            outputFrames = static_cast<std::uint32_t>(
                std::min<std::uint64_t>(outputFrames, remainingFrames));
            if (outputFrames == 0) {
                UpdateDevicePosition(packet, timestampReliable);
                return true;
            }
        }
        const bool silent =
            (packet.flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0;
        std::span<const std::byte> outputSamples = packet.samples;
        if (!silent && skippedFrames != 0) {
            const std::uint64_t skippedBytes =
                static_cast<std::uint64_t>(skippedFrames) * bytesPerFrame_;
            if (skippedBytes > outputSamples.size()) {
                error = MakeError(
                    SystemAudioCaptureErrorCode::EncodeFailed,
                    L"系统音频重叠样本的缓冲区长度无效。",
                    static_cast<long>(E_INVALIDARG));
                return false;
            }
            outputSamples = outputSamples.subspan(
                static_cast<std::size_t>(skippedBytes));
        }

        if (!WriteFrames(
                outputSamples,
                outputFrames,
                silent,
                error)) {
            return false;
        }
        if (alignedWithQpc) {
            alignmentRequired_ = false;
        }
        UpdateDevicePosition(packet, timestampReliable);
        return true;
    }

    void BeginActiveSegment(
        const std::optional<std::uint64_t> qpcPosition100Nanoseconds,
        const std::uint64_t timelineStartFrame) noexcept {
        segmentQpcStart100Nanoseconds_ = qpcPosition100Nanoseconds;
        segmentTimelineStartFrame_ = timelineStartFrame;
        ResetDeviceBaseline();
        alignmentRequired_ = qpcPosition100Nanoseconds.has_value();
    }

    [[nodiscard]] bool EndActiveSegment(
        const std::optional<SystemAudioQpcPosition> boundaryQpc,
        SystemAudioCaptureError& error) noexcept {
        if (!boundaryQpc.has_value() ||
            !segmentQpcStart100Nanoseconds_.has_value()) {
            return true;
        }

        std::uint64_t targetFrame = segmentTimelineStartFrame_;
        if (*boundaryQpc > *segmentQpcStart100Nanoseconds_) {
            const std::uint64_t segmentFrames =
                FramesForHundredNanoseconds(
                    *boundaryQpc - *segmentQpcStart100Nanoseconds_,
                    sampleRate_,
                    false);
            if (segmentFrames >
                std::numeric_limits<std::uint64_t>::max() - targetFrame) {
                error = MakeError(
                    SystemAudioCaptureErrorCode::EncodeFailed,
                    L"系统音频活动片段边界超出支持范围。",
                    static_cast<long>(E_INVALIDARG));
                return false;
            }
            targetFrame += segmentFrames;
        }
        return EnsureExactFrameCount(targetFrame, error);
    }

    void ResetDeviceBaseline() noexcept {
        hasDevicePosition_ = false;
        devicePositionEnd_ = 0;
    }

    [[nodiscard]] const SystemAudioCaptureStats& Stats() const noexcept {
        return stats_;
    }

    [[nodiscard]] bool EnsureMinimumDuration(
        const std::chrono::nanoseconds minimumDuration,
        SystemAudioCaptureError& error,
        const bool ensureAacSeed = false) noexcept {
        std::uint64_t targetFrames = FramesForDurationCeiling(
            minimumDuration,
            sampleRate_);
        if (ensureAacSeed && targetFrames != 0) {
            targetFrames = std::max(
                targetFrames,
                static_cast<std::uint64_t>(sampleRate_) *
                    kMinimumAacSeedMilliseconds / 1'000U);
        }
        if (targetFrames <= stats_.encodedFrames) {
            return true;
        }
        if (!WriteSilence(targetFrames - stats_.encodedFrames, error)) {
            return false;
        }
        ResetDeviceBaseline();
        alignmentRequired_ =
            segmentQpcStart100Nanoseconds_.has_value();
        return true;
    }

    [[nodiscard]] bool EnsureExactDuration(
        const std::chrono::nanoseconds exactDuration,
        SystemAudioCaptureError& error) noexcept {
        const std::uint64_t targetFrames = FramesForDurationCeiling(
            exactDuration,
            sampleRate_);
        if (targetFrames == 0 && stats_.encodedFrames == 0) {
            // Sink Writer 不能封口一个从未接收样本的 M4A。写入仅用于让
            // Finalize 安全完成；零时长调用不能返回一条伪装成 exact 的音轨。
            if (!WriteSilence(
                    std::max<std::uint64_t>(
                        1,
                        static_cast<std::uint64_t>(sampleRate_) *
                            kMinimumAacSeedMilliseconds / 1'000U),
                    error)) {
                return false;
            }
            error = MakeError(
                SystemAudioCaptureErrorCode::EncodeFailed,
                L"系统音频的目标时长为零，未生成可用音轨。",
                static_cast<long>(E_INVALIDARG));
            return false;
        }
        return EnsureExactFrameCount(targetFrames, error);
    }

    [[nodiscard]] bool CatchUpSilence(
        const std::chrono::nanoseconds activeDuration,
        SystemAudioCaptureError& error) noexcept {
        if (!segmentQpcStart100Nanoseconds_.has_value()) {
            // QPC unavailable: preserve the device-position timeline and let
            // Stop perform the final bounded silence fill.
            return true;
        }

        const auto holdback =
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                kSilenceCatchUpHoldback);
        if (activeDuration <= holdback) {
            return true;
        }
        const std::chrono::nanoseconds committedDuration =
            activeDuration - holdback;
        const std::uint64_t targetFrames = FramesForDurationCeiling(
            committedDuration,
            sampleRate_);
        if (targetFrames <= stats_.encodedFrames) {
            return true;
        }
        return EnsureMinimumDuration(committedDuration, error);
    }

private:
    [[nodiscard]] bool EnsureExactFrameCount(
        const std::uint64_t targetFrames,
        SystemAudioCaptureError& error) noexcept {
        if (stats_.encodedFrames > targetFrames) {
            error = MakeError(
                SystemAudioCaptureErrorCode::EncodeFailed,
                L"系统音频已越过共享时间线边界，已停用本次音轨以避免音画错位。",
                static_cast<long>(E_UNEXPECTED));
            return false;
        }
        if (stats_.encodedFrames < targetFrames &&
            !WriteSilence(targetFrames - stats_.encodedFrames, error)) {
            return false;
        }
        ResetDeviceBaseline();
        alignmentRequired_ =
            segmentQpcStart100Nanoseconds_.has_value();
        return true;
    }

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
        const std::uint64_t packetEnd =
            packet.devicePosition + packet.frameCount;
        devicePositionEnd_ = hasDevicePosition_
            ? std::max(devicePositionEnd_, packetEnd)
            : packetEnd;
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
    std::uint32_t bytesPerFrame_{};
    SystemAudioCaptureStats stats_{};
    std::uint64_t devicePositionEnd_{};
    std::uint64_t segmentTimelineStartFrame_{};
    std::optional<std::uint64_t> segmentQpcStart100Nanoseconds_;
    bool hasDevicePosition_{};
    bool alignmentRequired_{};
};

[[nodiscard]] bool DrainAvailablePackets(
    WasapiLoopbackSession& session,
    AudioTimelineWriter& timeline,
    const bool discard,
    SystemAudioCaptureError& error,
    const std::optional<SystemAudioQpcPosition> endBoundaryQpc =
        std::nullopt) noexcept {
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
        if (!timeline.ProcessPacket(
                packet,
                discard,
                endBoundaryQpc,
                error)) {
            return false;
        }
    }
}

class PendingPacketQueue final {
public:
    [[nodiscard]] bool Push(
        WasapiPacket packet,
        SystemAudioCaptureError& error) noexcept {
        const std::uint64_t packetBytes = packet.samples.size();
        if (packet.frameCount >
                std::numeric_limits<std::uint64_t>::max() - queuedFrames_ ||
            packetBytes >
                std::numeric_limits<std::uint64_t>::max() - queuedBytes_) {
            error = MakeError(
                SystemAudioCaptureErrorCode::CaptureFailed,
                L"系统音频预卷队列计数超出支持范围。",
                static_cast<long>(E_INVALIDARG));
            return false;
        }

        try {
            packets_.push_back(std::move(packet));
        } catch (const std::bad_alloc&) {
            error = MakeError(
                SystemAudioCaptureErrorCode::CaptureFailed,
                L"系统音频预卷队列内存不足。",
                static_cast<long>(E_OUTOFMEMORY));
            return false;
        } catch (...) {
            error = MakeError(
                SystemAudioCaptureErrorCode::CaptureFailed,
                L"系统音频预卷队列写入失败。",
                static_cast<long>(E_FAIL));
            return false;
        }

        queuedFrames_ += packet.frameCount;
        queuedBytes_ += packetBytes;
        return true;
    }

    [[nodiscard]] bool Empty() const noexcept {
        return packets_.empty();
    }

    [[nodiscard]] WasapiPacket& Front() noexcept {
        return packets_.front();
    }

    void PopFront() noexcept {
        const WasapiPacket& packet = packets_.front();
        queuedFrames_ -= packet.frameCount;
        queuedBytes_ -= packet.samples.size();
        packets_.pop_front();
    }

    void Clear() noexcept {
        packets_.clear();
        queuedFrames_ = 0;
        queuedBytes_ = 0;
    }

    [[nodiscard]] bool ExceedsBudget(
        const std::uint32_t sampleRate,
        const std::uint32_t bytesPerFrame) const noexcept {
        const std::uint64_t maximumFrames = FramesForDurationCeiling(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                kPendingPacketBudget),
            sampleRate);
        const std::uint64_t maximumBytes =
            maximumFrames >
                    std::numeric_limits<std::uint64_t>::max() / bytesPerFrame
                ? std::numeric_limits<std::uint64_t>::max()
                : maximumFrames * bytesPerFrame;
        return queuedFrames_ > maximumFrames ||
            queuedBytes_ > maximumBytes ||
            packets_.size() > maximumFrames;
    }

private:
    std::deque<WasapiPacket> packets_;
    std::uint64_t queuedFrames_{};
    std::uint64_t queuedBytes_{};
};

[[nodiscard]] bool CollectAvailablePackets(
    WasapiLoopbackSession& session,
    PendingPacketQueue& pendingPackets,
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
        if (!pendingPackets.Push(std::move(packet), error)) {
            return false;
        }
        packet = {};
    }
}

[[nodiscard]] std::optional<SystemAudioQpcPosition> PacketEndQpc(
    const WasapiPacket& packet,
    const std::uint32_t sampleRate) noexcept {
    if ((packet.flags & AUDCLNT_BUFFERFLAGS_TIMESTAMP_ERROR) != 0 ||
        packet.qpcPosition100Nanoseconds == 0) {
        return std::nullopt;
    }
    const std::uint64_t duration = HundredNanosecondsForFramesCeiling(
        packet.frameCount,
        sampleRate);
    if (duration >
        std::numeric_limits<std::uint64_t>::max() -
            packet.qpcPosition100Nanoseconds) {
        return std::nullopt;
    }
    return packet.qpcPosition100Nanoseconds + duration;
}

[[nodiscard]] std::optional<SystemAudioQpcPosition>
MaturePacketBoundaryQpc() noexcept {
    const std::optional<SystemAudioQpcPosition> now =
        QueryPerformanceCounter100Nanoseconds();
    if (!now.has_value()) {
        return std::nullopt;
    }
    using HundredNanoseconds =
        std::chrono::duration<std::int64_t, std::ratio<1, 10'000'000>>;
    const std::int64_t holdback =
        std::chrono::duration_cast<HundredNanoseconds>(
            kSilenceCatchUpHoldback).count();
    if (holdback <= 0 ||
        *now <= static_cast<std::uint64_t>(holdback)) {
        return 0;
    }
    return *now - static_cast<std::uint64_t>(holdback);
}

[[nodiscard]] bool FlushPendingPackets(
    PendingPacketQueue& pendingPackets,
    AudioTimelineWriter& timeline,
    const std::uint32_t sampleRate,
    const std::uint32_t bytesPerFrame,
    const bool flushAll,
    const std::optional<SystemAudioQpcPosition> commitBoundaryQpc,
    const std::optional<SystemAudioQpcPosition> endBoundaryQpc,
    SystemAudioCaptureError& error,
    const std::optional<std::uint64_t> maximumEncodedFrames =
        std::nullopt) noexcept {
    const std::optional<SystemAudioQpcPosition> localNow =
        QueryPerformanceCounter100Nanoseconds();
    using HundredNanoseconds =
        std::chrono::duration<std::int64_t, std::ratio<1, 10'000'000>>;
    const std::int64_t futureSkewCount =
        std::chrono::duration_cast<HundredNanoseconds>(
            kMaximumPacketFutureSkew).count();
    const std::uint64_t maximumPlausibleQpc =
        localNow.has_value() && futureSkewCount > 0 &&
                static_cast<std::uint64_t>(futureSkewCount) <=
                    std::numeric_limits<std::uint64_t>::max() - *localNow
            ? *localNow + static_cast<std::uint64_t>(futureSkewCount)
            : std::numeric_limits<std::uint64_t>::max();

    while (!pendingPackets.Empty()) {
        WasapiPacket& packet = pendingPackets.Front();
        bool forceSequential = pendingPackets.ExceedsBudget(
            sampleRate,
            bytesPerFrame);
        std::optional<SystemAudioQpcPosition> packetEnd =
            PacketEndQpc(packet, sampleRate);
        if (localNow.has_value() &&
            (packet.qpcPosition100Nanoseconds > maximumPlausibleQpc ||
             (packetEnd.has_value() &&
              *packetEnd > maximumPlausibleQpc))) {
            forceSequential = true;
        }

        if (forceSequential) {
            // 设备驱动的未来/跳变 QPC 不能阻塞队首。保留样本顺序，放弃
            // 该包的时间戳并让下一可靠包重新锚定。
            packet.flags |= AUDCLNT_BUFFERFLAGS_TIMESTAMP_ERROR |
                AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY;
            packet.qpcPosition100Nanoseconds = 0;
            packetEnd.reset();
        }

        if (!flushAll && commitBoundaryQpc.has_value()) {
            if (packetEnd.has_value() &&
                *packetEnd > *commitBoundaryQpc) {
                break;
            }
        }

        const bool timestampUnavailable = !packetEnd.has_value();
        const std::optional<SystemAudioQpcPosition> packetBoundary =
            timestampUnavailable
                ? std::nullopt
                : flushAll ? endBoundaryQpc : commitBoundaryQpc;
        if (!timeline.ProcessPacket(
                packet,
                false,
                packetBoundary,
                error,
                maximumEncodedFrames)) {
            return false;
        }
        pendingPackets.PopFront();
    }
    return true;
}

[[nodiscard]] bool TrimPausedPreRollToBudget(
    PendingPacketQueue& pendingPackets,
    AudioTimelineWriter& timeline,
    const std::uint32_t sampleRate,
    const std::uint32_t bytesPerFrame,
    SystemAudioCaptureError& error) noexcept {
    while (!pendingPackets.Empty() &&
           pendingPackets.ExceedsBudget(sampleRate, bytesPerFrame)) {
        if (!timeline.ProcessPacket(
                pendingPackets.Front(),
                true,
                std::nullopt,
                error)) {
            return false;
        }
        pendingPackets.PopFront();
    }
    return true;
}

}  // namespace

std::optional<SystemAudioQpcPosition>
QuerySystemAudioQpcPosition100Nanoseconds() noexcept {
    return QueryPerformanceCounter100Nanoseconds();
}

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
    std::optional<SystemAudioQpcPosition> requestedTransitionQpc;
    std::optional<std::chrono::nanoseconds> requestedTransitionDuration;
    std::optional<SystemAudioQpcPosition> requestedStopQpc;
    std::optional<std::chrono::nanoseconds> requestedExactDuration;
    std::uint64_t commandGeneration{};
    std::uint64_t appliedCommandGeneration{};
    std::optional<SystemAudioRecordingResult> completedResult;
    SystemAudioCaptureError terminalError{};

    struct ControlSnapshot final {
        bool stop{};
        bool paused{};
        std::optional<SystemAudioQpcPosition> boundaryQpc;
        std::optional<std::chrono::nanoseconds> exactTimelineDuration;
        std::uint64_t generation{};
    };

    [[nodiscard]] ControlSnapshot ReadControl() const {
        std::scoped_lock lock(stateMutex);
        return ControlSnapshot{
            stopRequested,
            requestedPaused,
            stopRequested ? requestedStopQpc : requestedTransitionQpc,
            stopRequested ? requestedExactDuration : requestedTransitionDuration,
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
        const std::optional<SystemAudioQpcPosition> timelineStartQpc,
        const bool prepared,
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

        const std::optional<SystemAudioQpcPosition> initialSegmentQpcStart =
            prepared
                ? std::nullopt
                : timelineStartQpc.has_value()
                    ? timelineStartQpc
                    : QueryPerformanceCounter100Nanoseconds();
        const Clock::time_point initialSegmentStarted = Clock::now();
        const std::uint32_t bytesPerFrame =
            static_cast<std::uint32_t>(config.channelCount) *
            kFloatBitsPerSample / kBitsPerByte;
        AudioTimelineWriter timeline(
            writer,
            config.sampleRate,
            bytesPerFrame);
        if (!prepared) {
            timeline.BeginActiveSegment(initialSegmentQpcStart, 0);
        }

        {
            std::scoped_lock lock(stateMutex);
            state = prepared ? SystemAudioCaptureState::Paused
                             : SystemAudioCaptureState::Capturing;
            starting = false;
            appliedCommandGeneration = commandGeneration;
        }
        startupResult.set_value(true);
        stateChanged.notify_all();

        SystemAudioCaptureError runtimeError{};
        bool capturePaused = prepared;
        PendingPacketQueue pendingPackets;
        // This clock advances only while recording is active. Incremental
        // silence keeps a zero-packet recording encoded before Stop is called.
        std::chrono::nanoseconds completedActiveDuration{};
        Clock::time_point activeSegmentStarted = initialSegmentStarted;
        const auto activeDurationAt = [&](const Clock::time_point now) {
            if (capturePaused) {
                return completedActiveDuration;
            }
            return completedActiveDuration +
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    now - activeSegmentStarted);
        };
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
                if (control.paused && !capturePaused) {
                    const Clock::time_point commandTime = Clock::now();
                    completedActiveDuration =
                        control.exactTimelineDuration.value_or(
                            activeDurationAt(commandTime));
                    const std::optional<std::uint64_t> exactTargetFrames =
                        control.exactTimelineDuration.has_value()
                            ? std::optional<std::uint64_t>(
                                  FramesForDurationCeiling(
                                      *control.exactTimelineDuration,
                                      config.sampleRate))
                            : std::nullopt;
                    if (!CollectAvailablePackets(
                            session,
                            pendingPackets,
                            runtimeError) ||
                        !FlushPendingPackets(
                            pendingPackets,
                            timeline,
                            config.sampleRate,
                            bytesPerFrame,
                            true,
                            std::nullopt,
                            control.boundaryQpc,
                            runtimeError,
                            exactTargetFrames)) {
                        break;
                    }
                    const bool segmentEnded =
                        control.exactTimelineDuration.has_value()
                            ? timeline.EnsureExactDuration(
                                  *control.exactTimelineDuration,
                                  runtimeError)
                            : control.boundaryQpc.has_value()
                                ? timeline.EndActiveSegment(
                                      control.boundaryQpc,
                                      runtimeError)
                                : timeline.EnsureMinimumDuration(
                                      completedActiveDuration,
                                      runtimeError);
                    if (!segmentEnded) {
                        break;
                    }
                    capturePaused = true;
                } else if (!control.paused && capturePaused) {
                    const Clock::time_point commandTime = Clock::now();
                    activeSegmentStarted = commandTime;
                    capturePaused = false;
                    timeline.BeginActiveSegment(
                        control.boundaryQpc,
                        timeline.Stats().encodedFrames);
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

            if (capturePaused) {
                if (!CollectAvailablePackets(
                        session,
                        pendingPackets,
                        runtimeError) ||
                    !TrimPausedPreRollToBudget(
                        pendingPackets,
                        timeline,
                        config.sampleRate,
                        bytesPerFrame,
                        runtimeError)) {
                    break;
                }
            } else {
                if (!CollectAvailablePackets(
                        session,
                        pendingPackets,
                        runtimeError)) {
                    break;
                }
                if (!capturePaused &&
                    !FlushPendingPackets(
                        pendingPackets,
                        timeline,
                        config.sampleRate,
                        bytesPerFrame,
                        false,
                        MaturePacketBoundaryQpc(),
                        std::nullopt,
                        runtimeError)) {
                    break;
                }
            }

            const Clock::time_point now = Clock::now();
            if (!capturePaused &&
                !timeline.CatchUpSilence(
                    activeDurationAt(now),
                    runtimeError)) {
                break;
            }
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

        nativeResult = session.Stop();
        if (FAILED(nativeResult) && !runtimeError) {
            runtimeError = MakeWasapiError(
                SystemAudioCaptureErrorCode::CaptureFailed,
                L"停止系统回环音频流失败。",
                nativeResult);
        }

        std::optional<SystemAudioQpcPosition> stopBoundaryQpc;
        std::optional<std::chrono::nanoseconds> exactDuration;
        {
            std::scoped_lock lock(stateMutex);
            stopBoundaryQpc = requestedStopQpc;
            exactDuration = requestedExactDuration;
        }
        if (!runtimeError) {
            if (capturePaused) {
                pendingPackets.Clear();
                static_cast<void>(DrainAvailablePackets(
                    session,
                    timeline,
                    true,
                    runtimeError));
            } else if (CollectAvailablePackets(
                           session,
                           pendingPackets,
                           runtimeError)) {
                static_cast<void>(FlushPendingPackets(
                    pendingPackets,
                    timeline,
                    config.sampleRate,
                    bytesPerFrame,
                    true,
                    std::nullopt,
                    stopBoundaryQpc,
                    runtimeError,
                    exactDuration.has_value()
                        ? std::optional<std::uint64_t>(
                              FramesForDurationCeiling(
                                  *exactDuration,
                                  config.sampleRate))
                        : std::nullopt));
            }
        }
        if (!runtimeError) {
            const std::chrono::nanoseconds targetDuration =
                exactDuration.value_or(activeDurationAt(Clock::now()));
            static_cast<void>(timeline.EnsureExactDuration(
                targetDuration,
                runtimeError));
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
        const std::uint64_t minimumUsableAacFrames =
            static_cast<std::uint64_t>(config.sampleRate) *
            kMinimumAacSeedMilliseconds / 1'000U;
        if (!runtimeError &&
            finalStats.encodedFrames < minimumUsableAacFrames) {
            runtimeError = MakeError(
                SystemAudioCaptureErrorCode::FinalizeFailed,
                L"录制时间过短，系统音轨不足以生成可用的 AAC 文件。",
                static_cast<long>(E_FAIL));
        }
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
            requestedTransitionQpc.reset();
            requestedTransitionDuration.reset();
            requestedStopQpc.reset();
            requestedExactDuration.reset();
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
    return StartInternal(
        config,
        std::nullopt,
        false,
        std::move(callbacks),
        error);
}

bool SystemAudioCapture::Start(
    const SystemAudioCaptureConfig& config,
    const std::optional<SystemAudioQpcPosition> timelineStartQpc,
    SystemAudioCaptureCallbacks callbacks,
    SystemAudioCaptureError* error) {
    return StartInternal(
        config,
        timelineStartQpc,
        false,
        std::move(callbacks),
        error);
}

bool SystemAudioCapture::StartPrepared(
    const SystemAudioCaptureConfig& config,
    SystemAudioCaptureCallbacks callbacks,
    SystemAudioCaptureError* error) {
    return StartInternal(
        config,
        std::nullopt,
        true,
        std::move(callbacks),
        error);
}

bool SystemAudioCapture::StartInternal(
    const SystemAudioCaptureConfig& config,
    const std::optional<SystemAudioQpcPosition> timelineStartQpc,
    const bool prepared,
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
        impl_->requestedPaused = prepared;
        impl_->requestedTransitionQpc.reset();
        impl_->requestedTransitionDuration.reset();
        impl_->requestedStopQpc.reset();
        impl_->requestedExactDuration.reset();
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
             timelineStartQpc,
             prepared,
             callbacks = std::move(callbacks),
             promise = std::move(startupPromise)]() mutable {
                implementation->Run(
                    config,
                    timelineStartQpc,
                    prepared,
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
    return PauseInternal(
        QuerySystemAudioQpcPosition100Nanoseconds(),
        std::nullopt,
        error);
}

bool SystemAudioCapture::Pause(
    const std::optional<SystemAudioQpcPosition> boundaryQpc,
    SystemAudioCaptureError* error) {
    return PauseInternal(boundaryQpc, std::nullopt, error);
}

bool SystemAudioCapture::Pause(
    const std::optional<SystemAudioQpcPosition> boundaryQpc,
    const std::chrono::nanoseconds exactTimelineDuration,
    SystemAudioCaptureError* error) {
    return PauseInternal(
        boundaryQpc,
        std::max(exactTimelineDuration, std::chrono::nanoseconds::zero()),
        error);
}

bool SystemAudioCapture::PauseInternal(
    const std::optional<SystemAudioQpcPosition> boundaryQpc,
    const std::optional<std::chrono::nanoseconds> exactTimelineDuration,
    SystemAudioCaptureError* error) {
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

    impl_->requestedTransitionQpc = boundaryQpc;
    impl_->requestedTransitionDuration = exactTimelineDuration;
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
    return Resume(QuerySystemAudioQpcPosition100Nanoseconds(), error);
}

bool SystemAudioCapture::Resume(
    const std::optional<SystemAudioQpcPosition> boundaryQpc,
    SystemAudioCaptureError* error) {
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

    impl_->requestedTransitionQpc = boundaryQpc;
    impl_->requestedTransitionDuration.reset();
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
    return StopInternal(
        QuerySystemAudioQpcPosition100Nanoseconds(),
        std::nullopt,
        error);
}

std::optional<SystemAudioRecordingResult> SystemAudioCapture::Stop(
    const std::chrono::nanoseconds minimumDuration,
    SystemAudioCaptureError* error) {
    return StopInternal(
        QuerySystemAudioQpcPosition100Nanoseconds(),
        std::max(minimumDuration, std::chrono::nanoseconds::zero()),
        error);
}

std::optional<SystemAudioRecordingResult> SystemAudioCapture::Stop(
    const std::optional<SystemAudioQpcPosition> boundaryQpc,
    const std::chrono::nanoseconds exactDuration,
    SystemAudioCaptureError* error) {
    return StopInternal(
        boundaryQpc,
        std::max(exactDuration, std::chrono::nanoseconds::zero()),
        error);
}

std::optional<SystemAudioRecordingResult> SystemAudioCapture::StopInternal(
    const std::optional<SystemAudioQpcPosition> boundaryQpc,
    const std::optional<std::chrono::nanoseconds> exactDuration,
    SystemAudioCaptureError* error) {
    std::scoped_lock lifecycleLock(impl_->lifecycleMutex);
    ClearError(error);

    {
        std::scoped_lock lock(impl_->stateMutex);
        if (impl_->state != SystemAudioCaptureState::Idle || impl_->starting) {
            impl_->requestedStopQpc = boundaryQpc;
            impl_->requestedExactDuration = exactDuration;
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
