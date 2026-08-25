#include "media/Mp4BoundaryInternal.h"

#include "common/Win32Helpers.h"
#include "media/Mp4Writer.h"

#include <mfapi.h>
#include <mferror.h>
#include <mfreadwrite.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <memory>
#include <system_error>
#include <utility>
#include <vector>

namespace qrec::detail {
namespace {

constexpr DWORD kAllStreams =
    static_cast<DWORD>(MF_SOURCE_READER_ALL_STREAMS);
constexpr DWORD kVideoStream =
    static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM);
constexpr LONGLONG kMaximumBoundaryRoundingGap = 1;
// H.264 stores pictures in coded macroblock dimensions. Media Foundation can
// initially expose the visible frame size and then report the macroblock-
// aligned coded size with the first decoded sample. Both right and bottom
// padding are valid as long as they are smaller than one 16-pixel macroblock.
constexpr UINT32 kMaximumCodedPaddingPixels = 15;

struct DecodedBoundarySource final {
    ComPtr<IMFSourceReader> reader;
    ComPtr<IMFMediaType> mediaType;
    UINT32 visibleWidth{};
    UINT32 visibleHeight{};
    UINT32 codedWidth{};
    UINT32 codedHeight{};
    LONG stride{};
};

[[nodiscard]] bool IsCompatibleCodedGeometry(
    const UINT32 codedWidth,
    const UINT32 codedHeight,
    const BoundarySourcePlan& plan) noexcept {
    return codedWidth >= plan.width && codedHeight >= plan.height &&
        codedWidth - plan.width <= kMaximumCodedPaddingPixels &&
        codedHeight - plan.height <= kMaximumCodedPaddingPixels;
}

class EncodedPathCleanup final {
public:
    ~EncodedPathCleanup() noexcept {
        if (path_.empty()) {
            return;
        }
        std::error_code ignored;
        static_cast<void>(std::filesystem::remove(path_, ignored));
    }

    void Set(std::filesystem::path path) {
        path_ = std::move(path);
    }

    void Release() noexcept {
        path_.clear();
    }

private:
    std::filesystem::path path_;
};

[[nodiscard]] std::wstring EncoderErrorText(
    const std::wstring_view operation,
    const HRESULT result) {
    return std::wstring(operation) + L"：" + win32::FormatError(result);
}

[[nodiscard]] BoundaryStepResult CreateDecodedBoundarySource(
    const std::filesystem::path& sourcePath,
    const BoundarySourcePlan& plan,
    DecodedBoundarySource* output) {
    if (output == nullptr) {
        return MakeBoundaryStep(
            Mp4BoundaryTrimOutcome::Failed,
            E_POINTER,
            L"创建边界解码器时收到空输出指针。");
    }

    ComPtr<IMFAttributes> attributes;
    HRESULT result = ::MFCreateAttributes(&attributes, 4);
    if (SUCCEEDED(result)) {
        result = attributes->SetUINT32(
            MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING,
            TRUE);
    }
    if (SUCCEEDED(result)) {
        result = attributes->SetUINT32(
            MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS,
            TRUE);
    }
    if (SUCCEEDED(result)) {
        result = attributes->SetUINT32(
            MF_SOURCE_READER_DISABLE_DXVA,
            FALSE);
    }

    DecodedBoundarySource decoded{};
    if (SUCCEEDED(result)) {
        result = ::MFCreateSourceReaderFromURL(
            sourcePath.c_str(),
            attributes.Get(),
            &decoded.reader);
    }
    if (FAILED(result)) {
        return MakeBoundaryStep(
            Mp4BoundaryTrimOutcome::Failed,
            result,
            EncoderErrorText(L"创建边界帧解码器失败", result));
    }

    static_cast<void>(decoded.reader->SetStreamSelection(kAllStreams, FALSE));
    result = decoded.reader->SetStreamSelection(kVideoStream, TRUE);
    ComPtr<IMFMediaType> requestedType;
    if (SUCCEEDED(result)) {
        result = ::MFCreateMediaType(&requestedType);
    }
    if (SUCCEEDED(result)) {
        result = requestedType->SetGUID(
            MF_MT_MAJOR_TYPE,
            MFMediaType_Video);
    }
    if (SUCCEEDED(result)) {
        result = requestedType->SetGUID(
            MF_MT_SUBTYPE,
            MFVideoFormat_RGB32);
    }
    if (SUCCEEDED(result)) {
        result = requestedType->SetUINT32(
            MF_MT_INTERLACE_MODE,
            MFVideoInterlace_Progressive);
    }
    if (SUCCEEDED(result)) {
        result = decoded.reader->SetCurrentMediaType(
            kVideoStream,
            nullptr,
            requestedType.Get());
    }
    if (SUCCEEDED(result)) {
        result = decoded.reader->GetCurrentMediaType(
            kVideoStream,
            &decoded.mediaType);
    }
    if (SUCCEEDED(result)) {
        result = ::MFGetAttributeSize(
            decoded.mediaType.Get(),
            MF_MT_FRAME_SIZE,
            &decoded.codedWidth,
            &decoded.codedHeight);
    }
    if (SUCCEEDED(result) &&
        !IsCompatibleCodedGeometry(
            decoded.codedWidth,
            decoded.codedHeight,
            plan)) {
        result = MF_E_INVALIDMEDIATYPE;
    }
    if (SUCCEEDED(result)) {
        UINT32 encodedStride = 0;
        if (SUCCEEDED(decoded.mediaType->GetUINT32(
                MF_MT_DEFAULT_STRIDE,
                &encodedStride))) {
            decoded.stride = static_cast<LONG>(encodedStride);
        } else {
            result = ::MFGetStrideForBitmapInfoHeader(
                MFVideoFormat_RGB32.Data1,
                decoded.codedWidth,
                &decoded.stride);
        }
    }
    if (FAILED(result)) {
        return MakeBoundaryStep(
            Mp4BoundaryTrimOutcome::Unsupported,
            result,
            L"源视频无法协商为尺寸一致的 RGB32 边界帧。");
    }
    decoded.visibleWidth = plan.width;
    decoded.visibleHeight = plan.height;
    *output = std::move(decoded);
    return MakeBoundaryStep(Mp4BoundaryTrimOutcome::Succeeded, S_OK);
}

[[nodiscard]] HRESULT ExtractTopDownBgra(
    IMFSample* sample,
    const DecodedBoundarySource& source,
    std::vector<std::uint8_t>* pixels) {
    if (sample == nullptr || pixels == nullptr) {
        return E_POINTER;
    }
    const std::size_t outputStride =
        static_cast<std::size_t>(source.visibleWidth) * 4U;
    if (source.visibleWidth == 0 || source.visibleHeight == 0 ||
        source.codedHeight == 0 ||
        source.visibleHeight > source.codedHeight ||
        source.visibleHeight >
        std::numeric_limits<std::size_t>::max() / outputStride) {
        return E_OUTOFMEMORY;
    }
    pixels->resize(outputStride * source.visibleHeight);

    ComPtr<IMFMediaBuffer> buffer;
    DWORD bufferCount = 0;
    HRESULT result = sample->GetBufferCount(&bufferCount);
    if (SUCCEEDED(result) && bufferCount == 1) {
        result = sample->GetBufferByIndex(0, &buffer);
    } else if (SUCCEEDED(result)) {
        result = sample->ConvertToContiguousBuffer(&buffer);
    }
    if (FAILED(result)) {
        return result;
    }

    BYTE* sourceScanline = nullptr;
    LONG sourceStride = source.stride;
    ComPtr<IMF2DBuffer> twoDimensional;
    if (SUCCEEDED(buffer.As(&twoDimensional))) {
        result = twoDimensional->Lock2D(&sourceScanline, &sourceStride);
        if (FAILED(result)) {
            return result;
        }
        const auto actualStride = static_cast<std::uint64_t>(
            std::llabs(static_cast<long long>(sourceStride)));
        if (sourceScanline == nullptr || actualStride < outputStride) {
            static_cast<void>(twoDimensional->Unlock2D());
            return MF_E_BUFFERTOOSMALL;
        }
        for (std::uint32_t row = 0; row < source.visibleHeight; ++row) {
            auto* destination = pixels->data() +
                static_cast<std::size_t>(row) * outputStride;
            const auto* input = sourceScanline +
                static_cast<std::ptrdiff_t>(row) * sourceStride;
            std::memcpy(destination, input, outputStride);
            for (std::size_t alpha = 3; alpha < outputStride; alpha += 4) {
                destination[alpha] = 0xFF;
            }
        }
        static_cast<void>(twoDimensional->Unlock2D());
        return S_OK;
    }

    DWORD maximumLength = 0;
    DWORD currentLength = 0;
    result = buffer->Lock(
        &sourceScanline,
        &maximumLength,
        &currentLength);
    static_cast<void>(maximumLength);
    if (FAILED(result)) {
        return result;
    }
    const std::size_t declaredStride = static_cast<std::size_t>(
        std::llabs(static_cast<long long>(sourceStride)));
    std::size_t actualStride = declaredStride;
    if (currentLength % source.codedHeight == 0) {
        const std::size_t inferredStride = currentLength / source.codedHeight;
        if (inferredStride >= outputStride) {
            actualStride = inferredStride;
        }
    }
    const bool bottomUp = sourceStride < 0;
    const std::size_t lastSourceRow = bottomUp
        ? static_cast<std::size_t>(source.codedHeight - 1U)
        : static_cast<std::size_t>(source.visibleHeight - 1U);
    const std::size_t required = actualStride * lastSourceRow + outputStride;
    if (sourceScanline == nullptr || currentLength < required) {
        static_cast<void>(buffer->Unlock());
        return MF_E_BUFFERTOOSMALL;
    }
    for (std::uint32_t row = 0; row < source.visibleHeight; ++row) {
        auto* destination = pixels->data() +
            static_cast<std::size_t>(row) * outputStride;
        const std::uint32_t sourceRow = bottomUp
            ? source.codedHeight - 1U - row
            : row;
        const auto* input = sourceScanline +
            static_cast<std::size_t>(sourceRow) * actualStride;
        std::memcpy(destination, input, outputStride);
        for (std::size_t alpha = 3; alpha < outputStride; alpha += 4) {
            destination[alpha] = 0xFF;
        }
    }
    static_cast<void>(buffer->Unlock());
    return S_OK;
}

}  // namespace

BoundaryStepResult EncodeBoundarySegment(
    const std::filesystem::path& sourcePath,
    const std::filesystem::path& temporaryPath,
    const BoundarySourcePlan& plan,
    const std::stop_token stopToken,
    BoundaryEncodeResult* output) {
    if (output == nullptr || !plan.encodeBoundary ||
        plan.spliceTime <= plan.visibleStart) {
        return MakeBoundaryStep(
            Mp4BoundaryTrimOutcome::Failed,
            E_INVALIDARG,
            L"边界重编码计划无效。");
    }

    const std::optional<Mp4BoundaryEncoderKey> encoderKey =
        Mp4BoundaryEncoderPool::MakeKey(
            sourcePath,
            plan.width,
            plan.height,
            plan.framesPerSecond,
            plan.averageBitrate);
    if (!encoderKey.has_value()) {
        return MakeBoundaryStep(
            Mp4BoundaryTrimOutcome::Unsupported,
            E_INVALIDARG,
            L"边界重编码参数无法建立预热配置。");
    }

    BoundaryEncodeResult encoded{};
    encoded.encoderKey = *encoderKey;
    Mp4BoundaryEncoderLease prewarmedLease;
    Mp4BoundaryEncoderAcquireStats acquisitionStats{};
    const Mp4BoundaryEncoderAcquireOutcome acquisition =
        Mp4BoundaryEncoderPool::Shared().TryAcquire(
            *encoderKey,
            stopToken,
            &prewarmedLease,
            &acquisitionStats);
    if (acquisition == Mp4BoundaryEncoderAcquireOutcome::Cancelled) {
        return MakeBoundaryStep(
            Mp4BoundaryTrimOutcome::Cancelled,
            HRESULT_FROM_WIN32(ERROR_CANCELLED));
    }
    encoded.encoderGeneration = acquisitionStats.generation;
    encoded.usedPrewarmedEncoder =
        acquisitionStats.usedPrewarmedEncoder;
    encoded.encoderPrepareWait = acquisitionStats.prepareWait;
    encoded.encoderOpen = acquisitionStats.encoderOpen;

    std::unique_ptr<media::Mp4Writer> localWriter;
    media::Mp4Writer* writer = prewarmedLease.Writer();
    std::wstring writerError;
    long writerNativeError = 0;
    if (acquisition != Mp4BoundaryEncoderAcquireOutcome::Acquired ||
        writer == nullptr) {
        localWriter = std::make_unique<media::Mp4Writer>();
        media::Mp4WriterConfig config{};
        config.outputPath = temporaryPath;
        config.width = plan.width;
        config.height = plan.height;
        config.framesPerSecond = plan.framesPerSecond;
        config.averageBitrate = plan.averageBitrate;
        config.preferHardwareEncoder = true;
        const auto openStarted = std::chrono::steady_clock::now();
        const bool opened = localWriter->Open(
            config,
            writerError,
            writerNativeError);
        encoded.encoderOpen =
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - openStarted);
        if (!opened) {
            *output = encoded;
            return MakeBoundaryStep(
                Mp4BoundaryTrimOutcome::Failed,
                static_cast<HRESULT>(writerNativeError),
                std::move(writerError));
        }
        writer = localWriter.get();
    }
    *output = encoded;

    DecodedBoundarySource source{};
    BoundaryStepResult step = CreateDecodedBoundarySource(
        sourcePath,
        plan,
        &source);
    if (!step.Succeeded()) {
        return step;
    }

    HRESULT result = SeekBoundaryReader(
        source.reader.Get(),
        plan.visibleStart);
    if (FAILED(result)) {
        return MakeBoundaryStep(
            Mp4BoundaryTrimOutcome::Failed,
            result,
            EncoderErrorText(L"定位边界解码起点失败", result));
    }

    const LONGLONG targetDuration = plan.spliceTime - plan.visibleStart;
    LONGLONG lastInputTime = -1;
    LONGLONG lastOutputEnd = 0;
    bool firstFrame = true;
    std::vector<std::uint8_t> pixels;
    for (;;) {
        if (stopToken.stop_requested()) {
            return MakeBoundaryStep(
                Mp4BoundaryTrimOutcome::Cancelled,
                HRESULT_FROM_WIN32(ERROR_CANCELLED));
        }
        DWORD actualStream = 0;
        DWORD flags = 0;
        LONGLONG frameTime = 0;
        ComPtr<IMFSample> sample;
        result = source.reader->ReadSample(
            kVideoStream,
            0,
            &actualStream,
            &flags,
            &frameTime,
            &sample);
        static_cast<void>(actualStream);
        if (FAILED(result) || (flags & MF_SOURCE_READERF_ERROR) != 0) {
            const HRESULT readError = FAILED(result) ? result : E_FAIL;
            return MakeBoundaryStep(
                Mp4BoundaryTrimOutcome::Failed,
                readError,
                EncoderErrorText(L"解码边界视频帧失败", readError));
        }
        if ((flags & MF_SOURCE_READERF_NATIVEMEDIATYPECHANGED) != 0 ||
            ((flags & MF_SOURCE_READERF_CURRENTMEDIATYPECHANGED) != 0 &&
             !firstFrame)) {
            return MakeBoundaryStep(
                Mp4BoundaryTrimOutcome::Unsupported,
                MF_E_INVALIDMEDIATYPE,
                L"边界解码期间媒体类型发生变化。");
        }
        if ((flags & MF_SOURCE_READERF_CURRENTMEDIATYPECHANGED) != 0) {
            ComPtr<IMFMediaType> changedType;
            UINT32 changedWidth = 0;
            UINT32 changedHeight = 0;
            GUID changedSubtype{};
            result = source.reader->GetCurrentMediaType(
                kVideoStream,
                &changedType);
            if (SUCCEEDED(result)) {
                result = changedType->GetGUID(
                    MF_MT_SUBTYPE,
                    &changedSubtype);
            }
            if (SUCCEEDED(result)) {
                result = ::MFGetAttributeSize(
                    changedType.Get(),
                    MF_MT_FRAME_SIZE,
                    &changedWidth,
                    &changedHeight);
            }
            if (SUCCEEDED(result)) {
                UINT32 changedStride = 0;
                if (SUCCEEDED(changedType->GetUINT32(
                        MF_MT_DEFAULT_STRIDE,
                        &changedStride))) {
                    source.stride = static_cast<LONG>(changedStride);
                } else {
                    result = ::MFGetStrideForBitmapInfoHeader(
                        MFVideoFormat_RGB32.Data1,
                        changedWidth,
                        &source.stride);
                }
            }
            if (FAILED(result) || changedSubtype != MFVideoFormat_RGB32 ||
                !IsCompatibleCodedGeometry(
                    changedWidth,
                    changedHeight,
                    plan)) {
                return MakeBoundaryStep(
                    Mp4BoundaryTrimOutcome::Unsupported,
                    FAILED(result) ? result : MF_E_INVALIDMEDIATYPE,
                    L"边界解码器的初始输出类型与编码计划不一致（实际 " +
                        std::to_wstring(changedWidth) + L"x" +
                        std::to_wstring(changedHeight) +
                        L"，计划 " + std::to_wstring(plan.width) +
                        L"x" + std::to_wstring(plan.height) +
                        L"，RGB32=" +
                        (changedSubtype == MFVideoFormat_RGB32
                             ? std::wstring(L"true")
                             : std::wstring(L"false")) +
                        L"）。" );
            }
            source.mediaType = std::move(changedType);
            source.codedWidth = changedWidth;
            source.codedHeight = changedHeight;
        }
        if ((flags & MF_SOURCE_READERF_STREAMTICK) != 0) {
            return MakeBoundaryStep(
                Mp4BoundaryTrimOutcome::Unsupported,
                MF_E_INVALID_TIMESTAMP,
                L"边界解码区间包含无法表示的 stream tick。" );
        }
        if ((flags & MF_SOURCE_READERF_ENDOFSTREAM) != 0) {
            break;
        }
        if (sample == nullptr || frameTime < plan.visibleStart) {
            continue;
        }
        if (frameTime >= plan.spliceTime) {
            break;
        }
        if (firstFrame && frameTime != plan.visibleStart) {
            return MakeBoundaryStep(
                Mp4BoundaryTrimOutcome::Unsupported,
                MF_E_INVALID_TIMESTAMP,
                L"解码器未从计划的首个可见帧开始输出。");
        }
        firstFrame = false;
        if (lastInputTime >= 0 && frameTime <= lastInputTime) {
            return MakeBoundaryStep(
                Mp4BoundaryTrimOutcome::Unsupported,
                MF_E_INVALID_TIMESTAMP,
                L"边界解码帧时间戳不严格递增。");
        }
        lastInputTime = frameTime;

        LONGLONG sampleDuration = 0;
        if (FAILED(sample->GetSampleDuration(&sampleDuration)) ||
            sampleDuration <= 0) {
            sampleDuration = plan.nominalFrameDuration;
        }
        const LONGLONG remainingDuration = plan.spliceTime - frameTime;
        if (remainingDuration >= sampleDuration &&
            remainingDuration - sampleDuration <=
                kMaximumBoundaryRoundingGap) {
            sampleDuration = remainingDuration;
        } else {
            sampleDuration = std::min(
                sampleDuration,
                remainingDuration);
        }
        if (sampleDuration <= 0) {
            break;
        }

        result = ExtractTopDownBgra(sample.Get(), source, &pixels);
        if (FAILED(result)) {
            return MakeBoundaryStep(
                Mp4BoundaryTrimOutcome::Failed,
                result,
                EncoderErrorText(L"规范化边界 BGRA 帧失败", result));
        }
        const LONGLONG outputTime = frameTime - plan.visibleStart;
        if (!writer->WriteBgraFrame(
                pixels,
                plan.width * 4U,
                outputTime,
                sampleDuration,
                writerError,
                writerNativeError)) {
            return MakeBoundaryStep(
                Mp4BoundaryTrimOutcome::Failed,
                static_cast<HRESULT>(writerNativeError),
                std::move(writerError));
        }
        lastOutputEnd = outputTime + sampleDuration;
        ++encoded.encodedFrames;
    }

    if (encoded.encodedFrames == 0 || lastOutputEnd != targetDuration) {
        return MakeBoundaryStep(
            Mp4BoundaryTrimOutcome::Unsupported,
            MF_E_END_OF_STREAM,
            L"边界解码帧未完整覆盖到下一个 CleanPoint。");
    }
    if (!writer->Finalize(writerError, writerNativeError)) {
        *output = encoded;
        return MakeBoundaryStep(
            Mp4BoundaryTrimOutcome::Failed,
            static_cast<HRESULT>(writerNativeError),
            std::move(writerError));
    }
    EncodedPathCleanup pooledPathCleanup;
    if (acquisition == Mp4BoundaryEncoderAcquireOutcome::Acquired) {
        encoded.actualPath = prewarmedLease.TakeFinalizedPath();
        if (encoded.actualPath.empty()) {
            *output = encoded;
            return MakeBoundaryStep(
                Mp4BoundaryTrimOutcome::Failed,
                E_UNEXPECTED,
                L"预热边界编码器未能移交成片路径。");
        }
        pooledPathCleanup.Set(encoded.actualPath);
        prewarmedLease = Mp4BoundaryEncoderLease{};
    } else {
        localWriter.reset();
        encoded.actualPath = temporaryPath;
    }
    encoded.duration = targetDuration;
    *output = encoded;
    pooledPathCleanup.Release();
    return MakeBoundaryStep(Mp4BoundaryTrimOutcome::Succeeded, S_OK);
}

}  // namespace qrec::detail
