#include "media/Mp4BoundaryInternal.h"

#include <mfapi.h>
#include <mferror.h>

#include <cstdint>
#include <string>
#include <vector>

namespace qrec::detail {
namespace {

constexpr DWORD kVideoStream =
    static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM);
// Source timestamps and the temporary MP4 each cross an MP4-timescale/100 ns
// Media Foundation conversion. On the supported 30/60 FPS grids, the two
// independent integer endpoint conversions can lose at most two 100 ns ticks.
constexpr LONGLONG kMaximumSampleEndRoundingDrift = 2;

[[nodiscard]] BoundaryStepResult Incompatible(const wchar_t* field) {
    return MakeBoundaryStep(
        Mp4BoundaryTrimOutcome::Unsupported,
        MF_E_INVALIDMEDIATYPE,
        L"边界临时 MP4 与源 H.264 的 " + std::wstring(field) +
            L" 不一致，拒绝混合压缩码流。");
}

[[nodiscard]] bool EqualRequiredGuid(
    IMFAttributes* left,
    IMFAttributes* right,
    const GUID& key) noexcept {
    GUID leftValue{};
    GUID rightValue{};
    return left != nullptr && right != nullptr &&
        SUCCEEDED(left->GetGUID(key, &leftValue)) &&
        SUCCEEDED(right->GetGUID(key, &rightValue)) &&
        leftValue == rightValue;
}

[[nodiscard]] bool EqualRequiredSize(
    IMFAttributes* left,
    IMFAttributes* right,
    const GUID& key) noexcept {
    UINT32 leftWidth = 0;
    UINT32 leftHeight = 0;
    UINT32 rightWidth = 0;
    UINT32 rightHeight = 0;
    return left != nullptr && right != nullptr &&
        SUCCEEDED(::MFGetAttributeSize(
            left,
            key,
            &leftWidth,
            &leftHeight)) &&
        SUCCEEDED(::MFGetAttributeSize(
            right,
            key,
            &rightWidth,
            &rightHeight)) &&
        leftWidth == rightWidth && leftHeight == rightHeight;
}

[[nodiscard]] bool EqualRequiredRatio(
    IMFAttributes* left,
    IMFAttributes* right,
    const GUID& key) noexcept {
    UINT32 leftNumerator = 0;
    UINT32 leftDenominator = 0;
    UINT32 rightNumerator = 0;
    UINT32 rightDenominator = 0;
    if (left == nullptr || right == nullptr ||
        FAILED(::MFGetAttributeRatio(
            left,
            key,
            &leftNumerator,
            &leftDenominator)) ||
        FAILED(::MFGetAttributeRatio(
            right,
            key,
            &rightNumerator,
            &rightDenominator)) ||
        leftDenominator == 0 || rightDenominator == 0) {
        return false;
    }
    return static_cast<std::uint64_t>(leftNumerator) * rightDenominator ==
        static_cast<std::uint64_t>(rightNumerator) * leftDenominator;
}

[[nodiscard]] bool EqualOptionalUint32(
    IMFAttributes* left,
    IMFAttributes* right,
    const GUID& key) noexcept {
    if (left == nullptr || right == nullptr) {
        return false;
    }
    UINT32 leftValue = 0;
    UINT32 rightValue = 0;
    const HRESULT leftResult = left->GetUINT32(key, &leftValue);
    const HRESULT rightResult = right->GetUINT32(key, &rightValue);
    if (leftResult == MF_E_ATTRIBUTENOTFOUND &&
        rightResult == MF_E_ATTRIBUTENOTFOUND) {
        return true;
    }
    return SUCCEEDED(leftResult) && SUCCEEDED(rightResult) &&
        leftValue == rightValue;
}

[[nodiscard]] bool ReadBlob(
    IMFAttributes* attributes,
    const GUID& key,
    std::vector<UINT8>* output,
    bool* found) {
    if (attributes == nullptr || output == nullptr || found == nullptr) {
        return false;
    }
    output->clear();
    *found = false;
    UINT32 size = 0;
    const HRESULT sizeResult = attributes->GetBlobSize(key, &size);
    if (sizeResult == MF_E_ATTRIBUTENOTFOUND) {
        return true;
    }
    if (FAILED(sizeResult) || size == 0) {
        return false;
    }
    output->resize(size);
    UINT32 written = 0;
    if (FAILED(attributes->GetBlob(
            key,
            output->data(),
            size,
            &written)) ||
        written != size) {
        return false;
    }
    *found = true;
    return true;
}

[[nodiscard]] bool EqualBlob(
    IMFAttributes* left,
    IMFAttributes* right,
    const GUID& key,
    const bool required) {
    std::vector<UINT8> leftBytes;
    std::vector<UINT8> rightBytes;
    bool leftFound = false;
    bool rightFound = false;
    return ReadBlob(left, key, &leftBytes, &leftFound) &&
        ReadBlob(right, key, &rightBytes, &rightFound) &&
        leftFound == rightFound && (!required || leftFound) &&
        leftBytes == rightBytes;
}

[[nodiscard]] BoundaryStepResult ValidateBoundarySamples(
    IMFSourceReader* reader,
    const BoundarySourcePlan& plan) {
    if (reader == nullptr || plan.encodedFrames == 0 ||
        plan.spliceTime <= plan.visibleStart) {
        return MakeBoundaryStep(
            Mp4BoundaryTrimOutcome::Failed,
            E_INVALIDARG,
            L"边界临时 H.264 样本门禁参数无效。");
    }
    HRESULT result = SeekBoundaryReader(reader, 0);
    if (FAILED(result)) {
        return MakeBoundaryStep(
            Mp4BoundaryTrimOutcome::Failed,
            result,
            L"无法定位边界临时 H.264 的首样本。");
    }

    const LONGLONG expectedDuration =
        plan.spliceTime - plan.visibleStart;
    LONGLONG previousTime = -1;
    LONGLONG lastEnd = 0;
    std::uint64_t sampleCount = 0;
    for (;;) {
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
            return MakeBoundaryStep(
                Mp4BoundaryTrimOutcome::Failed,
                FAILED(result) ? result : E_FAIL,
                L"读取边界临时 H.264 样本失败。");
        }
        if ((flags & (MF_SOURCE_READERF_CURRENTMEDIATYPECHANGED |
                      MF_SOURCE_READERF_NATIVEMEDIATYPECHANGED)) != 0 ||
            (flags & MF_SOURCE_READERF_STREAMTICK) != 0) {
            return MakeBoundaryStep(
                Mp4BoundaryTrimOutcome::Unsupported,
                MF_E_INVALIDMEDIATYPE,
                L"边界临时 H.264 在样本区间内改变了媒体状态。");
        }
        if ((flags & MF_SOURCE_READERF_ENDOFSTREAM) != 0) {
            break;
        }
        if (sample == nullptr) {
            continue;
        }

        BoundaryStepResult step = ValidateNoBFrameOrder(
            sample.Get(),
            sampleTime,
            &previousTime,
            false);
        if (!step.Succeeded()) {
            return step;
        }
        if (sampleCount == 0 &&
            (sampleTime != 0 || !IsCleanPoint(sample.Get()))) {
            return MakeBoundaryStep(
                Mp4BoundaryTrimOutcome::Unsupported,
                MF_E_INVALID_TIMESTAMP,
                L"边界临时 H.264 的首样本不是时间零 IDR/CleanPoint。" );
        }
        if (sampleTime >= expectedDuration) {
            return MakeBoundaryStep(
                Mp4BoundaryTrimOutcome::Unsupported,
                MF_E_INVALID_TIMESTAMP,
                L"边界临时 H.264 样本越过了拼接点。" );
        }
        LONGLONG sampleDuration = 0;
        if (FAILED(sample->GetSampleDuration(&sampleDuration)) ||
            sampleDuration <= 0) {
            sampleDuration = plan.nominalFrameDuration;
        }
        lastEnd = sampleTime + sampleDuration;
        ++sampleCount;
    }

    const LONGLONG endDifference = lastEnd >= expectedDuration
        ? lastEnd - expectedDuration
        : expectedDuration - lastEnd;
    if (sampleCount != plan.encodedFrames ||
        endDifference > kMaximumSampleEndRoundingDrift) {
        return MakeBoundaryStep(
            Mp4BoundaryTrimOutcome::Unsupported,
            MF_E_INVALID_TIMESTAMP,
            L"边界临时 H.264 的帧数或结束时间与编码计划不一致（实际样本 " +
                std::to_wstring(sampleCount) + L"，计划样本 " +
                std::to_wstring(plan.encodedFrames) + L"，实际结束 " +
                std::to_wstring(lastEnd) + L"，计划结束 " +
                std::to_wstring(expectedDuration) + L"，差值 " +
                std::to_wstring(endDifference) + L" 个 100ns tick）。" );
    }
    return MakeBoundaryStep(Mp4BoundaryTrimOutcome::Succeeded, S_OK);
}

}  // namespace

BoundaryStepResult CompareBoundaryNativeTypes(
    IMFMediaType* sourceType,
    IMFMediaType* boundaryType) {
    if (!EqualRequiredGuid(
            sourceType,
            boundaryType,
            MF_MT_MAJOR_TYPE)) {
        return Incompatible(L"MF_MT_MAJOR_TYPE");
    }
    if (!EqualRequiredGuid(
            sourceType,
            boundaryType,
            MF_MT_SUBTYPE)) {
        return Incompatible(L"MF_MT_SUBTYPE");
    }
    if (!EqualRequiredSize(
            sourceType,
            boundaryType,
            MF_MT_FRAME_SIZE)) {
        return Incompatible(L"MF_MT_FRAME_SIZE");
    }
    if (!EqualRequiredRatio(
            sourceType,
            boundaryType,
            MF_MT_FRAME_RATE)) {
        return Incompatible(L"MF_MT_FRAME_RATE");
    }
    if (!EqualRequiredRatio(
            sourceType,
            boundaryType,
            MF_MT_PIXEL_ASPECT_RATIO)) {
        return Incompatible(L"MF_MT_PIXEL_ASPECT_RATIO");
    }

    struct Uint32Field final {
        const GUID* key;
        const wchar_t* name;
    };
    const Uint32Field fields[]{
        {&MF_MT_INTERLACE_MODE, L"MF_MT_INTERLACE_MODE"},
        {&MF_MT_MPEG2_PROFILE, L"MF_MT_MPEG2_PROFILE"},
        {&MF_MT_MPEG2_LEVEL, L"MF_MT_MPEG2_LEVEL"},
        {&MF_MT_MPEG4_CURRENT_SAMPLE_ENTRY,
         L"MF_MT_MPEG4_CURRENT_SAMPLE_ENTRY"},
        {&MF_MT_VIDEO_PRIMARIES, L"MF_MT_VIDEO_PRIMARIES"},
        {&MF_MT_TRANSFER_FUNCTION, L"MF_MT_TRANSFER_FUNCTION"},
        {&MF_MT_YUV_MATRIX, L"MF_MT_YUV_MATRIX"},
        {&MF_MT_VIDEO_NOMINAL_RANGE, L"MF_MT_VIDEO_NOMINAL_RANGE"},
        {&MF_MT_VIDEO_CHROMA_SITING, L"MF_MT_VIDEO_CHROMA_SITING"},
    };
    for (const Uint32Field& field : fields) {
        if (!EqualOptionalUint32(
                sourceType,
                boundaryType,
                *field.key)) {
            return Incompatible(field.name);
        }
    }
    if (!EqualBlob(
            sourceType,
            boundaryType,
            MF_MT_MPEG_SEQUENCE_HEADER,
            true)) {
        return Incompatible(L"MF_MT_MPEG_SEQUENCE_HEADER (avcC)");
    }
    if (!EqualBlob(
            sourceType,
            boundaryType,
            MF_MT_MPEG4_SAMPLE_DESCRIPTION,
            false)) {
        return Incompatible(L"MF_MT_MPEG4_SAMPLE_DESCRIPTION");
    }
    return MakeBoundaryStep(Mp4BoundaryTrimOutcome::Succeeded, S_OK);
}

BoundaryStepResult ValidateBoundaryCompatibility(
    const std::filesystem::path& temporaryPath,
    const BoundarySourcePlan& plan) {
    if (!plan.encodeBoundary || plan.nativeType == nullptr) {
        return MakeBoundaryStep(
            Mp4BoundaryTrimOutcome::Failed,
            E_INVALIDARG,
            L"边界媒体兼容检查参数无效。");
    }
    ComPtr<IMFSourceReader> boundaryReader;
    ComPtr<IMFMediaType> boundaryType;
    BoundaryStepResult step = OpenNativeH264Source(
        temporaryPath,
        &boundaryReader,
        &boundaryType);
    if (!step.Succeeded()) {
        return step;
    }
    step = CompareBoundaryNativeTypes(
        plan.nativeType.Get(),
        boundaryType.Get());
    if (!step.Succeeded()) {
        return step;
    }
    return ValidateBoundarySamples(boundaryReader.Get(), plan);
}

}  // namespace qrec::detail
