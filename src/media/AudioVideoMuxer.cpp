#include "media/AudioVideoMuxer.h"

#include "common/Win32Helpers.h"
#include "media/CompressedTimelineAlignment.h"

#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <propvarutil.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <exception>
#include <limits>
#include <string_view>
#include <system_error>
#include <utility>

#pragma comment(lib, "mf.lib")
#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfreadwrite.lib")
#pragma comment(lib, "mfuuid.lib")
#pragma comment(lib, "ole32.lib")

namespace qrec {
namespace {

using Microsoft::WRL::ComPtr;

constexpr LONGLONG kTicksPerMillisecond = 10'000;
constexpr LONGLONG kTicksPerSecond = 10'000'000;
constexpr LONGLONG kNanosecondsPerTick = 100;
constexpr LONGLONG kTimestampToleranceTicks = 2;
constexpr LONGLONG kAudioBoundaryProbePrerollTicks = 10'000'000;
constexpr DWORD kAllStreams =
    static_cast<DWORD>(MF_SOURCE_READER_ALL_STREAMS);
constexpr DWORD kVideoStream =
    static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM);
constexpr DWORD kAudioStream =
    static_cast<DWORD>(MF_SOURCE_READER_FIRST_AUDIO_STREAM);
constexpr DWORD kMediaSource =
    static_cast<DWORD>(MF_SOURCE_READER_MEDIASOURCE);

class MediaFoundationSession final {
public:
    MediaFoundationSession() noexcept
        : result_(::MFStartup(MF_VERSION, MFSTARTUP_FULL)) {}

    ~MediaFoundationSession() {
        if (SUCCEEDED(result_)) {
            ::MFShutdown();
        }
    }

    MediaFoundationSession(const MediaFoundationSession&) = delete;
    MediaFoundationSession& operator=(const MediaFoundationSession&) = delete;

    [[nodiscard]] HRESULT Result() const noexcept { return result_; }

private:
    HRESULT result_{};
};

class DestinationCleanup final {
public:
    explicit DestinationCleanup(std::filesystem::path path)
        : path_(std::move(path)) {}

    ~DestinationCleanup() noexcept {
        if (!active_) {
            return;
        }
        std::error_code ignored;
        std::filesystem::remove(path_, ignored);
    }

    DestinationCleanup(const DestinationCleanup&) = delete;
    DestinationCleanup& operator=(const DestinationCleanup&) = delete;

    void Activate() noexcept { active_ = true; }
    void Release() noexcept { active_ = false; }

private:
    std::filesystem::path path_;
    bool active_{};
};

struct StepResult final {
    AudioVideoMuxOutcome outcome{AudioVideoMuxOutcome::Succeeded};
    HRESULT nativeError{S_OK};
    std::wstring errorMessage;

    [[nodiscard]] bool Succeeded() const noexcept {
        return outcome == AudioVideoMuxOutcome::Succeeded;
    }
};

struct SourceTrack final {
    ComPtr<IMFSourceReader> reader;
    ComPtr<IMFMediaType> nativeType;
};

struct PendingSample final {
    ComPtr<IMFSample> sample;
    LONGLONG outputTime{};
    LONGLONG duration{};

    void Reset() noexcept {
        sample.Reset();
        outputTime = 0;
        duration = 0;
    }
};

struct VideoReadState final {
    IMFSourceReader* reader{};
    LONGLONG outputDuration{};
    LONGLONG requestedDuration{};
    LONGLONG nominalFrameDuration{};
    LONGLONG maximumSourceSampleDuration{};
    LONGLONG previousSourceTime{-1};
    LONGLONG previousSourceEnd{-1};
    LONGLONG lastOutputEnd{};
    PendingSample pending;
    bool firstSample{true};
    bool endOfStream{};
};

struct AudioReadState final {
    IMFSourceReader* reader{};
    LONGLONG trimStart{};
    LONGLONG trimEnd{};
    LONGLONG previousSourceTime{-1};
    LONGLONG previousSourceEnd{-1};
    LONGLONG lastOutputEnd{};
    PendingSample pending;
    bool firstAcceptedSample{true};
    bool pendingDiscontinuity{};
    bool endOfStream{};
    bool droppedLeadingBoundaryAccessUnit{};
    bool droppedTrailingBoundaryAccessUnit{};
};

[[nodiscard]] StepResult MakeStep(
    const AudioVideoMuxOutcome outcome,
    const HRESULT nativeError,
    std::wstring errorMessage = {}) {
    StepResult result{};
    result.outcome = outcome;
    result.nativeError = nativeError;
    result.errorMessage = std::move(errorMessage);
    return result;
}

[[nodiscard]] AudioVideoMuxResult MakeResult(
    const AudioVideoMuxOutcome outcome,
    const HRESULT nativeError,
    std::wstring errorMessage = {}) {
    AudioVideoMuxResult result{};
    result.outcome = outcome;
    result.nativeError = nativeError;
    result.errorMessage = std::move(errorMessage);
    return result;
}

[[nodiscard]] std::wstring ErrorText(
    const std::wstring_view operation,
    const HRESULT result) {
    return std::wstring(operation) + L"：" + win32::FormatError(result);
}

[[nodiscard]] bool ToTicks(
    const std::chrono::milliseconds value,
    LONGLONG* output) noexcept {
    constexpr LONGLONG kMaximumTicksForNanoseconds =
        std::numeric_limits<LONGLONG>::max() / kNanosecondsPerTick;
    if (output == nullptr || value.count() < 0 ||
        value.count() > kMaximumTicksForNanoseconds /
            kTicksPerMillisecond) {
        return false;
    }
    *output = value.count() * kTicksPerMillisecond;
    return true;
}

[[nodiscard]] std::chrono::nanoseconds TicksToNanoseconds(
    const LONGLONG value) noexcept {
    return std::chrono::nanoseconds(
        std::max<LONGLONG>(0, value) * kNanosecondsPerTick);
}

[[nodiscard]] StepResult ReadPresentationDuration(
    IMFSourceReader* reader,
    LONGLONG* output) {
    if (reader == nullptr || output == nullptr) {
        return MakeStep(
            AudioVideoMuxOutcome::Failed,
            E_POINTER,
            L"读取压缩视频时长时收到空指针。");
    }
    *output = 0;

    PROPVARIANT duration{};
    ::PropVariantInit(&duration);
    const HRESULT readResult = reader->GetPresentationAttribute(
        kMediaSource,
        MF_PD_DURATION,
        &duration);
    if (FAILED(readResult)) {
        ::PropVariantClear(&duration);
        return MakeStep(
            AudioVideoMuxOutcome::Unsupported,
            readResult,
            ErrorText(L"读取 H.264 轨道实际时长失败", readResult));
    }

    bool valid = false;
    if (duration.vt == VT_UI8 &&
        duration.uhVal.QuadPart <= static_cast<ULONGLONG>(
            std::numeric_limits<LONGLONG>::max())) {
        *output = static_cast<LONGLONG>(duration.uhVal.QuadPart);
        valid = *output > 0;
    } else if (duration.vt == VT_I8) {
        *output = duration.hVal.QuadPart;
        valid = *output > 0;
    }
    ::PropVariantClear(&duration);
    if (!valid) {
        *output = 0;
        return MakeStep(
            AudioVideoMuxOutcome::Unsupported,
            MF_E_INVALID_TIMESTAMP,
            L"H.264 轨道缺少有效的实际时长。");
    }
    return MakeStep(AudioVideoMuxOutcome::Succeeded, S_OK);
}

[[nodiscard]] LONGLONG NominalVideoFrameDuration(
    IMFMediaType* mediaType) noexcept {
    if (mediaType == nullptr) {
        return 0;
    }
    UINT32 numerator = 0;
    UINT32 denominator = 0;
    if (FAILED(::MFGetAttributeRatio(
            mediaType,
            MF_MT_FRAME_RATE,
            &numerator,
            &denominator)) ||
        numerator == 0 || denominator == 0 ||
        denominator > static_cast<UINT32>(
            std::numeric_limits<LONGLONG>::max() / kTicksPerSecond)) {
        return 0;
    }
    const LONGLONG scaled = kTicksPerSecond * denominator;
    return std::max<LONGLONG>(
        1,
        (scaled + static_cast<LONGLONG>(numerator) / 2) / numerator);
}

[[nodiscard]] HRESULT SeekReader(
    IMFSourceReader* reader,
    const LONGLONG position) noexcept {
    if (reader == nullptr || position < 0) {
        return E_INVALIDARG;
    }
    PROPVARIANT value{};
    ::PropVariantInit(&value);
    value.vt = VT_I8;
    value.hVal.QuadPart = position;
    const HRESULT result = reader->SetCurrentPosition(GUID_NULL, value);
    ::PropVariantClear(&value);
    return result;
}

[[nodiscard]] StepResult FindNativeType(
    IMFSourceReader* reader,
    const DWORD stream,
    const GUID& expectedMajorType,
    const GUID& expectedSubtype,
    IMFMediaType** output) {
    if (reader == nullptr || output == nullptr) {
        return MakeStep(
            AudioVideoMuxOutcome::Failed,
            E_POINTER,
            L"查找压缩媒体类型时收到空指针。");
    }
    *output = nullptr;
    for (DWORD typeIndex = 0;; ++typeIndex) {
        ComPtr<IMFMediaType> candidate;
        const HRESULT result = reader->GetNativeMediaType(
            stream,
            typeIndex,
            &candidate);
        if (result == MF_E_NO_MORE_TYPES) {
            return MakeStep(
                AudioVideoMuxOutcome::Unsupported,
                MF_E_INVALIDMEDIATYPE,
                expectedMajorType == MFMediaType_Video
                    ? L"视频源不包含可直接重封装的 H.264 轨道。"
                    : L"音频旁路不包含可直接重封装的 AAC 轨道。");
        }
        if (FAILED(result)) {
            return MakeStep(
                AudioVideoMuxOutcome::Failed,
                result,
                ErrorText(L"枚举原生压缩媒体类型失败", result));
        }
        GUID majorType{};
        GUID subtype{};
        if (SUCCEEDED(candidate->GetGUID(MF_MT_MAJOR_TYPE, &majorType)) &&
            SUCCEEDED(candidate->GetGUID(MF_MT_SUBTYPE, &subtype)) &&
            majorType == expectedMajorType && subtype == expectedSubtype) {
            *output = candidate.Detach();
            return MakeStep(AudioVideoMuxOutcome::Succeeded, S_OK);
        }
    }
}

[[nodiscard]] StepResult OpenCompressedTrack(
    const std::filesystem::path& path,
    const DWORD stream,
    const GUID& majorType,
    const GUID& subtype,
    SourceTrack* output) {
    if (output == nullptr) {
        return MakeStep(
            AudioVideoMuxOutcome::Failed,
            E_POINTER,
            L"打开压缩轨道时收到空输出指针。");
    }

    ComPtr<IMFAttributes> attributes;
    HRESULT result = ::MFCreateAttributes(&attributes, 3);
    if (SUCCEEDED(result)) {
        result = attributes->SetUINT32(
            MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING,
            FALSE);
    }
    if (SUCCEEDED(result)) {
        result = attributes->SetUINT32(
            MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS,
            TRUE);
    }
    if (SUCCEEDED(result)) {
        result = ::MFCreateSourceReaderFromURL(
            path.c_str(),
            attributes.Get(),
            &output->reader);
    }
    if (FAILED(result)) {
        return MakeStep(
            AudioVideoMuxOutcome::Failed,
            result,
            ErrorText(L"打开压缩媒体源失败", result));
    }

    static_cast<void>(output->reader->SetStreamSelection(kAllStreams, FALSE));
    result = output->reader->SetStreamSelection(stream, TRUE);
    if (FAILED(result)) {
        return MakeStep(
            AudioVideoMuxOutcome::Failed,
            result,
            ErrorText(L"选择压缩媒体轨道失败", result));
    }
    StepResult step = FindNativeType(
        output->reader.Get(),
        stream,
        majorType,
        subtype,
        &output->nativeType);
    if (!step.Succeeded()) {
        return step;
    }
    result = output->reader->SetCurrentMediaType(
        stream,
        nullptr,
        output->nativeType.Get());
    if (FAILED(result)) {
        return MakeStep(
            AudioVideoMuxOutcome::Failed,
            result,
            ErrorText(L"启用原生压缩媒体类型失败", result));
    }
    return MakeStep(AudioVideoMuxOutcome::Succeeded, S_OK);
}

[[nodiscard]] bool IsCleanPoint(IMFSample* sample) noexcept {
    if (sample == nullptr) {
        return false;
    }
    UINT32 cleanPoint = FALSE;
    return SUCCEEDED(sample->GetUINT32(
               MFSampleExtension_CleanPoint,
               &cleanPoint)) &&
        cleanPoint != FALSE;
}

[[nodiscard]] StepResult ValidateAndRemoveVideoDecodeTimestamp(
    IMFSample* sample,
    const LONGLONG presentationTime) {
    UINT64 decodeTimestamp = 0;
    const HRESULT readResult = sample->GetUINT64(
        MFSampleExtension_DecodeTimestamp,
        &decodeTimestamp);
    if (readResult == MF_E_ATTRIBUTENOTFOUND) {
        return MakeStep(AudioVideoMuxOutcome::Succeeded, S_OK);
    }
    if (FAILED(readResult)) {
        return MakeStep(
            AudioVideoMuxOutcome::Failed,
            readResult,
            ErrorText(L"读取 H.264 解码时间戳失败", readResult));
    }
    if (decodeTimestamp > static_cast<UINT64>(
            std::numeric_limits<LONGLONG>::max()) ||
        static_cast<LONGLONG>(decodeTimestamp) != presentationTime) {
        return MakeStep(
            AudioVideoMuxOutcome::Unsupported,
            MF_E_INVALID_TIMESTAMP,
            L"待合并视频包含 B 帧或重排序时间戳，无法安全直通。");
    }
    const HRESULT deleteResult = sample->DeleteItem(
        MFSampleExtension_DecodeTimestamp);
    if (FAILED(deleteResult)) {
        return MakeStep(
            AudioVideoMuxOutcome::Failed,
            deleteResult,
            ErrorText(L"清理 H.264 直通解码时间戳失败", deleteResult));
    }
    return MakeStep(AudioVideoMuxOutcome::Succeeded, S_OK);
}

// AAC does not inherit the H.264 no-B-frame gate. If an audio source exposes
// a decode timestamp, preserve its offset while moving it onto the trimmed
// timeline. A pre-roll DTS that cannot be represented as UINT64 is omitted;
// presentation ordering remains the authoritative AAC ordering.
[[nodiscard]] StepResult RebaseOptionalAudioDecodeTimestamp(
    IMFSample* sample,
    const LONGLONG origin) {
    UINT64 decodeTimestamp = 0;
    const HRESULT readResult = sample->GetUINT64(
        MFSampleExtension_DecodeTimestamp,
        &decodeTimestamp);
    if (readResult == MF_E_ATTRIBUTENOTFOUND) {
        return MakeStep(AudioVideoMuxOutcome::Succeeded, S_OK);
    }
    if (FAILED(readResult)) {
        return MakeStep(
            AudioVideoMuxOutcome::Failed,
            readResult,
            ErrorText(L"读取 AAC 解码时间戳失败", readResult));
    }
    if (decodeTimestamp > static_cast<UINT64>(
            std::numeric_limits<LONGLONG>::max())) {
        return MakeStep(
            AudioVideoMuxOutcome::Unsupported,
            MF_E_INVALID_TIMESTAMP,
            L"AAC 解码时间戳超出可重封装范围。");
    }
    const LONGLONG signedTimestamp =
        static_cast<LONGLONG>(decodeTimestamp);
    const HRESULT writeResult = signedTimestamp < origin
        ? sample->DeleteItem(MFSampleExtension_DecodeTimestamp)
        : sample->SetUINT64(
              MFSampleExtension_DecodeTimestamp,
              static_cast<UINT64>(signedTimestamp - origin));
    if (FAILED(writeResult)) {
        return MakeStep(
            AudioVideoMuxOutcome::Failed,
            writeResult,
            ErrorText(L"重基准 AAC 解码时间戳失败", writeResult));
    }
    return MakeStep(AudioVideoMuxOutcome::Succeeded, S_OK);
}

[[nodiscard]] StepResult ValidateSampleTimeline(
    const LONGLONG sampleTime,
    const LONGLONG sampleDuration,
    LONGLONG* previousTime,
    LONGLONG* previousEnd,
    const std::wstring_view trackName) {
    if (previousTime == nullptr || previousEnd == nullptr ||
        sampleTime < 0 || sampleDuration <= 0 ||
        sampleTime > std::numeric_limits<LONGLONG>::max() - sampleDuration) {
        return MakeStep(
            AudioVideoMuxOutcome::Unsupported,
            MF_E_INVALID_TIMESTAMP,
            std::wstring(trackName) + L"样本时间戳或时长无效。");
    }
    if (*previousTime >= 0 &&
        (sampleTime <= *previousTime || sampleTime < *previousEnd)) {
        return MakeStep(
            AudioVideoMuxOutcome::Unsupported,
            MF_E_INVALID_TIMESTAMP,
            std::wstring(trackName) + L"样本时间戳不单调或发生重叠。");
    }
    *previousTime = sampleTime;
    *previousEnd = sampleTime + sampleDuration;
    return MakeStep(AudioVideoMuxOutcome::Succeeded, S_OK);
}

[[nodiscard]] bool CoversExpectedVideoEnd(
    const VideoReadState& state) noexcept {
    if (state.firstSample || state.lastOutputEnd <= 0) {
        return false;
    }
    const LONGLONG uncoveredDuration = std::max<LONGLONG>(
        0,
        state.outputDuration - state.lastOutputEnd);
    // outputDuration is the measured duration of the already-trimmed video.
    // Only the MP4 timescale round trip is tolerated here; frame quantization
    // is validated separately against requestedDuration.
    return uncoveredDuration <= kTimestampToleranceTicks;
}

[[nodiscard]] StepResult ReadNextVideoSample(
    VideoReadState* state,
    const std::stop_token stopToken) {
    if (state == nullptr || state->reader == nullptr ||
        state->outputDuration <= 0 || state->pending.sample != nullptr) {
        return MakeStep(
            AudioVideoMuxOutcome::Failed,
            E_INVALIDARG,
            L"读取 H.264 直通样本的状态无效。");
    }
    if (state->endOfStream) {
        return MakeStep(AudioVideoMuxOutcome::Succeeded, S_OK);
    }

    for (;;) {
        if (stopToken.stop_requested()) {
            return MakeStep(
                AudioVideoMuxOutcome::Cancelled,
                HRESULT_FROM_WIN32(ERROR_CANCELLED));
        }
        DWORD actualStream = 0;
        DWORD flags = 0;
        LONGLONG sampleTime = 0;
        ComPtr<IMFSample> sample;
        HRESULT result = state->reader->ReadSample(
            kVideoStream,
            0,
            &actualStream,
            &flags,
            &sampleTime,
            &sample);
        static_cast<void>(actualStream);
        if (FAILED(result) || (flags & MF_SOURCE_READERF_ERROR) != 0) {
            const HRESULT readError = FAILED(result) ? result : E_FAIL;
            return MakeStep(
                AudioVideoMuxOutcome::Failed,
                readError,
                ErrorText(L"读取 H.264 压缩样本失败", readError));
        }
        if ((flags & (MF_SOURCE_READERF_CURRENTMEDIATYPECHANGED |
                      MF_SOURCE_READERF_NATIVEMEDIATYPECHANGED)) != 0) {
            return MakeStep(
                AudioVideoMuxOutcome::Unsupported,
                MF_E_INVALIDMEDIATYPE,
                L"H.264 轨道在合并期间改变了媒体类型。");
        }
        if ((flags & MF_SOURCE_READERF_STREAMTICK) != 0) {
            return MakeStep(
                AudioVideoMuxOutcome::Unsupported,
                MF_E_INVALID_TIMESTAMP,
                L"H.264 轨道包含无法直通的 stream tick。");
        }
        if ((flags & MF_SOURCE_READERF_ENDOFSTREAM) != 0 ||
            sampleTime >= state->outputDuration) {
            state->endOfStream = true;
            if (!CoversExpectedVideoEnd(*state)) {
                return MakeStep(
                    AudioVideoMuxOutcome::Unsupported,
                    MF_E_END_OF_STREAM,
                    L"H.264 轨道没有完整覆盖目标时间线。");
            }
            return MakeStep(AudioVideoMuxOutcome::Succeeded, S_OK);
        }
        if (sample == nullptr) {
            continue;
        }

        LONGLONG sampleDuration = 0;
        result = sample->GetSampleDuration(&sampleDuration);
        if (FAILED(result) || sampleDuration <= 0) {
            return MakeStep(
                AudioVideoMuxOutcome::Unsupported,
                MF_E_INVALID_TIMESTAMP,
                L"H.264 压缩样本缺少有效时长。");
        }
        state->maximumSourceSampleDuration = std::max(
            state->maximumSourceSampleDuration,
            sampleDuration);
        StepResult step = ValidateSampleTimeline(
            sampleTime,
            sampleDuration,
            &state->previousSourceTime,
            &state->previousSourceEnd,
            L"H.264 ");
        if (!step.Succeeded()) {
            return step;
        }
        if (state->firstSample &&
            (sampleTime != 0 || !IsCleanPoint(sample.Get()))) {
            return MakeStep(
                AudioVideoMuxOutcome::Unsupported,
                MF_E_INVALID_TIMESTAMP,
                L"待合并视频必须从时间零的 H.264 CleanPoint 开始。");
        }
        step = ValidateAndRemoveVideoDecodeTimestamp(
            sample.Get(),
            sampleTime);
        if (!step.Succeeded()) {
            return step;
        }

        const LONGLONG remaining = state->outputDuration - sampleTime;
        sampleDuration = std::min(sampleDuration, remaining);
        if (sampleDuration <= 0) {
            state->endOfStream = true;
            return MakeStep(AudioVideoMuxOutcome::Succeeded, S_OK);
        }
        result = sample->SetSampleTime(sampleTime);
        if (SUCCEEDED(result)) {
            result = sample->SetSampleDuration(sampleDuration);
        }
        if (SUCCEEDED(result) && state->firstSample) {
            result = sample->SetUINT32(
                MFSampleExtension_Discontinuity,
                TRUE);
        }
        if (FAILED(result)) {
            return MakeStep(
                AudioVideoMuxOutcome::Failed,
                result,
                ErrorText(L"规范化 H.264 直通样本失败", result));
        }
        state->firstSample = false;
        state->lastOutputEnd = sampleTime + sampleDuration;
        state->pending.sample = std::move(sample);
        state->pending.outputTime = sampleTime;
        state->pending.duration = sampleDuration;
        return MakeStep(AudioVideoMuxOutcome::Succeeded, S_OK);
    }
}

[[nodiscard]] StepResult ReadNextAudioSample(
    AudioReadState* state,
    const std::stop_token stopToken) {
    if (state == nullptr || state->reader == nullptr ||
        state->trimEnd <= state->trimStart ||
        state->pending.sample != nullptr) {
        return MakeStep(
            AudioVideoMuxOutcome::Failed,
            E_INVALIDARG,
            L"读取 AAC 直通样本的状态无效。");
    }
    if (state->endOfStream) {
        return MakeStep(AudioVideoMuxOutcome::Succeeded, S_OK);
    }

    for (;;) {
        if (stopToken.stop_requested()) {
            return MakeStep(
                AudioVideoMuxOutcome::Cancelled,
                HRESULT_FROM_WIN32(ERROR_CANCELLED));
        }
        DWORD actualStream = 0;
        DWORD flags = 0;
        LONGLONG sampleTime = 0;
        ComPtr<IMFSample> sample;
        HRESULT result = state->reader->ReadSample(
            kAudioStream,
            0,
            &actualStream,
            &flags,
            &sampleTime,
            &sample);
        static_cast<void>(actualStream);
        if (FAILED(result) || (flags & MF_SOURCE_READERF_ERROR) != 0) {
            const HRESULT readError = FAILED(result) ? result : E_FAIL;
            return MakeStep(
                AudioVideoMuxOutcome::Failed,
                readError,
                ErrorText(L"读取 AAC 压缩样本失败", readError));
        }
        if ((flags & (MF_SOURCE_READERF_CURRENTMEDIATYPECHANGED |
                      MF_SOURCE_READERF_NATIVEMEDIATYPECHANGED)) != 0) {
            return MakeStep(
                AudioVideoMuxOutcome::Unsupported,
                MF_E_INVALIDMEDIATYPE,
                L"AAC 轨道在合并期间改变了媒体类型。");
        }
        if ((flags & MF_SOURCE_READERF_STREAMTICK) != 0) {
            state->pendingDiscontinuity = true;
        }
        if ((flags & MF_SOURCE_READERF_ENDOFSTREAM) != 0) {
            state->endOfStream = true;
            return MakeStep(AudioVideoMuxOutcome::Succeeded, S_OK);
        }
        if (sample == nullptr) {
            continue;
        }

        LONGLONG sampleDuration = 0;
        result = sample->GetSampleDuration(&sampleDuration);
        if (FAILED(result) || sampleDuration <= 0) {
            return MakeStep(
                AudioVideoMuxOutcome::Unsupported,
                MF_E_INVALID_TIMESTAMP,
                L"AAC access unit 缺少有效时长。");
        }
        StepResult step = ValidateSampleTimeline(
            sampleTime,
            sampleDuration,
            &state->previousSourceTime,
            &state->previousSourceEnd,
            L"AAC ");
        if (!step.Succeeded()) {
            return step;
        }
        const LONGLONG sampleEnd = sampleTime + sampleDuration;
        if (sampleTime < state->trimStart) {
            if (sampleEnd > state->trimStart) {
                state->droppedLeadingBoundaryAccessUnit = true;
            }
            continue;
        }
        if (sampleTime >= state->trimEnd) {
            state->endOfStream = true;
            return MakeStep(AudioVideoMuxOutcome::Succeeded, S_OK);
        }
        if (sampleEnd > state->trimEnd + kTimestampToleranceTicks) {
            state->droppedTrailingBoundaryAccessUnit = true;
            state->endOfStream = true;
            return MakeStep(AudioVideoMuxOutcome::Succeeded, S_OK);
        }

        sampleDuration = std::min(
            sampleDuration,
            state->trimEnd - sampleTime);
        const LONGLONG outputTime = sampleTime - state->trimStart;
        step = RebaseOptionalAudioDecodeTimestamp(
            sample.Get(),
            state->trimStart);
        if (!step.Succeeded()) {
            return step;
        }
        result = sample->SetSampleTime(outputTime);
        if (SUCCEEDED(result)) {
            result = sample->SetSampleDuration(sampleDuration);
        }
        if (SUCCEEDED(result) &&
            (state->firstAcceptedSample || state->pendingDiscontinuity)) {
            result = sample->SetUINT32(
                MFSampleExtension_Discontinuity,
                TRUE);
        }
        if (FAILED(result)) {
            return MakeStep(
                AudioVideoMuxOutcome::Failed,
                result,
                ErrorText(L"重基准 AAC 直通样本失败", result));
        }
        state->firstAcceptedSample = false;
        state->pendingDiscontinuity = false;
        state->lastOutputEnd = outputTime + sampleDuration;
        state->pending.sample = std::move(sample);
        state->pending.outputTime = outputTime;
        state->pending.duration = sampleDuration;
        return MakeStep(AudioVideoMuxOutcome::Succeeded, S_OK);
    }
}

[[nodiscard]] StepResult CreateMuxWriter(
    const std::filesystem::path& destinationPath,
    IMFMediaType* videoType,
    IMFMediaType* audioType,
    ComPtr<IMFSinkWriter>* outputWriter,
    DWORD* videoOutputStream,
    DWORD* audioOutputStream) {
    if (videoType == nullptr || audioType == nullptr ||
        outputWriter == nullptr || videoOutputStream == nullptr ||
        audioOutputStream == nullptr) {
        return MakeStep(
            AudioVideoMuxOutcome::Failed,
            E_POINTER,
            L"创建音视频重封装器时收到空指针。");
    }
    outputWriter->Reset();

    ComPtr<IMFAttributes> attributes;
    HRESULT result = ::MFCreateAttributes(&attributes, 4);
    if (SUCCEEDED(result)) {
        result = attributes->SetUINT32(
            MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS,
            TRUE);
    }
    if (SUCCEEDED(result)) {
        result = attributes->SetUINT32(
            MF_MPEG4SINK_SPSPPS_PASSTHROUGH,
            TRUE);
    }
    if (SUCCEEDED(result)) {
        result = attributes->SetUINT32(
            MF_SINK_WRITER_DISABLE_THROTTLING,
            TRUE);
    }

    ComPtr<IMFSinkWriter> writer;
    if (SUCCEEDED(result)) {
        result = ::MFCreateSinkWriterFromURL(
            destinationPath.c_str(),
            nullptr,
            attributes.Get(),
            &writer);
    }
    if (SUCCEEDED(result)) {
        result = writer->AddStream(videoType, videoOutputStream);
    }
    if (SUCCEEDED(result)) {
        result = writer->SetInputMediaType(
            *videoOutputStream,
            videoType,
            nullptr);
    }
    if (SUCCEEDED(result)) {
        result = writer->AddStream(audioType, audioOutputStream);
    }
    if (SUCCEEDED(result)) {
        result = writer->SetInputMediaType(
            *audioOutputStream,
            audioType,
            nullptr);
    }
    if (SUCCEEDED(result)) {
        result = writer->BeginWriting();
    }
    if (FAILED(result)) {
        return MakeStep(
            AudioVideoMuxOutcome::Failed,
            result,
            ErrorText(L"启动音视频 MP4 重封装器失败", result));
    }
    *outputWriter = std::move(writer);
    return MakeStep(AudioVideoMuxOutcome::Succeeded, S_OK);
}

[[nodiscard]] AudioVideoMuxResult ResultFromStep(
    StepResult step,
    AudioVideoMuxResult current) {
    current.outcome = step.outcome;
    current.nativeError = step.nativeError;
    current.errorMessage = std::move(step.errorMessage);
    return current;
}

[[nodiscard]] StepResult ValidateAndPreparePaths(
    const AudioVideoMuxRequest& request,
    std::filesystem::path* videoPath,
    std::filesystem::path* audioPath,
    std::filesystem::path* destinationPath) {
    if (videoPath == nullptr || audioPath == nullptr ||
        destinationPath == nullptr || request.trimEnd <= request.trimStart ||
        request.trimmedVideoPath.empty() || request.audioSidecarPath.empty() ||
        request.destinationPath.empty()) {
        return MakeStep(
            AudioVideoMuxOutcome::Unsupported,
            E_INVALIDARG,
            L"音视频合并参数无效。");
    }

    std::error_code pathError;
    *videoPath = std::filesystem::absolute(
        request.trimmedVideoPath,
        pathError).lexically_normal();
    if (pathError) {
        return MakeStep(
            AudioVideoMuxOutcome::Failed,
            HRESULT_FROM_WIN32(static_cast<DWORD>(pathError.value())),
            L"无法规范化 H.264 视频路径。");
    }
    *audioPath = std::filesystem::absolute(
        request.audioSidecarPath,
        pathError).lexically_normal();
    if (pathError) {
        return MakeStep(
            AudioVideoMuxOutcome::Failed,
            HRESULT_FROM_WIN32(static_cast<DWORD>(pathError.value())),
            L"无法规范化 AAC 音频旁路路径。");
    }
    *destinationPath = std::filesystem::absolute(
        request.destinationPath,
        pathError).lexically_normal();
    if (pathError ||
        _wcsicmp(videoPath->c_str(), destinationPath->c_str()) == 0 ||
        _wcsicmp(audioPath->c_str(), destinationPath->c_str()) == 0 ||
        _wcsicmp(destinationPath->extension().c_str(), L".mp4") != 0) {
        return MakeStep(
            AudioVideoMuxOutcome::Unsupported,
            E_INVALIDARG,
            L"目标必须是与输入不同的新 .mp4 文件。");
    }

    const bool videoExists = std::filesystem::is_regular_file(
        *videoPath,
        pathError);
    if (pathError || !videoExists) {
        return MakeStep(
            AudioVideoMuxOutcome::Failed,
            HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND),
            L"待合并的 H.264 MP4 不存在。");
    }
    const bool audioExists = std::filesystem::is_regular_file(
        *audioPath,
        pathError);
    if (pathError || !audioExists) {
        return MakeStep(
            AudioVideoMuxOutcome::Failed,
            HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND),
            L"待合并的 AAC/M4A 音频旁路不存在。");
    }
    const bool destinationExists = std::filesystem::exists(
        *destinationPath,
        pathError);
    if (pathError) {
        return MakeStep(
            AudioVideoMuxOutcome::Failed,
            HRESULT_FROM_WIN32(static_cast<DWORD>(pathError.value())),
            L"检查音视频合并目标失败。");
    }
    if (destinationExists) {
        return MakeStep(
            AudioVideoMuxOutcome::Failed,
            HRESULT_FROM_WIN32(ERROR_FILE_EXISTS),
            L"音视频合并目标文件已经存在。");
    }

    const std::filesystem::path parent = destinationPath->parent_path();
    if (!parent.empty()) {
        std::filesystem::create_directories(parent, pathError);
        if (pathError) {
            return MakeStep(
                AudioVideoMuxOutcome::Failed,
                HRESULT_FROM_WIN32(static_cast<DWORD>(pathError.value())),
                L"创建音视频合并目标目录失败。");
        }
    }
    return MakeStep(AudioVideoMuxOutcome::Succeeded, S_OK);
}

}  // namespace

CompressedVideoRetimeResult AudioVideoMuxer::RetimeCompressedVideo(
    const CompressedVideoRetimeRequest& request,
    const std::stop_token stopToken) noexcept {
    CompressedVideoRetimeResult retimeResult{};
    try {
        if (request.sourcePath.empty() || request.destinationPath.empty() ||
            request.playbackSpeedTenths < 1 ||
            request.playbackSpeedTenths > 30) {
            retimeResult.outcome = AudioVideoMuxOutcome::Unsupported;
            retimeResult.nativeError = E_INVALIDARG;
            retimeResult.errorMessage = L"H.264 重定时参数无效。";
            return retimeResult;
        }
        if (stopToken.stop_requested()) {
            retimeResult.outcome = AudioVideoMuxOutcome::Cancelled;
            retimeResult.nativeError = HRESULT_FROM_WIN32(ERROR_CANCELLED);
            return retimeResult;
        }

        std::error_code pathError;
        const std::filesystem::path sourcePath = std::filesystem::absolute(
            request.sourcePath,
            pathError).lexically_normal();
        if (pathError ||
            !std::filesystem::is_regular_file(sourcePath, pathError) ||
            pathError) {
            retimeResult.nativeError = HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND);
            retimeResult.errorMessage = L"H.264 重定时源文件不存在。";
            return retimeResult;
        }
        pathError.clear();
        const std::filesystem::path destinationPath = std::filesystem::absolute(
            request.destinationPath,
            pathError).lexically_normal();
        if (pathError ||
            _wcsicmp(sourcePath.c_str(), destinationPath.c_str()) == 0) {
            retimeResult.nativeError = E_INVALIDARG;
            retimeResult.errorMessage = L"H.264 重定时不能覆盖源文件。";
            return retimeResult;
        }
        if (::GetFileAttributesW(destinationPath.c_str()) !=
            INVALID_FILE_ATTRIBUTES) {
            retimeResult.nativeError = HRESULT_FROM_WIN32(ERROR_FILE_EXISTS);
            retimeResult.errorMessage = L"H.264 重定时目标文件已存在。";
            return retimeResult;
        }

        const std::filesystem::path parent = destinationPath.parent_path();
        if (!parent.empty()) {
            std::filesystem::create_directories(parent, pathError);
            if (pathError) {
                retimeResult.nativeError = HRESULT_FROM_WIN32(
                    static_cast<DWORD>(pathError.value()));
                retimeResult.errorMessage = L"无法创建 H.264 重定时输出目录。";
                return retimeResult;
            }
        }

        const win32::ScopedCoInitialize apartment(COINIT_MULTITHREADED);
        if (FAILED(apartment.Result()) &&
            apartment.Result() != RPC_E_CHANGED_MODE) {
            retimeResult.nativeError = apartment.Result();
            retimeResult.errorMessage = ErrorText(
                L"初始化 H.264 重定时 COM 线程失败",
                apartment.Result());
            return retimeResult;
        }
        const MediaFoundationSession mediaFoundation;
        if (FAILED(mediaFoundation.Result())) {
            retimeResult.nativeError = mediaFoundation.Result();
            retimeResult.errorMessage = ErrorText(
                L"启动 H.264 重定时 Media Foundation 失败",
                mediaFoundation.Result());
            return retimeResult;
        }

        SourceTrack video;
        StepResult step = OpenCompressedTrack(
            sourcePath,
            kVideoStream,
            MFMediaType_Video,
            MFVideoFormat_H264,
            &video);
        if (!step.Succeeded()) {
            retimeResult.outcome = step.outcome;
            retimeResult.nativeError = step.nativeError;
            retimeResult.errorMessage = std::move(step.errorMessage);
            return retimeResult;
        }
        HRESULT nativeResult = SeekReader(video.reader.Get(), 0);
        if (FAILED(nativeResult)) {
            retimeResult.nativeError = nativeResult;
            retimeResult.errorMessage = ErrorText(
                L"定位 H.264 重定时起点失败",
                nativeResult);
            return retimeResult;
        }

        ComPtr<IMFAttributes> writerAttributes;
        nativeResult = ::MFCreateAttributes(&writerAttributes, 4);
        if (SUCCEEDED(nativeResult)) {
            nativeResult = writerAttributes->SetUINT32(
                MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS,
                TRUE);
        }
        if (SUCCEEDED(nativeResult)) {
            nativeResult = writerAttributes->SetUINT32(
                MF_MPEG4SINK_SPSPPS_PASSTHROUGH,
                TRUE);
        }
        if (SUCCEEDED(nativeResult)) {
            nativeResult = writerAttributes->SetUINT32(
                MF_SINK_WRITER_DISABLE_THROTTLING,
                TRUE);
        }

        DestinationCleanup destinationCleanup(destinationPath);
        destinationCleanup.Activate();
        ComPtr<IMFSinkWriter> writer;
        DWORD outputStream = 0;
        if (SUCCEEDED(nativeResult)) {
            nativeResult = ::MFCreateSinkWriterFromURL(
                destinationPath.c_str(),
                nullptr,
                writerAttributes.Get(),
                &writer);
        }
        if (SUCCEEDED(nativeResult)) {
            nativeResult = writer->AddStream(
                video.nativeType.Get(),
                &outputStream);
        }
        if (SUCCEEDED(nativeResult)) {
            nativeResult = writer->SetInputMediaType(
                outputStream,
                video.nativeType.Get(),
                nullptr);
        }
        if (SUCCEEDED(nativeResult)) {
            nativeResult = writer->BeginWriting();
        }
        if (FAILED(nativeResult)) {
            retimeResult.nativeError = nativeResult;
            retimeResult.errorMessage = ErrorText(
                L"启动 H.264 压缩域重定时失败",
                nativeResult);
            return retimeResult;
        }

        const auto scaleTicks = [&request](const LONGLONG value) noexcept {
            if (value <= 0) {
                return LONGLONG{0};
            }
            const long double scaled =
                static_cast<long double>(value) * 10.0L /
                static_cast<long double>(request.playbackSpeedTenths);
            return static_cast<LONGLONG>(std::llround(std::min<long double>(
                scaled,
                static_cast<long double>(
                    std::numeric_limits<LONGLONG>::max()))));
        };

        LONGLONG previousSourceTime = -1;
        LONGLONG previousSourceEnd = -1;
        LONGLONG previousOutputTime = -1;
        LONGLONG previousOutputEnd = 0;
        bool firstSample = true;
        for (;;) {
            if (stopToken.stop_requested()) {
                retimeResult.outcome = AudioVideoMuxOutcome::Cancelled;
                retimeResult.nativeError = HRESULT_FROM_WIN32(ERROR_CANCELLED);
                return retimeResult;
            }

            DWORD actualStream = 0;
            DWORD flags = 0;
            LONGLONG sampleTime = 0;
            ComPtr<IMFSample> sample;
            nativeResult = video.reader->ReadSample(
                kVideoStream,
                0,
                &actualStream,
                &flags,
                &sampleTime,
                &sample);
            static_cast<void>(actualStream);
            if (FAILED(nativeResult) ||
                (flags & MF_SOURCE_READERF_ERROR) != 0) {
                retimeResult.nativeError = FAILED(nativeResult)
                    ? nativeResult
                    : E_FAIL;
                retimeResult.errorMessage = ErrorText(
                    L"读取 H.264 重定时样本失败",
                    retimeResult.nativeError);
                return retimeResult;
            }
            if ((flags & (MF_SOURCE_READERF_CURRENTMEDIATYPECHANGED |
                          MF_SOURCE_READERF_NATIVEMEDIATYPECHANGED)) != 0) {
                retimeResult.outcome = AudioVideoMuxOutcome::Unsupported;
                retimeResult.nativeError = MF_E_INVALIDMEDIATYPE;
                retimeResult.errorMessage = L"H.264 轨道在重定时期间改变了媒体类型。";
                return retimeResult;
            }
            if ((flags & MF_SOURCE_READERF_STREAMTICK) != 0) {
                retimeResult.outcome = AudioVideoMuxOutcome::Unsupported;
                retimeResult.nativeError = MF_E_INVALID_TIMESTAMP;
                retimeResult.errorMessage = L"H.264 轨道包含无法压缩域重定时的 stream tick。";
                return retimeResult;
            }
            if ((flags & MF_SOURCE_READERF_ENDOFSTREAM) != 0) {
                break;
            }
            if (sample == nullptr) {
                continue;
            }

            LONGLONG sampleDuration = 0;
            nativeResult = sample->GetSampleDuration(&sampleDuration);
            if (FAILED(nativeResult) || sampleDuration <= 0) {
                retimeResult.outcome = AudioVideoMuxOutcome::Unsupported;
                retimeResult.nativeError = MF_E_INVALID_TIMESTAMP;
                retimeResult.errorMessage = L"H.264 压缩样本缺少有效时长。";
                return retimeResult;
            }
            step = ValidateSampleTimeline(
                sampleTime,
                sampleDuration,
                &previousSourceTime,
                &previousSourceEnd,
                L"H.264 ");
            if (!step.Succeeded()) {
                retimeResult.outcome = step.outcome;
                retimeResult.nativeError = step.nativeError;
                retimeResult.errorMessage = std::move(step.errorMessage);
                return retimeResult;
            }
            if (firstSample &&
                (sampleTime != 0 || !IsCleanPoint(sample.Get()))) {
                retimeResult.outcome = AudioVideoMuxOutcome::Unsupported;
                retimeResult.nativeError = MF_E_INVALID_TIMESTAMP;
                retimeResult.errorMessage = L"H.264 重定时源必须从时间零的 CleanPoint 开始。";
                return retimeResult;
            }
            step = ValidateAndRemoveVideoDecodeTimestamp(
                sample.Get(),
                sampleTime);
            if (!step.Succeeded()) {
                retimeResult.outcome = step.outcome;
                retimeResult.nativeError = step.nativeError;
                retimeResult.errorMessage = std::move(step.errorMessage);
                return retimeResult;
            }

            LONGLONG outputTime = scaleTicks(sampleTime);
            LONGLONG outputEnd = scaleTicks(sampleTime + sampleDuration);
            if (previousOutputTime >= 0) {
                outputTime = std::max(outputTime, previousOutputEnd);
            }
            outputEnd = std::max(outputEnd, outputTime + 1);
            const LONGLONG outputDuration = outputEnd - outputTime;
            nativeResult = sample->SetSampleTime(outputTime);
            if (SUCCEEDED(nativeResult)) {
                nativeResult = sample->SetSampleDuration(outputDuration);
            }
            if (SUCCEEDED(nativeResult) && firstSample) {
                nativeResult = sample->SetUINT32(
                    MFSampleExtension_Discontinuity,
                    TRUE);
            }
            if (SUCCEEDED(nativeResult)) {
                nativeResult = writer->WriteSample(
                    outputStream,
                    sample.Get());
            }
            if (FAILED(nativeResult)) {
                retimeResult.nativeError = nativeResult;
                retimeResult.errorMessage = ErrorText(
                    L"写入 H.264 重定时样本失败",
                    nativeResult);
                return retimeResult;
            }
            firstSample = false;
            previousOutputTime = outputTime;
            previousOutputEnd = outputEnd;
            ++retimeResult.videoSamples;
        }

        if (retimeResult.videoSamples == 0 || firstSample) {
            retimeResult.outcome = AudioVideoMuxOutcome::Unsupported;
            retimeResult.nativeError = MF_E_END_OF_STREAM;
            retimeResult.errorMessage = L"H.264 重定时源不包含视频样本。";
            return retimeResult;
        }
        if (stopToken.stop_requested()) {
            retimeResult.outcome = AudioVideoMuxOutcome::Cancelled;
            retimeResult.nativeError = HRESULT_FROM_WIN32(ERROR_CANCELLED);
            return retimeResult;
        }
        nativeResult = writer->Finalize();
        writer.Reset();
        if (FAILED(nativeResult)) {
            retimeResult.nativeError = nativeResult;
            retimeResult.errorMessage = ErrorText(
                L"完成 H.264 压缩域重定时失败",
                nativeResult);
            return retimeResult;
        }

        retimeResult.outcome = AudioVideoMuxOutcome::Succeeded;
        retimeResult.nativeError = S_OK;
        retimeResult.outputDuration = TicksToNanoseconds(previousOutputEnd);
        retimeResult.errorMessage.clear();
        destinationCleanup.Release();
        return retimeResult;
    } catch (const std::exception&) {
        retimeResult.nativeError = E_UNEXPECTED;
        retimeResult.errorMessage = L"H.264 重定时发生未预期的标准库异常。";
        return retimeResult;
    } catch (...) {
        retimeResult.nativeError = E_UNEXPECTED;
        retimeResult.errorMessage = L"H.264 重定时发生未预期异常。";
        return retimeResult;
    }
}

AudioVideoMuxResult AudioVideoMuxer::Mux(
    const AudioVideoMuxRequest& request,
    const std::stop_token stopToken) noexcept {
    AudioVideoMuxResult result{};
    try {
        LONGLONG trimStart = 0;
        LONGLONG trimEnd = 0;
        if (!ToTicks(request.trimStart, &trimStart) ||
            !ToTicks(request.trimEnd, &trimEnd) || trimEnd <= trimStart) {
            return MakeResult(
                AudioVideoMuxOutcome::Unsupported,
                E_INVALIDARG,
                L"音视频合并裁剪区间无效。");
        }
        if (stopToken.stop_requested()) {
            return MakeResult(
                AudioVideoMuxOutcome::Cancelled,
                HRESULT_FROM_WIN32(ERROR_CANCELLED));
        }

        std::filesystem::path videoPath;
        std::filesystem::path audioPath;
        std::filesystem::path destinationPath;
        StepResult step = ValidateAndPreparePaths(
            request,
            &videoPath,
            &audioPath,
            &destinationPath);
        if (!step.Succeeded()) {
            return ResultFromStep(std::move(step), std::move(result));
        }

        const win32::ScopedCoInitialize apartment(COINIT_MULTITHREADED);
        if (FAILED(apartment.Result()) &&
            apartment.Result() != RPC_E_CHANGED_MODE) {
            return MakeResult(
                AudioVideoMuxOutcome::Failed,
                apartment.Result(),
                ErrorText(L"初始化音视频合并 COM 线程失败", apartment.Result()));
        }
        const MediaFoundationSession mediaFoundation;
        if (FAILED(mediaFoundation.Result())) {
            return MakeResult(
                AudioVideoMuxOutcome::Failed,
                mediaFoundation.Result(),
                ErrorText(L"启动音视频合并 Media Foundation 失败",
                          mediaFoundation.Result()));
        }

        SourceTrack video;
        step = OpenCompressedTrack(
            videoPath,
            kVideoStream,
            MFMediaType_Video,
            MFVideoFormat_H264,
            &video);
        if (!step.Succeeded()) {
            return ResultFromStep(std::move(step), std::move(result));
        }
        SourceTrack audio;
        step = OpenCompressedTrack(
            audioPath,
            kAudioStream,
            MFMediaType_Audio,
            MFAudioFormat_AAC,
            &audio);
        if (!step.Succeeded()) {
            return ResultFromStep(std::move(step), std::move(result));
        }

        LONGLONG measuredVideoDuration = 0;
        step = ReadPresentationDuration(
            video.reader.Get(),
            &measuredVideoDuration);
        if (!step.Succeeded()) {
            return ResultFromStep(std::move(step), std::move(result));
        }
        media::CompressedTimelineAlignment alignment{};
        if (!media::TryAlignCompressedVideoToRange(
                trimStart,
                trimEnd,
                measuredVideoDuration,
                &alignment)) {
            return MakeResult(
                AudioVideoMuxOutcome::Unsupported,
                MF_E_INVALID_TIMESTAMP,
                L"H.264 轨道实际时长无效。");
        }

        result.requestedDuration = TicksToNanoseconds(
            alignment.requestedDurationTicks);
        result.videoDuration = TicksToNanoseconds(
            alignment.outputDurationTicks);
        result.videoStartAdjustment = TicksToNanoseconds(
            alignment.videoStartAdjustmentTicks);
        result.effectiveAudioTrimStart = TicksToNanoseconds(
            alignment.effectiveAudioTrimStartTicks);
        result.effectiveAudioTrimEnd = TicksToNanoseconds(
            alignment.effectiveAudioTrimEndTicks);

        HRESULT nativeResult = SeekReader(video.reader.Get(), 0);
        if (SUCCEEDED(nativeResult)) {
            nativeResult = SeekReader(
                audio.reader.Get(),
                std::max<LONGLONG>(
                    0,
                    alignment.effectiveAudioTrimStartTicks - std::min(
                        alignment.effectiveAudioTrimStartTicks,
                        kAudioBoundaryProbePrerollTicks)));
        }
        if (FAILED(nativeResult)) {
            return MakeResult(
                AudioVideoMuxOutcome::Failed,
                nativeResult,
                ErrorText(L"定位音视频重封装起点失败", nativeResult));
        }

        VideoReadState videoState{};
        videoState.reader = video.reader.Get();
        videoState.outputDuration = alignment.outputDurationTicks;
        videoState.requestedDuration = alignment.requestedDurationTicks;
        videoState.nominalFrameDuration = NominalVideoFrameDuration(
            video.nativeType.Get());
        AudioReadState audioState{};
        audioState.reader = audio.reader.Get();
        audioState.trimStart = alignment.effectiveAudioTrimStartTicks;
        audioState.trimEnd = alignment.effectiveAudioTrimEndTicks;

        step = ReadNextVideoSample(&videoState, stopToken);
        if (!step.Succeeded()) {
            return ResultFromStep(std::move(step), std::move(result));
        }
        step = ReadNextAudioSample(&audioState, stopToken);
        if (!step.Succeeded()) {
            return ResultFromStep(std::move(step), std::move(result));
        }
        if (videoState.pending.sample == nullptr) {
            return MakeResult(
                AudioVideoMuxOutcome::Unsupported,
                MF_E_END_OF_STREAM,
                L"目标区间内没有可重封装的 H.264 样本。");
        }
        if (audioState.pending.sample == nullptr) {
            return MakeResult(
                AudioVideoMuxOutcome::Unsupported,
                MF_E_END_OF_STREAM,
                L"目标区间短于完整 AAC access unit，无法无损加入音轨。");
        }

        DestinationCleanup destinationCleanup(destinationPath);
        destinationCleanup.Activate();
        ComPtr<IMFSinkWriter> writer;
        DWORD videoOutputStream = 0;
        DWORD audioOutputStream = 0;
        step = CreateMuxWriter(
            destinationPath,
            video.nativeType.Get(),
            audio.nativeType.Get(),
            &writer,
            &videoOutputStream,
            &audioOutputStream);
        if (!step.Succeeded()) {
            return ResultFromStep(std::move(step), std::move(result));
        }

        const LONGLONG firstAudioTime = audioState.pending.outputTime;
        if (firstAudioTime > 0) {
            nativeResult = writer->SendStreamTick(audioOutputStream, 0);
            if (FAILED(nativeResult)) {
                return MakeResult(
                    AudioVideoMuxOutcome::Failed,
                    nativeResult,
                    ErrorText(L"写入 AAC 起始空隙标记失败", nativeResult));
            }
        }
        while (videoState.pending.sample != nullptr ||
               audioState.pending.sample != nullptr) {
            if (stopToken.stop_requested()) {
                return MakeResult(
                    AudioVideoMuxOutcome::Cancelled,
                    HRESULT_FROM_WIN32(ERROR_CANCELLED));
            }
            const bool writeVideo = audioState.pending.sample == nullptr ||
                (videoState.pending.sample != nullptr &&
                 videoState.pending.outputTime <=
                     audioState.pending.outputTime);
            if (writeVideo) {
                nativeResult = writer->WriteSample(
                    videoOutputStream,
                    videoState.pending.sample.Get());
                if (FAILED(nativeResult)) {
                    return MakeResult(
                        AudioVideoMuxOutcome::Failed,
                        nativeResult,
                        ErrorText(L"写入 H.264 直通样本失败", nativeResult));
                }
                ++result.videoSamples;
                videoState.pending.Reset();
                step = ReadNextVideoSample(&videoState, stopToken);
            } else {
                nativeResult = writer->WriteSample(
                    audioOutputStream,
                    audioState.pending.sample.Get());
                if (FAILED(nativeResult)) {
                    return MakeResult(
                        AudioVideoMuxOutcome::Failed,
                        nativeResult,
                        ErrorText(L"写入 AAC 直通样本失败", nativeResult));
                }
                ++result.audioSamples;
                audioState.pending.Reset();
                step = ReadNextAudioSample(&audioState, stopToken);
            }
            if (!step.Succeeded()) {
                return ResultFromStep(std::move(step), std::move(result));
            }
        }

        if (result.videoSamples == 0 || result.audioSamples == 0) {
            return ResultFromStep(
                MakeStep(
                    AudioVideoMuxOutcome::Unsupported,
                    MF_E_END_OF_STREAM,
                    L"音视频轨道没有可写入的压缩样本。"),
                std::move(result));
        }
        if (!CoversExpectedVideoEnd(videoState)) {
            return ResultFromStep(
                MakeStep(
                    AudioVideoMuxOutcome::Unsupported,
                    MF_E_END_OF_STREAM,
                    L"H.264 轨道没有覆盖其实际媒体时长。"),
                std::move(result));
        }
        if (!media::CoversFrameQuantizedRequestedSpan(
                videoState.requestedDuration,
                videoState.outputDuration,
                videoState.nominalFrameDuration,
                videoState.maximumSourceSampleDuration,
                kTimestampToleranceTicks)) {
            return ResultFromStep(
                MakeStep(
                    AudioVideoMuxOutcome::Unsupported,
                    MF_E_END_OF_STREAM,
                    L"H.264 轨道比请求区间短超过一个视频帧。"),
                std::move(result));
        }
        if (stopToken.stop_requested()) {
            return MakeResult(
                AudioVideoMuxOutcome::Cancelled,
                HRESULT_FROM_WIN32(ERROR_CANCELLED));
        }

        nativeResult = writer->Finalize();
        writer.Reset();
        if (FAILED(nativeResult)) {
            return MakeResult(
                AudioVideoMuxOutcome::Failed,
                nativeResult,
                ErrorText(L"完成音视频 MP4 重封装失败", nativeResult));
        }
        if (stopToken.stop_requested()) {
            return MakeResult(
                AudioVideoMuxOutcome::Cancelled,
                HRESULT_FROM_WIN32(ERROR_CANCELLED));
        }

        result.outcome = AudioVideoMuxOutcome::Succeeded;
        result.nativeError = S_OK;
        result.errorMessage.clear();
        result.audioLeadingGap = TicksToNanoseconds(firstAudioTime);
        result.audioTrailingGap = TicksToNanoseconds(
            std::max<LONGLONG>(
                0,
                alignment.outputDurationTicks - audioState.lastOutputEnd));
        result.droppedLeadingBoundaryAccessUnit =
            audioState.droppedLeadingBoundaryAccessUnit;
        result.droppedTrailingBoundaryAccessUnit =
            audioState.droppedTrailingBoundaryAccessUnit;
        destinationCleanup.Release();
        return result;
    } catch (const std::exception&) {
        return MakeResult(
            AudioVideoMuxOutcome::Failed,
            E_UNEXPECTED,
            L"音视频重封装发生未预期的标准库异常。");
    } catch (...) {
        return MakeResult(
            AudioVideoMuxOutcome::Failed,
            E_UNEXPECTED,
            L"音视频重封装发生未预期异常。");
    }
}

}  // namespace qrec
