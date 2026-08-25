#include "media/Mp4BoundaryInternal.h"

#include "common/Win32Helpers.h"

#include <mfapi.h>
#include <mferror.h>
#include <mfreadwrite.h>

#include <algorithm>
#include <string_view>

namespace qrec::detail {
namespace {

constexpr DWORD kVideoStream =
    static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM);
// Keep this aligned with the boundary compatibility gate. The boundary MP4
// may be shorter by two 100 ns ticks after its MP4 timescale round trip; the
// final sample is clipped to the exact splice time below.
constexpr LONGLONG kMaximumSampleEndRoundingDrift = 2;

[[nodiscard]] std::wstring MuxErrorText(
    const std::wstring_view operation,
    const HRESULT result) {
    return std::wstring(operation) + L"：" + win32::FormatError(result);
}

[[nodiscard]] BoundaryStepResult CreateCompressedWriter(
    const std::filesystem::path& destinationPath,
    IMFMediaType* nativeType,
    ComPtr<IMFSinkWriter>* writer,
    DWORD* outputStream) {
    if (nativeType == nullptr || writer == nullptr || outputStream == nullptr) {
        return MakeBoundaryStep(
            Mp4BoundaryTrimOutcome::Failed,
            E_POINTER,
            L"创建边界重封装器时收到空指针。");
    }

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
    if (SUCCEEDED(result)) {
        result = ::MFCreateSinkWriterFromURL(
            destinationPath.c_str(),
            nullptr,
            attributes.Get(),
            writer->ReleaseAndGetAddressOf());
    }
    if (SUCCEEDED(result)) {
        result = (*writer)->AddStream(nativeType, outputStream);
    }
    if (SUCCEEDED(result)) {
        result = (*writer)->SetInputMediaType(
            *outputStream,
            nativeType,
            nullptr);
    }
    if (SUCCEEDED(result)) {
        result = (*writer)->BeginWriting();
    }
    if (FAILED(result)) {
        return MakeBoundaryStep(
            Mp4BoundaryTrimOutcome::Failed,
            result,
            MuxErrorText(L"启动边界 MP4 重封装失败", result));
    }
    return MakeBoundaryStep(Mp4BoundaryTrimOutcome::Succeeded, S_OK);
}

struct SegmentWriteState final {
    LONGLONG lastOutputTime{-1};
    LONGLONG lastOutputEnd{};
    bool wroteAny{};
};

[[nodiscard]] BoundaryStepResult WritePendingSample(
    IMFSinkWriter* writer,
    const DWORD outputStream,
    IMFSample* sample,
    const LONGLONG sourceTime,
    const LONGLONG sourceDuration,
    const LONGLONG sourceBegin,
    const LONGLONG outputBase,
    SegmentWriteState* state) {
    if (writer == nullptr || sample == nullptr || state == nullptr ||
        sourceDuration <= 0 || sourceTime < sourceBegin) {
        return MakeBoundaryStep(
            Mp4BoundaryTrimOutcome::Failed,
            E_INVALIDARG,
            L"写入边界压缩样本的参数无效。");
    }
    const LONGLONG outputTime = sourceTime - sourceBegin + outputBase;
    if (outputTime <= state->lastOutputTime ||
        (state->wroteAny && outputTime < state->lastOutputEnd)) {
        return MakeBoundaryStep(
            Mp4BoundaryTrimOutcome::Unsupported,
            MF_E_INVALID_TIMESTAMP,
            L"拼接后的 H.264 样本时间戳不连续或发生重叠。");
    }

    HRESULT result = sample->SetSampleTime(outputTime);
    if (SUCCEEDED(result)) {
        result = sample->SetSampleDuration(sourceDuration);
    }
    if (SUCCEEDED(result) && !state->wroteAny) {
        result = sample->SetUINT32(
            MFSampleExtension_Discontinuity,
            TRUE);
    }
    if (SUCCEEDED(result)) {
        result = writer->WriteSample(outputStream, sample);
    }
    if (FAILED(result)) {
        return MakeBoundaryStep(
            Mp4BoundaryTrimOutcome::Failed,
            result,
            MuxErrorText(L"写入边界 H.264 压缩样本失败", result));
    }
    state->lastOutputTime = outputTime;
    state->lastOutputEnd = outputTime + sourceDuration;
    state->wroteAny = true;
    return MakeBoundaryStep(Mp4BoundaryTrimOutcome::Succeeded, S_OK);
}

[[nodiscard]] BoundaryStepResult CopyCompressedRange(
    IMFSourceReader* reader,
    IMFSinkWriter* writer,
    const DWORD outputStream,
    const LONGLONG sourceBegin,
    const LONGLONG sourceEnd,
    const LONGLONG outputBase,
    const std::stop_token stopToken,
    SegmentWriteState* writeState,
    std::uint64_t* writtenSamples) {
    if (reader == nullptr || writer == nullptr || writeState == nullptr ||
        writtenSamples == nullptr || sourceBegin < 0 ||
        sourceEnd <= sourceBegin) {
        return MakeBoundaryStep(
            Mp4BoundaryTrimOutcome::Failed,
            E_INVALIDARG,
            L"压缩样本拼接区间无效。");
    }

    HRESULT result = SeekBoundaryReader(reader, sourceBegin);
    if (FAILED(result)) {
        return MakeBoundaryStep(
            Mp4BoundaryTrimOutcome::Failed,
            result,
            MuxErrorText(L"定位压缩样本拼接起点失败", result));
    }

    LONGLONG previousReadTime = -1;
    LONGLONG pendingTime = -1;
    LONGLONG pendingDeclaredDuration = 0;
    ComPtr<IMFSample> pendingSample;
    bool firstAccepted = true;
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
                MuxErrorText(L"读取待拼接 H.264 样本失败", readError));
        }
        if ((flags & (MF_SOURCE_READERF_CURRENTMEDIATYPECHANGED |
                      MF_SOURCE_READERF_NATIVEMEDIATYPECHANGED)) != 0) {
            return MakeBoundaryStep(
                Mp4BoundaryTrimOutcome::Unsupported,
                MF_E_INVALIDMEDIATYPE,
                L"拼接区间内的 H.264 媒体类型发生变化。");
        }
        if ((flags & MF_SOURCE_READERF_STREAMTICK) != 0) {
            return MakeBoundaryStep(
                Mp4BoundaryTrimOutcome::Unsupported,
                MF_E_INVALID_TIMESTAMP,
                L"拼接区间包含无法直通的 stream tick。" );
        }

        const bool endOfStream =
            (flags & MF_SOURCE_READERF_ENDOFSTREAM) != 0;
        if (!endOfStream && sample == nullptr) {
            continue;
        }
        if (!endOfStream) {
            BoundaryStepResult step = ValidateNoBFrameOrder(
                sample.Get(),
                sampleTime,
                &previousReadTime,
                true);
            if (!step.Succeeded()) {
                return step;
            }
            if (sampleTime < sourceBegin) {
                continue;
            }
        }

        if (endOfStream || sampleTime >= sourceEnd) {
            if (pendingSample == nullptr) {
                return MakeBoundaryStep(
                    Mp4BoundaryTrimOutcome::Unsupported,
                    MF_E_END_OF_STREAM,
                    L"拼接区间内没有可写入的 H.264 样本。");
            }
            if (endOfStream &&
                pendingDeclaredDuration > 0 &&
                pendingTime + pendingDeclaredDuration +
                        kMaximumSampleEndRoundingDrift <
                    sourceEnd) {
                return MakeBoundaryStep(
                    Mp4BoundaryTrimOutcome::Unsupported,
                    MF_E_END_OF_STREAM,
                    L"最后一个 H.264 样本未覆盖裁剪终点。");
            }
            BoundaryStepResult step = WritePendingSample(
                writer,
                outputStream,
                pendingSample.Get(),
                pendingTime,
                sourceEnd - pendingTime,
                sourceBegin,
                outputBase,
                writeState);
            if (!step.Succeeded()) {
                return step;
            }
            ++(*writtenSamples);
            return MakeBoundaryStep(Mp4BoundaryTrimOutcome::Succeeded, S_OK);
        }

        if (firstAccepted) {
            firstAccepted = false;
            if (sampleTime != sourceBegin || !IsCleanPoint(sample.Get())) {
                return MakeBoundaryStep(
                    Mp4BoundaryTrimOutcome::Unsupported,
                    MF_E_INVALID_TIMESTAMP,
                    L"拼接区间不是从精确的 H.264 CleanPoint 开始。");
            }
        }
        if (pendingSample != nullptr) {
            BoundaryStepResult step = WritePendingSample(
                writer,
                outputStream,
                pendingSample.Get(),
                pendingTime,
                sampleTime - pendingTime,
                sourceBegin,
                outputBase,
                writeState);
            if (!step.Succeeded()) {
                return step;
            }
            ++(*writtenSamples);
        }
        pendingTime = sampleTime;
        pendingDeclaredDuration = 0;
        static_cast<void>(sample->GetSampleDuration(
            &pendingDeclaredDuration));
        pendingSample = std::move(sample);
    }
}

}  // namespace

BoundaryStepResult RemuxBoundarySegments(
    const std::filesystem::path& sourcePath,
    const std::filesystem::path& temporaryPath,
    const std::filesystem::path& destinationPath,
    const BoundarySourcePlan& plan,
    const std::stop_token stopToken,
    BoundaryRemuxResult* output) {
    if (output == nullptr || plan.nativeType == nullptr) {
        return MakeBoundaryStep(
            Mp4BoundaryTrimOutcome::Failed,
            E_INVALIDARG,
            L"边界重封装参数无效。");
    }

    ComPtr<IMFSourceReader> sourceReader;
    ComPtr<IMFMediaType> sourceType;
    BoundaryStepResult step = OpenNativeH264Source(
        sourcePath,
        &sourceReader,
        &sourceType);
    if (!step.Succeeded()) {
        return step;
    }

    ComPtr<IMFSourceReader> boundaryReader;
    ComPtr<IMFMediaType> boundaryType;
    if (plan.encodeBoundary) {
        step = OpenNativeH264Source(
            temporaryPath,
            &boundaryReader,
            &boundaryType);
        if (!step.Succeeded()) {
            return step;
        }
        step = CompareBoundaryNativeTypes(
            sourceType.Get(),
            boundaryType.Get());
        if (!step.Succeeded()) {
            return step;
        }
    }

    ComPtr<IMFSinkWriter> writer;
    DWORD outputStream = 0;
    step = CreateCompressedWriter(
        destinationPath,
        sourceType.Get(),
        &writer,
        &outputStream);
    if (!step.Succeeded()) {
        return step;
    }

    SegmentWriteState writeState{};
    std::uint64_t boundarySamples = 0;
    BoundaryRemuxResult remuxed{};
    if (plan.encodeBoundary) {
        step = CopyCompressedRange(
            boundaryReader.Get(),
            writer.Get(),
            outputStream,
            0,
            plan.spliceTime - plan.visibleStart,
            0,
            stopToken,
            &writeState,
            &boundarySamples);
        if (!step.Succeeded()) {
            return step;
        }
        if (boundarySamples != plan.encodedFrames) {
            return MakeBoundaryStep(
                Mp4BoundaryTrimOutcome::Unsupported,
                MF_E_INVALID_TIMESTAMP,
                L"边界临时 H.264 样本数与编码帧数不一致。" );
        }
    }

    if (plan.spliceTime < plan.requestedEnd) {
        step = CopyCompressedRange(
            sourceReader.Get(),
            writer.Get(),
            outputStream,
            plan.spliceTime,
            plan.requestedEnd,
            plan.spliceTime - plan.visibleStart,
            stopToken,
            &writeState,
            &remuxed.passthroughSamples);
        if (!step.Succeeded()) {
            return step;
        }
    } else if (!plan.encodeBoundary) {
        return MakeBoundaryStep(
            Mp4BoundaryTrimOutcome::Unsupported,
            MF_E_END_OF_STREAM,
            L"裁剪区间没有可输出的视频样本。");
    }

    if (!writeState.wroteAny ||
        writeState.lastOutputEnd !=
            plan.requestedEnd - plan.visibleStart) {
        return MakeBoundaryStep(
            Mp4BoundaryTrimOutcome::Unsupported,
            MF_E_INVALID_TIMESTAMP,
            L"重封装后的 H.264 时间线没有精确覆盖裁剪区间。");
    }
    const HRESULT finalizeResult = writer->Finalize();
    writer.Reset();
    if (FAILED(finalizeResult)) {
        return MakeBoundaryStep(
            Mp4BoundaryTrimOutcome::Failed,
            finalizeResult,
            MuxErrorText(L"完成边界 MP4 重封装失败", finalizeResult));
    }

    *output = remuxed;
    return MakeBoundaryStep(Mp4BoundaryTrimOutcome::Succeeded, S_OK);
}

}  // namespace qrec::detail
