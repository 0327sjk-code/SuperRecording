#include "media/Mp4BoundaryInternal.h"

#include "common/Win32Helpers.h"
#include "media/Mp4Writer.h"

#include <mfapi.h>
#include <mferror.h>
#include <mfreadwrite.h>
#include <propvarutil.h>

#include <algorithm>
#include <limits>
#include <utility>

namespace qrec::detail {
namespace {

constexpr DWORD kAllStreams =
    static_cast<DWORD>(MF_SOURCE_READER_ALL_STREAMS);
constexpr DWORD kVideoStream =
    static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM);
constexpr LONGLONG kTicksPerSecond = 10'000'000;
constexpr LONGLONG kDefaultFrameDuration = 166'667;

[[nodiscard]] std::wstring BoundaryErrorText(
    const std::wstring_view operation,
    const HRESULT result) {
    return std::wstring(operation) + L"：" + win32::FormatError(result);
}

[[nodiscard]] HRESULT FindNativeH264Type(
    IMFSourceReader* reader,
    IMFMediaType** output) {
    if (reader == nullptr || output == nullptr) {
        return E_POINTER;
    }
    *output = nullptr;
    for (DWORD typeIndex = 0;; ++typeIndex) {
        ComPtr<IMFMediaType> candidate;
        const HRESULT result = reader->GetNativeMediaType(
            kVideoStream,
            typeIndex,
            &candidate);
        if (result == MF_E_NO_MORE_TYPES) {
            return MF_E_INVALIDMEDIATYPE;
        }
        if (FAILED(result)) {
            return result;
        }
        GUID majorType{};
        GUID subtype{};
        if (SUCCEEDED(candidate->GetGUID(MF_MT_MAJOR_TYPE, &majorType)) &&
            SUCCEEDED(candidate->GetGUID(MF_MT_SUBTYPE, &subtype)) &&
            majorType == MFMediaType_Video && subtype == MFVideoFormat_H264) {
            *output = candidate.Detach();
            return S_OK;
        }
    }
}

[[nodiscard]] BoundaryStepResult ValidateSourceGeometry(
    IMFMediaType* nativeType,
    BoundarySourcePlan* plan) {
    if (nativeType == nullptr || plan == nullptr) {
        return MakeBoundaryStep(
            Mp4BoundaryTrimOutcome::Failed,
            E_POINTER,
            L"读取边界裁剪媒体类型时收到空指针。");
    }

    HRESULT result = ::MFGetAttributeSize(
        nativeType,
        MF_MT_FRAME_SIZE,
        &plan->width,
        &plan->height);
    if (FAILED(result) || plan->width == 0 || plan->height == 0) {
        return MakeBoundaryStep(
            Mp4BoundaryTrimOutcome::Unsupported,
            FAILED(result) ? result : MF_E_INVALIDMEDIATYPE,
            L"源 H.264 缺少有效画面尺寸。");
    }
    result = ::MFGetAttributeRatio(
        nativeType,
        MF_MT_FRAME_RATE,
        &plan->frameRateNumerator,
        &plan->frameRateDenominator);
    if (FAILED(result) || plan->frameRateNumerator == 0 ||
        plan->frameRateDenominator == 0) {
        return MakeBoundaryStep(
            Mp4BoundaryTrimOutcome::Unsupported,
            FAILED(result) ? result : MF_E_INVALIDMEDIATYPE,
            L"源 H.264 缺少有效帧率。");
    }

    const std::uint64_t frameRateNumerator = plan->frameRateNumerator;
    const std::uint64_t frameRateDenominator = plan->frameRateDenominator;
    if (frameRateNumerator == 30ULL * frameRateDenominator) {
        plan->framesPerSecond = 30;
    } else if (frameRateNumerator == 60ULL * frameRateDenominator) {
        plan->framesPerSecond = 60;
    } else {
        return MakeBoundaryStep(
            Mp4BoundaryTrimOutcome::Unsupported,
            MF_E_INVALIDMEDIATYPE,
            L"边界重编码当前仅支持 30 FPS 或 60 FPS 源视频。");
    }

    UINT32 sequenceBytes = 0;
    result = nativeType->GetBlobSize(
        MF_MT_MPEG_SEQUENCE_HEADER,
        &sequenceBytes);
    if (FAILED(result) || sequenceBytes == 0) {
        return MakeBoundaryStep(
            Mp4BoundaryTrimOutcome::Unsupported,
            FAILED(result) ? result : MF_E_INVALIDMEDIATYPE,
            L"源 H.264 缺少 MPEG sequence header，不能安全拼接。");
    }

    UINT32 averageBitrate = 0;
    if (SUCCEEDED(nativeType->GetUINT32(
            MF_MT_AVG_BITRATE,
            &averageBitrate)) &&
        averageBitrate != 0) {
        plan->averageBitrate = averageBitrate;
    } else {
        plan->averageBitrate = media::Mp4Writer::RecommendBitrate(
            plan->width,
            plan->height,
            plan->framesPerSecond);
    }
    plan->nominalFrameDuration = BoundaryFrameDuration(nativeType);
    return MakeBoundaryStep(Mp4BoundaryTrimOutcome::Succeeded, S_OK);
}

}  // namespace

BoundaryStepResult MakeBoundaryStep(
    const Mp4BoundaryTrimOutcome outcome,
    const HRESULT nativeError,
    std::wstring errorMessage) {
    BoundaryStepResult result{};
    result.outcome = outcome;
    result.nativeError = nativeError;
    result.errorMessage = std::move(errorMessage);
    return result;
}

BoundaryStepResult OpenNativeH264Source(
    const std::filesystem::path& path,
    ComPtr<IMFSourceReader>* reader,
    ComPtr<IMFMediaType>* nativeType) {
    if (path.empty() || reader == nullptr || nativeType == nullptr) {
        return MakeBoundaryStep(
            Mp4BoundaryTrimOutcome::Failed,
            E_INVALIDARG,
            L"打开边界裁剪源文件的参数无效。");
    }
    reader->Reset();
    nativeType->Reset();

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
            reader->ReleaseAndGetAddressOf());
    }
    if (FAILED(result)) {
        return MakeBoundaryStep(
            Mp4BoundaryTrimOutcome::Failed,
            result,
            BoundaryErrorText(L"打开 H.264 源文件失败", result));
    }

    static_cast<void>((*reader)->SetStreamSelection(kAllStreams, FALSE));
    result = (*reader)->SetStreamSelection(kVideoStream, TRUE);
    if (SUCCEEDED(result)) {
        result = FindNativeH264Type(
            reader->Get(),
            nativeType->ReleaseAndGetAddressOf());
    }
    if (result == MF_E_INVALIDMEDIATYPE) {
        return MakeBoundaryStep(
            Mp4BoundaryTrimOutcome::Unsupported,
            result,
            L"源文件不是可直接拼接的 H.264 MP4。");
    }
    if (SUCCEEDED(result)) {
        result = (*reader)->SetCurrentMediaType(
            kVideoStream,
            nullptr,
            nativeType->Get());
    }
    if (FAILED(result)) {
        return MakeBoundaryStep(
            Mp4BoundaryTrimOutcome::Failed,
            result,
            BoundaryErrorText(L"选择原生 H.264 媒体类型失败", result));
    }
    return MakeBoundaryStep(Mp4BoundaryTrimOutcome::Succeeded, S_OK);
}

HRESULT SeekBoundaryReader(
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

BoundaryStepResult ValidateNoBFrameOrder(
    IMFSample* sample,
    const LONGLONG presentationTime,
    LONGLONG* previousPresentationTime,
    const bool removeDecodeTimestamp) {
    if (sample == nullptr || previousPresentationTime == nullptr ||
        presentationTime < 0) {
        return MakeBoundaryStep(
            Mp4BoundaryTrimOutcome::Unsupported,
            MF_E_INVALID_TIMESTAMP,
            L"H.264 样本包含无效的呈现时间戳。");
    }

    UINT64 decodeTimestamp = 0;
    const HRESULT decodeResult = sample->GetUINT64(
        MFSampleExtension_DecodeTimestamp,
        &decodeTimestamp);
    if (SUCCEEDED(decodeResult)) {
        if (decodeTimestamp > static_cast<UINT64>(
                std::numeric_limits<LONGLONG>::max()) ||
            static_cast<LONGLONG>(decodeTimestamp) != presentationTime) {
            return MakeBoundaryStep(
                Mp4BoundaryTrimOutcome::Unsupported,
                MF_E_INVALID_TIMESTAMP,
                L"源 H.264 包含 B 帧或解码时间重排序，不能边界拼接。");
        }
        if (removeDecodeTimestamp) {
            const HRESULT deleteResult = sample->DeleteItem(
                MFSampleExtension_DecodeTimestamp);
            if (FAILED(deleteResult)) {
                return MakeBoundaryStep(
                    Mp4BoundaryTrimOutcome::Failed,
                    deleteResult,
                    BoundaryErrorText(
                        L"清理 H.264 解码时间戳失败",
                        deleteResult));
            }
        }
    } else if (decodeResult != MF_E_ATTRIBUTENOTFOUND) {
        return MakeBoundaryStep(
            Mp4BoundaryTrimOutcome::Failed,
            decodeResult,
            BoundaryErrorText(L"读取 H.264 解码时间戳失败", decodeResult));
    }

    if (*previousPresentationTime >= 0 &&
        presentationTime <= *previousPresentationTime) {
        return MakeBoundaryStep(
            Mp4BoundaryTrimOutcome::Unsupported,
            MF_E_INVALID_TIMESTAMP,
            L"源 H.264 样本时间戳不严格递增，不能边界拼接。");
    }
    *previousPresentationTime = presentationTime;
    return MakeBoundaryStep(Mp4BoundaryTrimOutcome::Succeeded, S_OK);
}

bool IsCleanPoint(IMFSample* sample) noexcept {
    if (sample == nullptr) {
        return false;
    }
    UINT32 cleanPoint = FALSE;
    return SUCCEEDED(sample->GetUINT32(
               MFSampleExtension_CleanPoint,
               &cleanPoint)) &&
        cleanPoint != FALSE;
}

LONGLONG BoundaryFrameDuration(IMFMediaType* mediaType) noexcept {
    if (mediaType == nullptr) {
        return kDefaultFrameDuration;
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
        return kDefaultFrameDuration;
    }
    const LONGLONG scaled = kTicksPerSecond * denominator;
    return std::max<LONGLONG>(1, (scaled + numerator / 2) / numerator);
}

BoundaryStepResult ProbeBoundaryEncoderKey(
    const std::filesystem::path& sourcePath,
    Mp4BoundaryEncoderKey* output) {
    if (output == nullptr) {
        return MakeBoundaryStep(
            Mp4BoundaryTrimOutcome::Failed,
            E_POINTER,
            L"预热边界编码器时收到空输出指针。");
    }

    ComPtr<IMFSourceReader> reader;
    ComPtr<IMFMediaType> nativeType;
    BoundaryStepResult step = OpenNativeH264Source(
        sourcePath,
        &reader,
        &nativeType);
    if (!step.Succeeded()) {
        return step;
    }

    BoundarySourcePlan plan{};
    step = ValidateSourceGeometry(nativeType.Get(), &plan);
    if (!step.Succeeded()) {
        return step;
    }

    const std::optional<Mp4BoundaryEncoderKey> key =
        Mp4BoundaryEncoderPool::MakeKey(
            sourcePath,
            plan.width,
            plan.height,
            plan.framesPerSecond,
            plan.averageBitrate);
    if (!key.has_value()) {
        return MakeBoundaryStep(
            Mp4BoundaryTrimOutcome::Unsupported,
            E_INVALIDARG,
            L"源视频参数无法建立边界编码器预热配置。");
    }
    *output = *key;
    return MakeBoundaryStep(Mp4BoundaryTrimOutcome::Succeeded, S_OK);
}

BoundaryStepResult AnalyzeBoundarySource(
    const std::filesystem::path& sourcePath,
    const LONGLONG requestedStart,
    const LONGLONG requestedEnd,
    const std::stop_token stopToken,
    BoundarySourcePlan* output) {
    if (output == nullptr || requestedStart < 0 ||
        requestedEnd <= requestedStart) {
        return MakeBoundaryStep(
            Mp4BoundaryTrimOutcome::Unsupported,
            E_INVALIDARG,
            L"边界裁剪区间无效。");
    }

    BoundarySourcePlan plan{};
    plan.requestedStart = requestedStart;
    plan.requestedEnd = requestedEnd;
    ComPtr<IMFSourceReader> reader;
    BoundaryStepResult step = OpenNativeH264Source(
        sourcePath,
        &reader,
        &plan.nativeType);
    if (!step.Succeeded()) {
        return step;
    }
    step = ValidateSourceGeometry(plan.nativeType.Get(), &plan);
    if (!step.Succeeded()) {
        return step;
    }

    HRESULT result = SeekBoundaryReader(reader.Get(), requestedStart);
    if (FAILED(result)) {
        return MakeBoundaryStep(
            Mp4BoundaryTrimOutcome::Failed,
            result,
            BoundaryErrorText(L"定位边界裁剪起点失败", result));
    }

    bool firstSample = true;
    bool visibleSampleFound = false;
    LONGLONG previousTime = -1;
    for (;;) {
        if (stopToken.stop_requested()) {
            return MakeBoundaryStep(
                Mp4BoundaryTrimOutcome::Cancelled,
                HRESULT_FROM_WIN32(ERROR_CANCELLED));
        }
        DWORD actualStream = 0;
        DWORD flags = 0;
        LONGLONG sampleTime = 0;
        ComPtr<IMFSample> sample;
        result = reader->ReadSample(
            kVideoStream,
            0,
            &actualStream,
            &flags,
            &sampleTime,
            &sample);
        static_cast<void>(actualStream);
        if (FAILED(result) || (flags & MF_SOURCE_READERF_ERROR) != 0) {
            const HRESULT readError = FAILED(result) ? result : E_FAIL;
            return MakeBoundaryStep(
                Mp4BoundaryTrimOutcome::Failed,
                readError,
                BoundaryErrorText(L"扫描源 H.264 GOP 失败", readError));
        }
        if ((flags & (MF_SOURCE_READERF_CURRENTMEDIATYPECHANGED |
                      MF_SOURCE_READERF_NATIVEMEDIATYPECHANGED)) != 0) {
            return MakeBoundaryStep(
                Mp4BoundaryTrimOutcome::Unsupported,
                MF_E_INVALIDMEDIATYPE,
                L"源视频在裁剪区间内改变了媒体类型。");
        }
        if ((flags & MF_SOURCE_READERF_STREAMTICK) != 0) {
            return MakeBoundaryStep(
                Mp4BoundaryTrimOutcome::Unsupported,
                MF_E_INVALID_TIMESTAMP,
                L"源视频裁剪区间包含无法直通的 stream tick。" );
        }
        if ((flags & MF_SOURCE_READERF_ENDOFSTREAM) != 0) {
            break;
        }
        if (sample == nullptr) {
            continue;
        }

        step = ValidateNoBFrameOrder(
            sample.Get(),
            sampleTime,
            &previousTime,
            false);
        if (!step.Succeeded()) {
            return step;
        }
        if (firstSample) {
            firstSample = false;
            if (sampleTime > requestedStart || !IsCleanPoint(sample.Get())) {
                return MakeBoundaryStep(
                    Mp4BoundaryTrimOutcome::Unsupported,
                    MF_E_INVALID_TIMESTAMP,
                    L"源文件无法提供裁剪起点之前的 CleanPoint 解码预卷。");
            }
        }
        if (sampleTime >= requestedEnd) {
            break;
        }
        if (sampleTime < requestedStart) {
            continue;
        }

        if (!visibleSampleFound) {
            plan.visibleStart = sampleTime;
            visibleSampleFound = true;
            if (IsCleanPoint(sample.Get())) {
                plan.spliceTime = sampleTime;
                plan.encodeBoundary = false;
                *output = std::move(plan);
                return MakeBoundaryStep(
                    Mp4BoundaryTrimOutcome::Succeeded,
                    S_OK);
            }
            continue;
        }
        if (IsCleanPoint(sample.Get())) {
            plan.spliceTime = sampleTime;
            plan.encodeBoundary = true;
            *output = std::move(plan);
            return MakeBoundaryStep(Mp4BoundaryTrimOutcome::Succeeded, S_OK);
        }
    }

    if (!visibleSampleFound) {
        return MakeBoundaryStep(
            Mp4BoundaryTrimOutcome::Unsupported,
            MF_E_END_OF_STREAM,
            L"裁剪区间内没有可见视频帧。");
    }
    plan.spliceTime = requestedEnd;
    plan.encodeBoundary = true;
    *output = std::move(plan);
    return MakeBoundaryStep(Mp4BoundaryTrimOutcome::Succeeded, S_OK);
}

}  // namespace qrec::detail
