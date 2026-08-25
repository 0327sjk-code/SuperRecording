#include "media/Mp4SmartTrimmer.h"

#include "common/Win32Helpers.h"
#include "media/Mp4EditListPatcher.h"

#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <propvarutil.h>
#include <wrl/client.h>

#include <algorithm>
#include <limits>
#include <utility>

#pragma comment(lib, "mf.lib")
#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfreadwrite.lib")
#pragma comment(lib, "mfuuid.lib")

namespace qrec {
namespace {

using Microsoft::WRL::ComPtr;

constexpr LONGLONG kTicksPerMillisecond = 10'000;
constexpr LONGLONG kDefaultFrameDuration = 166'667;
constexpr LONGLONG kTicksPerSecond = 10'000'000;
constexpr DWORD kAllStreams = static_cast<DWORD>(MF_SOURCE_READER_ALL_STREAMS);
constexpr DWORD kVideoStream = static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM);

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

[[nodiscard]] std::wstring ErrorText(
    const std::wstring_view operation,
    const HRESULT result) {
    return std::wstring(operation) + L"：" + win32::FormatError(result);
}

[[nodiscard]] Mp4SmartTrimResult MakeResult(
    const Mp4SmartTrimOutcome outcome,
    const HRESULT nativeError,
    std::wstring errorMessage = {}) {
    Mp4SmartTrimResult result{};
    result.outcome = outcome;
    result.nativeError = nativeError;
    result.errorMessage = std::move(errorMessage);
    return result;
}

[[nodiscard]] bool ToTicks(
    const std::chrono::milliseconds value,
    LONGLONG* output) noexcept {
    if (output == nullptr || value.count() < 0 ||
        value.count() > std::numeric_limits<LONGLONG>::max() /
            kTicksPerMillisecond) {
        return false;
    }
    *output = value.count() * kTicksPerMillisecond;
    return true;
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

[[nodiscard]] HRESULT SeekReader(
    IMFSourceReader* reader,
    const LONGLONG position) {
    PROPVARIANT value{};
    ::PropVariantInit(&value);
    value.vt = VT_I8;
    value.hVal.QuadPart = position;
    const HRESULT result = reader->SetCurrentPosition(GUID_NULL, value);
    ::PropVariantClear(&value);
    return result;
}

[[nodiscard]] LONGLONG FrameDurationForType(
    IMFMediaType* mediaType) noexcept {
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

}  // namespace

Mp4SmartTrimResult Mp4SmartTrimmer::Trim(
    const std::filesystem::path& sourcePath,
    const std::filesystem::path& destinationPath,
    const std::chrono::milliseconds trimStart,
    const std::chrono::milliseconds trimEnd,
    const std::stop_token stopToken,
    const ProgressCallback& progress) {
    LONGLONG start = 0;
    LONGLONG end = 0;
    if (sourcePath.empty() || destinationPath.empty() ||
        !ToTicks(trimStart, &start) || !ToTicks(trimEnd, &end) || end <= start) {
        return MakeResult(
            Mp4SmartTrimOutcome::Unsupported,
            E_INVALIDARG,
            L"快速裁切区间无效。");
    }
    if (stopToken.stop_requested()) {
        return MakeResult(
            Mp4SmartTrimOutcome::Cancelled,
            HRESULT_FROM_WIN32(ERROR_CANCELLED));
    }
    DestinationCleanup destinationCleanup(destinationPath);

    ComPtr<IMFAttributes> readerAttributes;
    HRESULT nativeResult = ::MFCreateAttributes(&readerAttributes, 3);
    if (SUCCEEDED(nativeResult)) {
        nativeResult = readerAttributes->SetUINT32(
            MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING,
            FALSE);
    }
    if (SUCCEEDED(nativeResult)) {
        nativeResult = readerAttributes->SetUINT32(
            MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS,
            TRUE);
    }
    ComPtr<IMFSourceReader> reader;
    if (SUCCEEDED(nativeResult)) {
        nativeResult = ::MFCreateSourceReaderFromURL(
            sourcePath.c_str(),
            readerAttributes.Get(),
            &reader);
    }
    if (FAILED(nativeResult)) {
        return MakeResult(
            Mp4SmartTrimOutcome::Failed,
            nativeResult,
            ErrorText(L"打开快速裁切源文件失败", nativeResult));
    }
    static_cast<void>(reader->SetStreamSelection(kAllStreams, FALSE));
    nativeResult = reader->SetStreamSelection(kVideoStream, TRUE);
    ComPtr<IMFMediaType> nativeType;
    if (SUCCEEDED(nativeResult)) {
        nativeResult = FindNativeH264Type(reader.Get(), &nativeType);
    }
    if (nativeResult == MF_E_INVALIDMEDIATYPE) {
        return MakeResult(
            Mp4SmartTrimOutcome::Unsupported,
            nativeResult,
            L"源视频不是可直接重封装的 H.264 MP4。");
    }
    if (SUCCEEDED(nativeResult)) {
        nativeResult = reader->SetCurrentMediaType(
            kVideoStream,
            nullptr,
            nativeType.Get());
    }
    if (FAILED(nativeResult)) {
        return MakeResult(
            Mp4SmartTrimOutcome::Failed,
            nativeResult,
            ErrorText(L"读取原始 H.264 样本失败", nativeResult));
    }
    const LONGLONG fallbackFrameDuration = FrameDurationForType(nativeType.Get());

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
    ComPtr<IMFSinkWriter> writer;
    if (SUCCEEDED(nativeResult)) {
        nativeResult = ::MFCreateSinkWriterFromURL(
            destinationPath.c_str(),
            nullptr,
            writerAttributes.Get(),
            &writer);
        if (SUCCEEDED(nativeResult)) {
            destinationCleanup.Activate();
        }
    }
    DWORD outputStream = 0;
    if (SUCCEEDED(nativeResult)) {
        nativeResult = writer->AddStream(nativeType.Get(), &outputStream);
    }
    if (SUCCEEDED(nativeResult)) {
        nativeResult = writer->SetInputMediaType(
            outputStream,
            nativeType.Get(),
            nullptr);
    }
    if (SUCCEEDED(nativeResult)) {
        nativeResult = writer->BeginWriting();
    }
    if (SUCCEEDED(nativeResult)) {
        nativeResult = SeekReader(reader.Get(), start);
    }
    if (FAILED(nativeResult)) {
        return MakeResult(
            Mp4SmartTrimOutcome::Failed,
            nativeResult,
            ErrorText(L"启动 MP4 快速裁切失败", nativeResult));
    }

    Mp4SmartTrimResult result = MakeResult(Mp4SmartTrimOutcome::Failed, E_FAIL);
    LONGLONG firstSourceTime = -1;
    LONGLONG firstVisibleSourceTime = -1;
    LONGLONG lastWrittenSourceEnd = -1;
    LONGLONG previousSourceTime = -1;
    std::uint64_t sampleIndex = 0;
    while (!stopToken.stop_requested()) {
        DWORD actualStream = 0;
        DWORD streamFlags = 0;
        LONGLONG sourceTime = 0;
        ComPtr<IMFSample> sample;
        nativeResult = reader->ReadSample(
            kVideoStream,
            0,
            &actualStream,
            &streamFlags,
            &sourceTime,
            &sample);
        if (FAILED(nativeResult)) {
            result = MakeResult(
                Mp4SmartTrimOutcome::Failed,
                nativeResult,
                ErrorText(L"读取 H.264 样本失败", nativeResult));
            break;
        }
        if ((streamFlags & MF_SOURCE_READERF_ENDOFSTREAM) != 0) {
            break;
        }
        if (sample == nullptr) {
            continue;
        }
        if (sourceTime >= end) {
            break;
        }

        LONGLONG sampleDuration = 0;
        if (FAILED(sample->GetSampleDuration(&sampleDuration)) ||
            sampleDuration <= 0) {
            sampleDuration = fallbackFrameDuration;
        }
        sampleDuration = std::min(sampleDuration, end - sourceTime);
        if (sampleDuration <= 0) {
            break;
        }
        if (firstSourceTime < 0) {
            firstSourceTime = sourceTime;
            if (firstSourceTime > start) {
                result = MakeResult(
                    Mp4SmartTrimOutcome::Unsupported,
                    MF_E_INVALID_TIMESTAMP,
                    L"源文件无法提供裁切起点所需的关键帧预卷。");
                break;
            }
            UINT32 cleanPoint = FALSE;
            if (FAILED(sample->GetUINT32(
                    MFSampleExtension_CleanPoint,
                    &cleanPoint)) ||
                cleanPoint == FALSE) {
                result = MakeResult(
                    Mp4SmartTrimOutcome::Unsupported,
                    MF_E_INVALID_TIMESTAMP,
                    L"裁切起点不是可安全直通的 H.264 关键帧，需要兼容编码。");
                break;
            }
            static_cast<void>(sample->SetUINT32(
                MFSampleExtension_Discontinuity,
                TRUE));
        }

        // This fast path deliberately accepts only decode-order H.264. The
        // Media Foundation MP4 sink requires an explicit DTS for reordered
        // B-frames, while its UINT64 DTS attribute cannot represent the
        // negative preroll required by an exact edit. Legacy/reordered input
        // therefore falls back to the compatibility transcoder.
        UINT64 decodeTimestamp = 0;
        const HRESULT decodeTimestampResult = sample->GetUINT64(
            MFSampleExtension_DecodeTimestamp,
            &decodeTimestamp);
        if (SUCCEEDED(decodeTimestampResult)) {
            if (decodeTimestamp > static_cast<UINT64>(
                    std::numeric_limits<LONGLONG>::max()) ||
                static_cast<LONGLONG>(decodeTimestamp) != sourceTime) {
                result = MakeResult(
                    Mp4SmartTrimOutcome::Unsupported,
                    MF_E_INVALID_TIMESTAMP,
                    L"源视频包含重排序/B 帧时间戳，需要兼容编码。");
                break;
            }
            nativeResult = sample->DeleteItem(
                MFSampleExtension_DecodeTimestamp);
            if (FAILED(nativeResult)) {
                result = MakeResult(
                    Mp4SmartTrimOutcome::Failed,
                    nativeResult,
                    ErrorText(L"清理直通样本解码时间戳失败", nativeResult));
                break;
            }
        } else if (decodeTimestampResult != MF_E_ATTRIBUTENOTFOUND) {
            result = MakeResult(
                Mp4SmartTrimOutcome::Failed,
                decodeTimestampResult,
                ErrorText(L"读取直通样本解码时间戳失败", decodeTimestampResult));
            break;
        }
        if (previousSourceTime >= 0 && sourceTime < previousSourceTime) {
            result = MakeResult(
                Mp4SmartTrimOutcome::Unsupported,
                MF_E_INVALID_TIMESTAMP,
                L"源视频样本时间发生重排序，需要兼容编码。");
            break;
        }
        previousSourceTime = sourceTime;

        if (sourceTime < start) {
            ++result.prerollSamples;
        } else if (firstVisibleSourceTime < 0) {
            firstVisibleSourceTime = sourceTime;
        }
        // Keep preroll timestamps non-negative so the MF sink preserves every
        // compressed sample. After Finalize, Mp4EditListPatcher hides the
        // preroll in a real edts/elst box without touching H.264 payloads.
        nativeResult = sample->SetSampleTime(sourceTime - firstSourceTime);
        if (SUCCEEDED(nativeResult)) {
            nativeResult = sample->SetSampleDuration(sampleDuration);
        }
        if (SUCCEEDED(nativeResult)) {
            nativeResult = writer->WriteSample(outputStream, sample.Get());
        }
        if (FAILED(nativeResult)) {
            result = MakeResult(
                Mp4SmartTrimOutcome::Failed,
                nativeResult,
                ErrorText(L"写入快速裁切样本失败", nativeResult));
            break;
        }

        ++result.writtenSamples;
        lastWrittenSourceEnd = sourceTime + sampleDuration;
        ++sampleIndex;
        if (progress && (sampleIndex % 16 == 0)) {
            const double fraction = std::clamp(
                static_cast<double>(sourceTime - start) /
                    static_cast<double>(end - start),
                0.0,
                1.0);
            progress(fraction);
        }
    }

    if (stopToken.stop_requested()) {
        return MakeResult(
            Mp4SmartTrimOutcome::Cancelled,
            HRESULT_FROM_WIN32(ERROR_CANCELLED));
    }
    if (result.outcome != Mp4SmartTrimOutcome::Failed ||
        !result.errorMessage.empty()) {
        return result;
    }
    if (result.writtenSamples == 0) {
        return MakeResult(
            Mp4SmartTrimOutcome::Unsupported,
            MF_E_END_OF_STREAM,
            L"裁切区间内没有可写入的视频样本。");
    }
    if (firstVisibleSourceTime < 0 ||
        lastWrittenSourceEnd <= firstVisibleSourceTime) {
        return MakeResult(
            Mp4SmartTrimOutcome::Unsupported,
            MF_E_END_OF_STREAM,
            L"裁切区间短于可显示的视频帧。");
    }

    nativeResult = writer->Finalize();
    if (FAILED(nativeResult)) {
        return MakeResult(
            Mp4SmartTrimOutcome::Failed,
            nativeResult,
            ErrorText(L"完成快速裁切 MP4 失败", nativeResult));
    }
    writer.Reset();

    const Mp4EditListPatchResult patchResult = Mp4EditListPatcher::Patch(
        destinationPath,
        firstVisibleSourceTime - firstSourceTime,
        lastWrittenSourceEnd - firstVisibleSourceTime);
    if (patchResult.outcome != Mp4EditListPatchOutcome::Succeeded) {
        return MakeResult(
            patchResult.outcome == Mp4EditListPatchOutcome::Unsupported
                ? Mp4SmartTrimOutcome::Unsupported
                : Mp4SmartTrimOutcome::Failed,
            patchResult.nativeError,
            patchResult.errorMessage.empty()
                ? L"无法写入精确裁切所需的 MP4 编辑列表。"
                : patchResult.errorMessage);
    }

    result.outcome = Mp4SmartTrimOutcome::Succeeded;
    result.nativeError = S_OK;
    result.errorMessage.clear();
    if (progress) {
        progress(1.0);
    }
    destinationCleanup.Release();
    return result;
}

}  // namespace qrec
