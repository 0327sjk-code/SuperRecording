#include "media/MediaExporter.h"

#include "media/AudioVideoMuxer.h"

#include "common/Win32Helpers.h"
#include "media/Mp4BoundaryTrimmer.h"
#include "media/Mp4Writer.h"

#include <codecapi.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <propvarutil.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cwctype>
#include <cstring>
#include <exception>
#include <format>
#include <limits>
#include <optional>
#include <system_error>
#include <vector>

#pragma comment(lib, "mf.lib")
#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfreadwrite.lib")
#pragma comment(lib, "mfuuid.lib")
#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "shell32.lib")

namespace qrec {
namespace {

using Microsoft::WRL::ComPtr;

constexpr LONGLONG kHundredNanosecondsPerMillisecond = 10'000;
constexpr auto kPassthroughRangeTolerance = std::chrono::milliseconds(2);
constexpr std::uint32_t kGifMaximumWidth = 1280;
constexpr std::uint32_t kGifMaximumHeight = 720;
constexpr int kGifMaximumFramesPerSecond = 15;
constexpr DWORD kAllSourceStreams = static_cast<DWORD>(MF_SOURCE_READER_ALL_STREAMS);
constexpr DWORD kFirstVideoStream = static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM);
constexpr DWORD kMediaSource = static_cast<DWORD>(MF_SOURCE_READER_MEDIASOURCE);

std::atomic_uint64_t gPartialSequence{0};

struct SourceVideo final {
    ComPtr<IMFSourceReader> reader;
    ComPtr<IMFMediaType> decodedType;
    std::uint32_t width{};
    std::uint32_t height{};
    std::uint32_t frameRateNumerator{60};
    std::uint32_t frameRateDenominator{1};
    LONG stride{};
    LONGLONG duration{};
};

struct FrameReadResult final {
    ComPtr<IMFSample> sample;
    LONGLONG timestamp{};
    bool endOfStream{};
};

class ScopedMediaFoundation final {
public:
    ScopedMediaFoundation() noexcept
        : result_(::MFStartup(MF_VERSION, MFSTARTUP_FULL)) {}

    ~ScopedMediaFoundation() {
        if (SUCCEEDED(result_)) {
            ::MFShutdown();
        }
    }

    [[nodiscard]] HRESULT Result() const noexcept { return result_; }

private:
    HRESULT result_{};
};

class ClipboardGuard final {
public:
    explicit ClipboardGuard(const HWND owner) noexcept {
        for (int attempt = 0; attempt < 5 && !opened_; ++attempt) {
            opened_ = ::OpenClipboard(owner) != FALSE;
            if (!opened_) {
                ::Sleep(10);
            }
        }
    }

    ~ClipboardGuard() {
        if (opened_) {
            ::CloseClipboard();
        }
    }

    [[nodiscard]] bool IsOpen() const noexcept { return opened_; }

private:
    bool opened_{};
};

std::wstring HResultMessage(const std::wstring_view action, const HRESULT result) {
    return std::wstring(action) + L"：" + win32::FormatError(result);
}

void ReportProgress(
    const MediaExporter::ProgressCallback& callback,
    const double fraction,
    std::wstring phase) {
    if (!callback) {
        return;
    }
    ExportProgress progress{};
    progress.fraction = std::clamp(fraction, 0.0, 1.0);
    progress.phase = std::move(phase);
    try {
        callback(progress);
    } catch (...) {
        ::OutputDebugStringW(
            L"[SuperRecording.MediaExport] progress callback threw an exception\n");
    }
}

LONGLONG ToHundredNanoseconds(const std::chrono::milliseconds value) noexcept {
    return std::max<std::int64_t>(0, value.count()) * kHundredNanosecondsPerMillisecond;
}

std::chrono::milliseconds ToMilliseconds(const LONGLONG value) noexcept {
    return std::chrono::milliseconds(
        std::max<LONGLONG>(0, value) / kHundredNanosecondsPerMillisecond);
}

std::wstring LowercaseExtension(const std::filesystem::path& path) {
    std::wstring extension = path.extension().wstring();
    std::transform(
        extension.begin(),
        extension.end(),
        extension.begin(),
        [](const wchar_t character) {
            return static_cast<wchar_t>(std::towlower(character));
        });
    return extension;
}

bool IsWholeRangeMp4(const ExportRequest& request) noexcept {
    if (request.format != OutputFormat::Mp4 ||
        request.recording.sourcePath.empty() ||
        request.trimStart > kPassthroughRangeTolerance ||
        request.recording.duration.count() <= 0) {
        return false;
    }

    try {
        if (LowercaseExtension(request.recording.sourcePath) != L".mp4") {
            return false;
        }
    } catch (...) {
        return false;
    }

    if (request.trimEnd <= request.trimStart ||
        request.trimEnd >= request.recording.duration) {
        return true;
    }
    return request.recording.duration - request.trimEnd <=
        kPassthroughRangeTolerance;
}

std::filesystem::path PartialPathFor(const std::filesystem::path& destination) {
    const auto directory = destination.parent_path();
    const std::wstring extension = destination.extension().wstring();
    const std::uint64_t sequence = gPartialSequence.fetch_add(
        1,
        std::memory_order_relaxed);
    return directory / std::format(
        L"{}.qrec-partial-{}-{}{}",
        destination.stem().wstring(),
        ::GetCurrentProcessId(),
        sequence,
        extension);
}

bool PathsReferToSameFile(
    const std::filesystem::path& left,
    const std::filesystem::path& right) noexcept {
    std::error_code code;
    if (std::filesystem::exists(left, code) && std::filesystem::exists(right, code)) {
        const bool equivalent = std::filesystem::equivalent(left, right, code);
        if (!code) {
            return equivalent;
        }
    }
    code.clear();
    const auto normalizedLeft = std::filesystem::weakly_canonical(left, code);
    if (code) {
        return false;
    }
    code.clear();
    const auto normalizedRight = std::filesystem::weakly_canonical(right, code);
    return !code && _wcsicmp(normalizedLeft.c_str(), normalizedRight.c_str()) == 0;
}

HRESULT ExtractTopDownBgra(
    IMFSample* sample,
    const SourceVideo& source,
    std::vector<std::uint8_t>* pixels);

HRESULT MaterializePassthroughMp4(
    const std::filesystem::path& source,
    const std::filesystem::path& destination,
    const MediaExporter::ProgressCallback& progress,
    const std::stop_token stopToken,
    MediaExportDisposition* disposition,
    MediaArtifactDelivery* delivery) {
    if (disposition == nullptr || delivery == nullptr) {
        return E_POINTER;
    }
    ReportProgress(progress, 0.01, L"正在快速提交原始 MP4…");
    const HRESULT materializeResult = ExportArtifactCache::Materialize(
        source,
        destination,
        stopToken,
        delivery,
        [&progress](
            const std::uint64_t transferredBytes,
            const std::uint64_t totalBytes) {
            const double fraction = totalBytes > 0
                ? static_cast<double>(transferredBytes) /
                    static_cast<double>(totalBytes)
                : 0.0;
            ReportProgress(
                progress,
                fraction * 0.98,
                L"正在复制原始 MP4…");
        });
    if (FAILED(materializeResult)) {
        return materializeResult;
    }
    if (*delivery == MediaArtifactDelivery::HardLinked) {
        *disposition = MediaExportDisposition::HardLinkedPassthrough;
    } else {
        *disposition = MediaExportDisposition::CopiedPassthrough;
    }
    return S_OK;
}

HRESULT CreateSourceVideo(const std::filesystem::path& sourcePath, SourceVideo* output) {
    if (output == nullptr) {
        return E_POINTER;
    }

    ComPtr<IMFAttributes> attributes;
    HRESULT result = ::MFCreateAttributes(&attributes, 4);
    if (FAILED(result)) {
        return result;
    }
    attributes->SetUINT32(MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, TRUE);
    attributes->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, TRUE);
    attributes->SetUINT32(MF_LOW_LATENCY, FALSE);

    SourceVideo video{};
    result = ::MFCreateSourceReaderFromURL(sourcePath.c_str(), attributes.Get(), &video.reader);
    if (FAILED(result)) {
        return result;
    }

    video.reader->SetStreamSelection(kAllSourceStreams, FALSE);
    result = video.reader->SetStreamSelection(kFirstVideoStream, TRUE);
    if (FAILED(result)) {
        return result;
    }

    ComPtr<IMFMediaType> requestedType;
    result = ::MFCreateMediaType(&requestedType);
    if (FAILED(result)) {
        return result;
    }
    requestedType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    requestedType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32);
    requestedType->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    result = video.reader->SetCurrentMediaType(
        kFirstVideoStream, nullptr, requestedType.Get());
    if (FAILED(result)) {
        return result;
    }

    result = video.reader->GetCurrentMediaType(
        kFirstVideoStream, &video.decodedType);
    if (FAILED(result)) {
        return result;
    }
    result = ::MFGetAttributeSize(
        video.decodedType.Get(), MF_MT_FRAME_SIZE, &video.width, &video.height);
    if (FAILED(result) || video.width == 0 || video.height == 0) {
        return FAILED(result) ? result : MF_E_INVALIDMEDIATYPE;
    }

    if (FAILED(::MFGetAttributeRatio(
            video.decodedType.Get(),
            MF_MT_FRAME_RATE,
            &video.frameRateNumerator,
            &video.frameRateDenominator)) ||
        video.frameRateNumerator == 0 || video.frameRateDenominator == 0) {
        video.frameRateNumerator = 60;
        video.frameRateDenominator = 1;
    }

    UINT32 unsignedStride = 0;
    if (SUCCEEDED(video.decodedType->GetUINT32(MF_MT_DEFAULT_STRIDE, &unsignedStride))) {
        video.stride = static_cast<LONG>(unsignedStride);
    } else {
        result = ::MFGetStrideForBitmapInfoHeader(
            MFVideoFormat_RGB32.Data1, video.width, &video.stride);
        if (FAILED(result)) {
            return result;
        }
        video.decodedType->SetUINT32(MF_MT_DEFAULT_STRIDE, static_cast<UINT32>(video.stride));
    }

    PROPVARIANT duration{};
    ::PropVariantInit(&duration);
    if (SUCCEEDED(video.reader->GetPresentationAttribute(
            kMediaSource, MF_PD_DURATION, &duration))) {
        if (duration.vt == VT_UI8) {
            video.duration = static_cast<LONGLONG>(duration.uhVal.QuadPart);
        } else if (duration.vt == VT_I8) {
            video.duration = duration.hVal.QuadPart;
        }
    }
    ::PropVariantClear(&duration);

    *output = std::move(video);
    return S_OK;
}

HRESULT SeekSource(IMFSourceReader* reader, const LONGLONG position) {
    PROPVARIANT value{};
    ::PropVariantInit(&value);
    value.vt = VT_I8;
    value.hVal.QuadPart = std::max<LONGLONG>(0, position);
    const HRESULT result = reader->SetCurrentPosition(GUID_NULL, value);
    ::PropVariantClear(&value);
    return result;
}

HRESULT ReadVideoFrame(IMFSourceReader* reader, FrameReadResult* output) {
    if (reader == nullptr || output == nullptr) {
        return E_POINTER;
    }
    for (;;) {
        DWORD actualStream = 0;
        DWORD flags = 0;
        LONGLONG timestamp = 0;
        ComPtr<IMFSample> sample;
        const HRESULT result = reader->ReadSample(
            kFirstVideoStream,
            0,
            &actualStream,
            &flags,
            &timestamp,
            &sample);
        if (FAILED(result)) {
            return result;
        }
        if ((flags & MF_SOURCE_READERF_ERROR) != 0) {
            return E_FAIL;
        }
        if ((flags & MF_SOURCE_READERF_ENDOFSTREAM) != 0) {
            output->sample.Reset();
            output->timestamp = timestamp;
            output->endOfStream = true;
            return S_OK;
        }
        if (sample != nullptr) {
            output->sample = std::move(sample);
            output->timestamp = timestamp;
            output->endOfStream = false;
            return S_OK;
        }
    }
}

LONGLONG NominalFrameDuration(const SourceVideo& source) noexcept {
    return std::max<LONGLONG>(
        1,
        (10'000'000LL * static_cast<LONGLONG>(source.frameRateDenominator)) /
            static_cast<LONGLONG>(source.frameRateNumerator));
}

std::uint32_t ComputeVideoBitRate(const SourceVideo& source) noexcept {
    const double framesPerSecond = static_cast<double>(source.frameRateNumerator) /
        static_cast<double>(source.frameRateDenominator);
    const double estimated = static_cast<double>(source.width) *
        static_cast<double>(source.height) * framesPerSecond * 0.16;
    return static_cast<std::uint32_t>(
        std::clamp(estimated, 4'000'000.0, 60'000'000.0));
}

HRESULT ExportMp4(
    const ExportRequest& request,
    const std::filesystem::path& partialPath,
    const MediaExporter::ProgressCallback& progress,
    const std::stop_token stopToken,
    std::wstring* errorMessage) {
    SourceVideo source;
    HRESULT result = CreateSourceVideo(request.recording.sourcePath, &source);
    if (FAILED(result)) {
        if (errorMessage != nullptr) {
            *errorMessage = HResultMessage(L"无法读取源录屏", result);
        }
        return result;
    }

    LONGLONG trimStart = ToHundredNanoseconds(request.trimStart);
    LONGLONG trimEnd = ToHundredNanoseconds(request.trimEnd);
    const LONGLONG knownDuration = source.duration > 0
        ? source.duration
        : ToHundredNanoseconds(request.recording.duration);
    if (trimEnd <= trimStart) {
        trimEnd = knownDuration;
    }
    if (knownDuration > 0) {
        trimStart = std::clamp<LONGLONG>(trimStart, 0, knownDuration);
        trimEnd = std::clamp<LONGLONG>(trimEnd, trimStart, knownDuration);
    }
    if (trimEnd <= trimStart) {
        if (errorMessage != nullptr) {
            *errorMessage = L"裁剪区间为空。";
        }
        return E_INVALIDARG;
    }

    const double sourceFramesPerSecond =
        static_cast<double>(source.frameRateNumerator) /
        static_cast<double>(source.frameRateDenominator);
    const int framesPerSecond = request.recording.framesPerSecond == 30 ||
            request.recording.framesPerSecond == 60
        ? request.recording.framesPerSecond
        : (sourceFramesPerSecond >= 45.0 ? 60 : 30);
    media::Mp4Writer writer;
    media::Mp4WriterConfig writerConfig{};
    writerConfig.outputPath = partialPath;
    writerConfig.width = source.width;
    writerConfig.height = source.height;
    writerConfig.framesPerSecond = framesPerSecond;
    writerConfig.averageBitrate = ComputeVideoBitRate(source);
    writerConfig.preferHardwareEncoder = true;
    std::wstring writerError;
    long writerNativeError = 0;
    if (!writer.Open(writerConfig, writerError, writerNativeError)) {
        if (errorMessage != nullptr) {
            *errorMessage = std::move(writerError);
        }
        return static_cast<HRESULT>(writerNativeError);
    }

    result = SeekSource(source.reader.Get(), trimStart);
    if (FAILED(result)) {
        if (errorMessage != nullptr) {
            *errorMessage = HResultMessage(L"无法定位裁剪起点", result);
        }
        return result;
    }

    ReportProgress(progress, 0.02, L"正在编码 MP4…");
    const LONGLONG nominalDuration = NominalFrameDuration(source);
    const LONGLONG requestedDuration = std::max<LONGLONG>(1, trimEnd - trimStart);
    std::optional<LONGLONG> outputBase;
    std::uint64_t writtenFrames = 0;
    std::vector<std::uint8_t> pixels;
    LONGLONG lastOutputTimestamp = -1;

    for (;;) {
        if (stopToken.stop_requested()) {
            return HRESULT_FROM_WIN32(ERROR_CANCELLED);
        }
        FrameReadResult frame;
        result = ReadVideoFrame(source.reader.Get(), &frame);
        if (FAILED(result)) {
            if (errorMessage != nullptr) {
                *errorMessage = HResultMessage(L"读取源视频帧失败", result);
            }
            return result;
        }
        if (frame.endOfStream || frame.timestamp >= trimEnd) {
            break;
        }
        if (frame.timestamp < trimStart) {
            continue;
        }

        if (!outputBase.has_value()) {
            outputBase = frame.timestamp;
        }
        LONGLONG sampleDuration = 0;
        if (FAILED(frame.sample->GetSampleDuration(&sampleDuration)) || sampleDuration <= 0) {
            sampleDuration = nominalDuration;
        }
        sampleDuration = std::min(sampleDuration, trimEnd - frame.timestamp);
        const LONGLONG outputTimestamp = frame.timestamp - *outputBase;
        if (outputTimestamp <= lastOutputTimestamp) {
            continue;
        }
        result = ExtractTopDownBgra(frame.sample.Get(), source, &pixels);
        if (FAILED(result)) {
            if (errorMessage != nullptr) {
                *errorMessage = HResultMessage(L"规范化解码帧行跨度失败", result);
            }
            return result;
        }
        if (!writer.WriteBgraFrame(
                pixels,
                source.width * 4U,
                outputTimestamp,
                std::max<LONGLONG>(1, sampleDuration),
                writerError,
                writerNativeError)) {
            if (errorMessage != nullptr) {
                *errorMessage = std::move(writerError);
            }
            return static_cast<HRESULT>(writerNativeError);
        }
        lastOutputTimestamp = outputTimestamp;
        ++writtenFrames;
        const double fraction = static_cast<double>(frame.timestamp - trimStart) /
            static_cast<double>(requestedDuration);
        ReportProgress(progress, 0.02 + fraction * 0.94, L"正在编码 MP4…");
    }

    if (writtenFrames == 0) {
        if (errorMessage != nullptr) {
            *errorMessage = L"裁剪区间内没有可导出的视频帧。";
        }
        return MF_E_END_OF_STREAM;
    }
    if (stopToken.stop_requested()) {
        return HRESULT_FROM_WIN32(ERROR_CANCELLED);
    }
    ReportProgress(progress, 0.97, L"正在完成 MP4 文件…");
    if (!writer.Finalize(writerError, writerNativeError)) {
        if (errorMessage != nullptr) {
            *errorMessage = std::move(writerError);
        }
        return static_cast<HRESULT>(writerNativeError);
    }
    return S_OK;
}

std::pair<std::uint32_t, std::uint32_t> ComputeGifSize(
    const std::uint32_t width,
    const std::uint32_t height) noexcept {
    const double scale = std::min({
        1.0,
        static_cast<double>(kGifMaximumWidth) / static_cast<double>(width),
        static_cast<double>(kGifMaximumHeight) / static_cast<double>(height)});
    return {
        std::max<std::uint32_t>(1, static_cast<std::uint32_t>(std::lround(width * scale))),
        std::max<std::uint32_t>(1, static_cast<std::uint32_t>(std::lround(height * scale)))};
}

HRESULT ExtractTopDownBgra(
    IMFSample* sample,
    const SourceVideo& source,
    std::vector<std::uint8_t>* pixels) {
    if (sample == nullptr || pixels == nullptr) {
        return E_POINTER;
    }
    const std::size_t outputStride = static_cast<std::size_t>(source.width) * 4;
    if (source.height > std::numeric_limits<std::size_t>::max() / outputStride) {
        return E_OUTOFMEMORY;
    }
    pixels->resize(outputStride * source.height);

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
            twoDimensional->Unlock2D();
            return MF_E_BUFFERTOOSMALL;
        }
        for (std::uint32_t row = 0; row < source.height; ++row) {
            auto* destination = pixels->data() + static_cast<std::size_t>(row) * outputStride;
            const auto* input = sourceScanline + static_cast<std::ptrdiff_t>(row) * sourceStride;
            std::memcpy(destination, input, outputStride);
            for (std::size_t pixel = 3; pixel < outputStride; pixel += 4) {
                destination[pixel] = 0xFF;
            }
        }
        twoDimensional->Unlock2D();
        return S_OK;
    }

    DWORD maximumLength = 0;
    DWORD currentLength = 0;
    result = buffer->Lock(&sourceScanline, &maximumLength, &currentLength);
    if (FAILED(result)) {
        return result;
    }
    const std::size_t declaredStride = static_cast<std::size_t>(
        std::llabs(static_cast<long long>(sourceStride)));
    std::size_t actualStride = declaredStride;
    if (source.height != 0 && currentLength % source.height == 0) {
        const std::size_t inferredStride = currentLength / source.height;
        if (inferredStride >= outputStride) {
            actualStride = inferredStride;
        }
    }
    const std::size_t codedHeight =
        (static_cast<std::size_t>(source.height) + 15U) & ~std::size_t{15U};
    if (codedHeight != 0U && currentLength % codedHeight == 0U) {
        const std::size_t codedStride = currentLength / codedHeight;
        if (codedStride >= outputStride && codedStride % 4U == 0U) {
            actualStride = codedStride;
        }
    }
    const std::size_t required = actualStride * (source.height - 1U) + outputStride;
    if (sourceScanline == nullptr || currentLength < required) {
        buffer->Unlock();
        return MF_E_BUFFERTOOSMALL;
    }
    const bool bottomUp = sourceStride < 0;
    for (std::uint32_t row = 0; row < source.height; ++row) {
        auto* destination = pixels->data() + static_cast<std::size_t>(row) * outputStride;
        const std::uint32_t sourceRow = bottomUp ? source.height - 1 - row : row;
        const auto* input = sourceScanline +
            static_cast<std::size_t>(sourceRow) * actualStride;
        std::memcpy(destination, input, outputStride);
        for (std::size_t pixel = 3; pixel < outputStride; pixel += 4) {
            destination[pixel] = 0xFF;
        }
    }
    buffer->Unlock();
    return S_OK;
}

HRESULT SetGifLoopMetadata(IWICBitmapEncoder* encoder) {
    ComPtr<IWICMetadataQueryWriter> writer;
    HRESULT result = encoder->GetMetadataQueryWriter(&writer);
    if (FAILED(result)) {
        return result;
    }
    std::array<UCHAR, 11> application{
        'N', 'E', 'T', 'S', 'C', 'A', 'P', 'E', '2', '.', '0'};
    PROPVARIANT applicationValue{};
    applicationValue.vt = VT_UI1 | VT_VECTOR;
    applicationValue.caub.cElems = static_cast<ULONG>(application.size());
    applicationValue.caub.pElems = application.data();
    result = writer->SetMetadataByName(L"/appext/Application", &applicationValue);
    if (FAILED(result)) {
        return result;
    }
    std::array<UCHAR, 5> loopData{3, 1, 0, 0, 0};
    PROPVARIANT loopValue{};
    loopValue.vt = VT_UI1 | VT_VECTOR;
    loopValue.caub.cElems = static_cast<ULONG>(loopData.size());
    loopValue.caub.pElems = loopData.data();
    return writer->SetMetadataByName(L"/appext/Data", &loopValue);
}

HRESULT WriteGifFrame(
    IWICImagingFactory* factory,
    IWICBitmapEncoder* encoder,
    const SourceVideo& source,
    const std::vector<std::uint8_t>& pixels,
    const std::uint32_t outputWidth,
    const std::uint32_t outputHeight,
    const std::uint16_t delayCentiseconds) {
    ComPtr<IWICBitmap> bitmap;
    HRESULT result = factory->CreateBitmapFromMemory(
        source.width,
        source.height,
        GUID_WICPixelFormat32bppBGRA,
        source.width * 4,
        static_cast<UINT>(pixels.size()),
        const_cast<BYTE*>(pixels.data()),
        &bitmap);
    if (FAILED(result)) {
        return result;
    }

    ComPtr<IWICBitmapSource> scaledSource;
    if (outputWidth != source.width || outputHeight != source.height) {
        ComPtr<IWICBitmapScaler> scaler;
        result = factory->CreateBitmapScaler(&scaler);
        if (FAILED(result)) {
            return result;
        }
        result = scaler->Initialize(
            bitmap.Get(), outputWidth, outputHeight, WICBitmapInterpolationModeFant);
        if (FAILED(result)) {
            return result;
        }
        scaledSource = scaler;
    } else {
        scaledSource = bitmap;
    }

    ComPtr<IWICPalette> palette;
    result = factory->CreatePalette(&palette);
    if (FAILED(result)) {
        return result;
    }
    result = palette->InitializeFromBitmap(scaledSource.Get(), 256, FALSE);
    if (FAILED(result)) {
        return result;
    }

    ComPtr<IWICFormatConverter> converter;
    result = factory->CreateFormatConverter(&converter);
    if (FAILED(result)) {
        return result;
    }
    result = converter->Initialize(
        scaledSource.Get(),
        GUID_WICPixelFormat8bppIndexed,
        WICBitmapDitherTypeErrorDiffusion,
        palette.Get(),
        0.0,
        WICBitmapPaletteTypeCustom);
    if (FAILED(result)) {
        return result;
    }

    ComPtr<IWICBitmapFrameEncode> frame;
    ComPtr<IPropertyBag2> frameOptions;
    result = encoder->CreateNewFrame(&frame, &frameOptions);
    if (FAILED(result)) {
        return result;
    }
    result = frame->Initialize(frameOptions.Get());
    if (FAILED(result)) {
        return result;
    }
    result = frame->SetSize(outputWidth, outputHeight);
    if (FAILED(result)) {
        return result;
    }
    WICPixelFormatGUID pixelFormat = GUID_WICPixelFormat8bppIndexed;
    result = frame->SetPixelFormat(&pixelFormat);
    if (FAILED(result) || pixelFormat != GUID_WICPixelFormat8bppIndexed) {
        return FAILED(result) ? result : WINCODEC_ERR_UNSUPPORTEDPIXELFORMAT;
    }
    frame->SetPalette(palette.Get());

    ComPtr<IWICMetadataQueryWriter> metadata;
    if (SUCCEEDED(frame->GetMetadataQueryWriter(&metadata))) {
        PROPVARIANT delay{};
        delay.vt = VT_UI2;
        delay.uiVal = std::max<std::uint16_t>(1, delayCentiseconds);
        metadata->SetMetadataByName(L"/grctlext/Delay", &delay);

        PROPVARIANT disposal{};
        disposal.vt = VT_UI1;
        disposal.bVal = 2;
        metadata->SetMetadataByName(L"/grctlext/Disposal", &disposal);
    }

    result = frame->WriteSource(converter.Get(), nullptr);
    if (FAILED(result)) {
        return result;
    }
    return frame->Commit();
}

HRESULT ExportGif(
    const ExportRequest& request,
    const std::filesystem::path& partialPath,
    const MediaExporter::ProgressCallback& progress,
    const std::stop_token stopToken,
    std::wstring* errorMessage) {
    SourceVideo source;
    HRESULT result = CreateSourceVideo(request.recording.sourcePath, &source);
    if (FAILED(result)) {
        if (errorMessage != nullptr) {
            *errorMessage = HResultMessage(L"无法读取源录屏", result);
        }
        return result;
    }

    LONGLONG trimStart = ToHundredNanoseconds(request.trimStart);
    LONGLONG trimEnd = ToHundredNanoseconds(request.trimEnd);
    const LONGLONG knownDuration = source.duration > 0
        ? source.duration
        : ToHundredNanoseconds(request.recording.duration);
    if (trimEnd <= trimStart) {
        trimEnd = knownDuration;
    }
    if (knownDuration > 0) {
        trimStart = std::clamp<LONGLONG>(trimStart, 0, knownDuration);
        trimEnd = std::clamp<LONGLONG>(trimEnd, trimStart, knownDuration);
    }
    if (trimEnd <= trimStart) {
        if (errorMessage != nullptr) {
            *errorMessage = L"裁剪区间为空。";
        }
        return E_INVALIDARG;
    }

    ComPtr<IWICImagingFactory> factory;
    result = ::CoCreateInstance(
        CLSID_WICImagingFactory2,
        nullptr,
        CLSCTX_INPROC_SERVER,
        IID_PPV_ARGS(&factory));
    if (FAILED(result)) {
        result = ::CoCreateInstance(
            CLSID_WICImagingFactory,
            nullptr,
            CLSCTX_INPROC_SERVER,
            IID_PPV_ARGS(&factory));
    }
    if (FAILED(result)) {
        if (errorMessage != nullptr) {
            *errorMessage = HResultMessage(L"无法启动 GIF 编码器", result);
        }
        return result;
    }

    ComPtr<IWICStream> stream;
    result = factory->CreateStream(&stream);
    if (SUCCEEDED(result)) {
        result = stream->InitializeFromFilename(partialPath.c_str(), GENERIC_WRITE);
    }
    ComPtr<IWICBitmapEncoder> encoder;
    if (SUCCEEDED(result)) {
        result = factory->CreateEncoder(GUID_ContainerFormatGif, nullptr, &encoder);
    }
    if (SUCCEEDED(result)) {
        result = encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache);
    }
    if (FAILED(result)) {
        if (errorMessage != nullptr) {
            *errorMessage = HResultMessage(L"无法创建 GIF 文件", result);
        }
        return result;
    }
    // 某些旧版 WIC 不允许全局循环元数据；帧内容仍可正常导出。
    SetGifLoopMetadata(encoder.Get());

    result = SeekSource(source.reader.Get(), trimStart);
    if (FAILED(result)) {
        if (errorMessage != nullptr) {
            *errorMessage = HResultMessage(L"无法定位裁剪起点", result);
        }
        return result;
    }

    const double sourceFramesPerSecond = static_cast<double>(source.frameRateNumerator) /
        static_cast<double>(source.frameRateDenominator);
    const int gifFramesPerSecond = std::clamp(
        static_cast<int>(std::lround(sourceFramesPerSecond)), 1, kGifMaximumFramesPerSecond);
    const LONGLONG gifFrameStep = 10'000'000LL / gifFramesPerSecond;
    const auto [outputWidth, outputHeight] = ComputeGifSize(source.width, source.height);
    const LONGLONG requestedDuration = std::max<LONGLONG>(1, trimEnd - trimStart);
    LONGLONG nextFrameTime = trimStart;
    std::uint64_t writtenFrames = 0;
    std::vector<std::uint8_t> pixels;

    ReportProgress(
        progress,
        0.02,
        std::format(L"正在生成 GIF（{} FPS，{}×{}）…",
                    gifFramesPerSecond, outputWidth, outputHeight));

    for (;;) {
        if (stopToken.stop_requested()) {
            return HRESULT_FROM_WIN32(ERROR_CANCELLED);
        }
        FrameReadResult frame;
        result = ReadVideoFrame(source.reader.Get(), &frame);
        if (FAILED(result)) {
            if (errorMessage != nullptr) {
                *errorMessage = HResultMessage(L"读取源视频帧失败", result);
            }
            return result;
        }
        if (frame.endOfStream || frame.timestamp >= trimEnd) {
            break;
        }
        if (frame.timestamp < trimStart || frame.timestamp < nextFrameTime) {
            continue;
        }
        result = ExtractTopDownBgra(frame.sample.Get(), source, &pixels);
        if (SUCCEEDED(result)) {
            // GIF 延迟单位是 1/100 秒；用误差扩散交替 6/7 等延迟，避免 15 FPS
            // 全部取 7 导致长片段逐渐变慢。
            const auto accumulatedDelay = [](const std::uint64_t frameCount, const int fps) {
                return static_cast<std::uint64_t>(std::llround(
                    static_cast<double>(frameCount) * 100.0 / static_cast<double>(fps)));
            };
            const std::uint64_t delayUnits =
                accumulatedDelay(writtenFrames + 1, gifFramesPerSecond) -
                accumulatedDelay(writtenFrames, gifFramesPerSecond);
            const auto frameDelay = static_cast<std::uint16_t>(
                std::clamp<std::uint64_t>(delayUnits, 1, 65'535));
            result = WriteGifFrame(
                factory.Get(), encoder.Get(), source, pixels,
                outputWidth, outputHeight, frameDelay);
        }
        if (FAILED(result)) {
            if (errorMessage != nullptr) {
                *errorMessage = HResultMessage(L"写入 GIF 视频帧失败", result);
            }
            return result;
        }
        ++writtenFrames;
        do {
            nextFrameTime += gifFrameStep;
        } while (nextFrameTime <= frame.timestamp);

        const double fraction = static_cast<double>(frame.timestamp - trimStart) /
            static_cast<double>(requestedDuration);
        ReportProgress(progress, 0.02 + fraction * 0.94, L"正在生成 GIF…");
    }

    if (writtenFrames == 0) {
        if (errorMessage != nullptr) {
            *errorMessage = L"裁剪区间内没有可导出的 GIF 帧。";
        }
        return MF_E_END_OF_STREAM;
    }
    if (stopToken.stop_requested()) {
        return HRESULT_FROM_WIN32(ERROR_CANCELLED);
    }
    ReportProgress(progress, 0.97, L"正在完成 GIF 文件…");
    result = encoder->Commit();
    if (FAILED(result) && errorMessage != nullptr) {
        *errorMessage = HResultMessage(L"完成 GIF 文件失败", result);
    }
    return result;
}

std::wstring_view BoundaryTrimOutcomeName(
    const Mp4BoundaryTrimOutcome outcome) noexcept {
    switch (outcome) {
    case Mp4BoundaryTrimOutcome::Succeeded:
        return L"Succeeded";
    case Mp4BoundaryTrimOutcome::Unsupported:
        return L"Unsupported";
    case Mp4BoundaryTrimOutcome::Cancelled:
        return L"Cancelled";
    case Mp4BoundaryTrimOutcome::Failed:
        return L"Failed";
    }
    return L"Unknown";
}

std::uint64_t FileSizeOrZero(const std::filesystem::path& path) noexcept {
    std::error_code error;
    const std::uintmax_t size = std::filesystem::file_size(path, error);
    return error ? 0 : static_cast<std::uint64_t>(size);
}

MediaExportResult PrepareCachedArtifact(
    const ExportRequest& request,
    const MediaExporter::ProgressCallback& progress,
    const std::stop_token stopToken) {
    MediaExportResult result{};
    if (request.recording.sourcePath.empty()) {
        result.nativeError = E_INVALIDARG;
        result.errorMessage = L"源录屏路径为空。";
        return result;
    }
    if (stopToken.stop_requested()) {
        result.cancelled = true;
        result.nativeError = HRESULT_FROM_WIN32(ERROR_CANCELLED);
        result.errorMessage = L"导出缓存预生成已取消。";
        return result;
    }

    std::wstring keyError;
    const std::optional<ExportArtifactCacheKey> key =
        ExportArtifactCache::BuildKey(request, &keyError);
    if (!key.has_value()) {
        result.nativeError = HRESULT_FROM_WIN32(ERROR_FILE_INVALID);
        result.errorMessage = std::move(keyError);
        return result;
    }
    result.sourceBytes = key->sourceSize;
    result.cacheKey = ExportArtifactCache::KeyId(*key);

    if (request.includeSystemAudio) {
        ExportRequest videoOnlyRequest = request;
        videoOnlyRequest.includeSystemAudio = false;
        videoOnlyRequest.destinationPath.clear();
        const MediaExportResult videoResult = PrepareCachedArtifact(
            videoOnlyRequest,
            [&progress](const ExportProgress& videoProgress) {
                ReportProgress(
                    progress,
                    0.005 + std::clamp(videoProgress.fraction, 0.0, 1.0) * 0.78,
                    L"正在准备视频画面…");
            },
            stopToken);
        if (!videoResult.success) {
            result.cancelled = videoResult.cancelled;
            result.nativeError = videoResult.nativeError;
            result.errorMessage = videoResult.errorMessage.empty()
                ? L"无法准备有声 MP4 的视频画面。"
                : videoResult.errorMessage;
            result.diagnosticSummary =
                L"audioMuxVideoPreparationFailed=true; videoDetail=" +
                videoResult.diagnosticSummary;
            return result;
        }

        ReportProgress(progress, 0.80, L"正在快速合并电脑声音…");
        AudioVideoMuxResult muxResult{};
        const ExportArtifactCacheResult cacheResult =
            ExportArtifactCache::Shared().GetOrCreate(
                *key,
                L".mp4",
                stopToken,
                [&](const std::filesystem::path& stagingPath,
                    const std::stop_token generationStopToken,
                    std::wstring* generationError) -> HRESULT {
                    muxResult = AudioVideoMuxer::Mux(
                        AudioVideoMuxRequest{
                            videoResult.outputPath,
                            request.recording.systemAudio.sourcePath,
                            stagingPath,
                            request.trimStart,
                            request.trimEnd,
                        },
                        generationStopToken);
                    if (generationError != nullptr) {
                        *generationError = muxResult.errorMessage;
                    }
                    switch (muxResult.outcome) {
                    case AudioVideoMuxOutcome::Succeeded:
                        return S_OK;
                    case AudioVideoMuxOutcome::Cancelled:
                        return HRESULT_FROM_WIN32(ERROR_CANCELLED);
                    case AudioVideoMuxOutcome::Unsupported:
                    case AudioVideoMuxOutcome::Failed:
                    default:
                        return FAILED(muxResult.nativeError)
                            ? muxResult.nativeError
                            : E_FAIL;
                    }
                });

        result.cacheHit = cacheResult.cacheHit;
        result.waitedForCacheBuilder = cacheResult.waitedForBuilder;
        result.cacheBuilderWait = cacheResult.builderWait;
        result.cacheGeneration = cacheResult.generationElapsed;
        result.cacheKey = cacheResult.keyId;
        result.nativeError = cacheResult.nativeError;
        result.diagnosticSummary = cacheResult.cacheHit
            ? L"audioMux=true; cacheArtifactValidated=true"
            : std::format(
                L"audioMux=true; videoDisposition={}; videoCacheHit={}; "
                L"videoSamples={}; audioSamples={}; audioLeadingGapNs={}; "
                L"audioTrailingGapNs={}; droppedLeadingAccessUnit={}; "
                L"droppedTrailingAccessUnit={}; muxError={}",
                MediaExporter::DispositionName(videoResult.disposition),
                videoResult.cacheHit ? L"true" : L"false",
                muxResult.videoSamples,
                muxResult.audioSamples,
                muxResult.audioLeadingGap.count(),
                muxResult.audioTrailingGap.count(),
                muxResult.droppedLeadingBoundaryAccessUnit ? L"true" : L"false",
                muxResult.droppedTrailingBoundaryAccessUnit ? L"true" : L"false",
                muxResult.errorMessage);
        if (!cacheResult.success) {
            result.cancelled = cacheResult.cancelled;
            result.errorMessage = cacheResult.errorMessage.empty()
                ? L"无法把电脑声音合并到 MP4。"
                : cacheResult.errorMessage;
            return result;
        }

        result.success = true;
        result.nativeError = S_OK;
        result.outputPath = cacheResult.artifactPath;
        result.outputBytes = FileSizeOrZero(cacheResult.artifactPath);
        result.disposition = cacheResult.cacheHit
            ? MediaExportDisposition::CachedArtifact
            : MediaExportDisposition::AudioMuxed;
        ReportProgress(progress, 0.97, L"有声 MP4 已准备完成");
        return result;
    }

    if (IsWholeRangeMp4(request)) {
        result.success = true;
        result.nativeError = S_OK;
        result.outputPath = request.recording.sourcePath;
        result.outputBytes = result.sourceBytes;
        result.disposition = MediaExportDisposition::OriginalPassthrough;
        result.diagnosticSummary = L"wholeRangeSourceNeedsNoWarmup=true";
        return result;
    }

    ReportProgress(progress, 0.005, L"正在检查导出缓存…");
    MediaExportDisposition generatedDisposition =
        MediaExportDisposition::Transcoded;
    std::wstring generationDiagnostic;
    const std::wstring extension = request.format == OutputFormat::Gif
        ? L".gif"
        : L".mp4";
    const ExportArtifactCacheResult cacheResult =
        ExportArtifactCache::Shared().GetOrCreate(
            *key,
            extension,
            stopToken,
            [&](const std::filesystem::path& stagingPath,
                const std::stop_token generationStopToken,
                std::wstring* generationError) -> HRESULT {
                const win32::ScopedCoInitialize apartment(COINIT_MULTITHREADED);
                if (FAILED(apartment.Result()) &&
                    apartment.Result() != RPC_E_CHANGED_MODE) {
                    if (generationError != nullptr) {
                        *generationError = HResultMessage(
                            L"无法初始化导出线程",
                            apartment.Result());
                    }
                    return apartment.Result();
                }
                const ScopedMediaFoundation mediaFoundation;
                if (FAILED(mediaFoundation.Result())) {
                    if (generationError != nullptr) {
                        *generationError = HResultMessage(
                            L"无法启动 Media Foundation",
                            mediaFoundation.Result());
                    }
                    return mediaFoundation.Result();
                }

                if (request.format == OutputFormat::Gif) {
                    generatedDisposition = MediaExportDisposition::Transcoded;
                    generationDiagnostic = L"generator=GifTranscode";
                    return ExportGif(
                        request,
                        stagingPath,
                        progress,
                        generationStopToken,
                        generationError);
                }

                ReportProgress(
                    progress,
                    0.01,
                    L"正在编码起始边界并直通原始画面…");
                const Mp4BoundaryTrimResult boundaryTrim =
                    Mp4BoundaryTrimmer::Trim(
                    request.recording.sourcePath,
                    stagingPath,
                    request.trimStart,
                    request.trimEnd,
                    generationStopToken);
                generationDiagnostic = std::format(
                    L"boundaryTrimOutcome={}; boundaryTrimHResult=0x{:08X}; "
                    L"boundaryEncodedFrames={}; boundaryPassthroughSamples={}; "
                    L"boundaryDurationNs={}; boundaryScanMs={}; "
                    L"boundaryDecodeEncodeMs={}; boundaryRemuxMs={}; "
                    L"usedPrewarmedEncoder={}; encoderPrepareWaitMs={}; "
                    L"encoderOpenMs={}; "
                    L"boundaryTrimReason={}",
                    BoundaryTrimOutcomeName(boundaryTrim.outcome),
                    static_cast<std::uint32_t>(boundaryTrim.nativeError),
                    boundaryTrim.encodedFrames,
                    boundaryTrim.passthroughSamples,
                    boundaryTrim.boundaryDuration.count(),
                    boundaryTrim.scanDuration.count(),
                    boundaryTrim.decodeEncodeDuration.count(),
                    boundaryTrim.remuxDuration.count(),
                    boundaryTrim.usedPrewarmedEncoder ? L"true" : L"false",
                    boundaryTrim.encoderPrepareWait.count(),
                    boundaryTrim.encoderOpen.count(),
                    boundaryTrim.errorMessage);
                if (boundaryTrim.outcome == Mp4BoundaryTrimOutcome::Succeeded) {
                    generatedDisposition =
                        MediaExportDisposition::BoundaryTrimmedHybrid;
                    ReportProgress(progress, 0.96, L"边界裁剪成片已就绪");
                    return S_OK;
                }
                if (boundaryTrim.outcome == Mp4BoundaryTrimOutcome::Cancelled) {
                    if (generationError != nullptr) {
                        *generationError = boundaryTrim.errorMessage;
                    }
                    return HRESULT_FROM_WIN32(ERROR_CANCELLED);
                }

                std::error_code cleanupError;
                std::filesystem::remove(stagingPath, cleanupError);
                ReportProgress(
                    progress,
                    0.01,
                    L"边界直通不可用，正在兼容编码 MP4…");
                generatedDisposition = MediaExportDisposition::Transcoded;
                return ExportMp4(
                    request,
                    stagingPath,
                    progress,
                    generationStopToken,
                    generationError);
            });

    result.cacheHit = cacheResult.cacheHit;
    result.waitedForCacheBuilder = cacheResult.waitedForBuilder;
    result.cacheBuilderWait = cacheResult.builderWait;
    result.cacheGeneration = cacheResult.generationElapsed;
    result.cacheKey = cacheResult.keyId;
    result.nativeError = cacheResult.nativeError;
    result.diagnosticSummary = generationDiagnostic;
    if (!cacheResult.success) {
        result.cancelled = cacheResult.cancelled;
        result.errorMessage = cacheResult.errorMessage;
        return result;
    }

    result.success = true;
    result.errorMessage.clear();
    result.outputPath = cacheResult.artifactPath;
    result.outputBytes = FileSizeOrZero(cacheResult.artifactPath);
    result.disposition = cacheResult.cacheHit
        ? MediaExportDisposition::CachedArtifact
        : generatedDisposition;
    if (cacheResult.cacheHit) {
        result.diagnosticSummary = L"cacheArtifactValidated=true";
        ReportProgress(progress, 0.97, L"已复用导出缓存");
    }
    return result;
}

void CompleteDiagnostics(
    const ExportRequest& request,
    MediaExportResult* result) {
    if (result == nullptr) {
        return;
    }
    const std::wstring detail = std::move(result->diagnosticSummary);
    const std::wstring error = result->errorMessage;
    result->diagnosticSummary = std::format(
        L"format={}; source={}; trimStartMs={}; trimEndMs={}; "
        L"recordingDurationMs={}; includeSystemAudio={}; audioSource={}; "
        L"sourceBytes={}; cacheKey={}; cacheHit={}; "
        L"waitedForCacheBuilder={}; cacheBuilderWaitMs={}; "
        L"cacheGenerationMs={}; deliveryMs={}; "
        L"disposition={}; delivery={}; elapsedMs={}; outputBytes={}; "
        L"success={}; cancelled={}; nativeError=0x{:08X}; detail={}; error={}",
        request.format == OutputFormat::Gif ? L"GIF" : L"MP4",
        request.recording.sourcePath.wstring(),
        request.trimStart.count(),
        request.trimEnd.count(),
        request.recording.duration.count(),
        request.includeSystemAudio ? L"true" : L"false",
        request.recording.systemAudio.sourcePath.wstring(),
        result->sourceBytes,
        result->cacheKey,
        result->cacheHit ? L"true" : L"false",
        result->waitedForCacheBuilder ? L"true" : L"false",
        result->cacheBuilderWait.count(),
        result->cacheGeneration.count(),
        result->deliveryElapsed.count(),
        MediaExporter::DispositionName(result->disposition),
        MediaExporter::DeliveryName(result->delivery),
        result->elapsed.count(),
        result->outputBytes,
        result->success ? L"true" : L"false",
        result->cancelled ? L"true" : L"false",
        static_cast<std::uint32_t>(result->nativeError),
        detail,
        error);
    const std::wstring debugLine =
        L"[SuperRecording.MediaExport] " + result->diagnosticSummary + L"\n";
    ::OutputDebugStringW(debugLine.c_str());
}

}  // namespace

MediaExporter::~MediaExporter() {
    CancelAndWait();
}

bool MediaExporter::StartExport(
    ExportRequest request,
    ProgressCallback progress,
    CompletionCallback completed) {
    std::jthread previousWorker;
    {
        std::scoped_lock lock(workerMutex_);
        if (running_.load(std::memory_order_acquire)) {
            return false;
        }
        if (worker_.joinable()) {
            if (worker_.get_id() == std::this_thread::get_id()) {
                worker_.detach();
            } else {
                previousWorker = std::move(worker_);
            }
        }
        running_.store(true, std::memory_order_release);
        const std::uint64_t workerGeneration = workerGeneration_.fetch_add(
            1,
            std::memory_order_acq_rel) + 1;
        try {
            worker_ = std::jthread(
                [this,
                 workerGeneration,
                 request = std::move(request),
                 progress = std::move(progress),
                 completed = std::move(completed)](
                    const std::stop_token stopToken) mutable {
            MediaExportResult result;
            const auto exportStarted = std::chrono::steady_clock::now();
            try {
                result = RunExport(request, progress, stopToken);
            } catch (const std::exception& exception) {
                result.success = false;
                result.nativeError = E_FAIL;
                result.outputPath = request.destinationPath;
                result.errorMessage = L"导出发生异常：";
                const std::string message = exception.what();
                result.errorMessage.append(message.begin(), message.end());
            } catch (...) {
                result.success = false;
                result.nativeError = E_FAIL;
                result.outputPath = request.destinationPath;
                result.errorMessage = L"导出发生未知异常。";
            }
            result.elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - exportStarted);
            CompleteDiagnostics(request, &result);
            if (workerGeneration_.load(std::memory_order_acquire) ==
                workerGeneration) {
                running_.store(false, std::memory_order_release);
            }
            if (completed) {
                try {
                    completed(result);
                } catch (...) {
                    // 回调异常不能终止工作线程或进程。
                }
            }
                });
        } catch (...) {
            running_.store(false, std::memory_order_release);
            return false;
        }
    }
    if (previousWorker.joinable()) {
        previousWorker.join();
    }
    return true;
}

void MediaExporter::Cancel() noexcept {
    std::scoped_lock lock(workerMutex_);
    if (worker_.joinable()) {
        worker_.request_stop();
    }
}

void MediaExporter::CancelAndWait() noexcept {
    std::jthread workerToJoin;
    bool calledFromWorker = false;
    std::uint64_t cancelledGeneration = 0;
    {
        std::scoped_lock lock(workerMutex_);
        if (worker_.joinable()) {
            cancelledGeneration = workerGeneration_.load(
                std::memory_order_acquire);
            worker_.request_stop();
            calledFromWorker = worker_.get_id() == std::this_thread::get_id();
            if (calledFromWorker) {
                worker_.detach();
            } else {
                workerToJoin = std::move(worker_);
            }
        }
    }
    if (workerToJoin.joinable()) {
        workerToJoin.join();
    }
    if (!calledFromWorker &&
        workerGeneration_.load(std::memory_order_acquire) ==
            cancelledGeneration) {
        running_.store(false, std::memory_order_release);
    }
}

bool MediaExporter::IsRunning() const noexcept {
    return running_.load(std::memory_order_acquire);
}

MediaExportResult MediaExporter::WarmCache(
    const ExportRequest& request,
    const ProgressCallback& progress,
    const std::stop_token stopToken) {
    const auto started = std::chrono::steady_clock::now();
    MediaExportResult result{};
    try {
        result = PrepareCachedArtifact(request, progress, stopToken);
    } catch (const std::exception& exception) {
        result.nativeError = E_FAIL;
        result.errorMessage = L"预生成导出缓存时发生异常：";
        const std::string message = exception.what();
        result.errorMessage.append(message.begin(), message.end());
    } catch (...) {
        result.nativeError = E_FAIL;
        result.errorMessage = L"预生成导出缓存时发生未知异常。";
    }
    result.elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - started);
    CompleteDiagnostics(request, &result);
    return result;
}

std::wstring_view MediaExporter::DispositionName(
    const MediaExportDisposition disposition) noexcept {
    switch (disposition) {
    case MediaExportDisposition::None:
        return L"None";
    case MediaExportDisposition::Transcoded:
        return L"Transcoded";
    case MediaExportDisposition::AudioMuxed:
        return L"AudioMuxed";
    case MediaExportDisposition::BoundaryTrimmedHybrid:
        return L"BoundaryTrimmedHybrid";
    case MediaExportDisposition::SmartTrimmedPassthrough:
        return L"SmartTrimmedPassthrough";
    case MediaExportDisposition::HardLinkedPassthrough:
        return L"HardLinkedPassthrough";
    case MediaExportDisposition::CopiedPassthrough:
        return L"CopiedPassthrough";
    case MediaExportDisposition::OriginalPassthrough:
        return L"OriginalPassthrough";
    case MediaExportDisposition::CachedArtifact:
        return L"CachedArtifact";
    }
    return L"Unknown";
}

std::wstring_view MediaExporter::DeliveryName(
    const MediaArtifactDelivery delivery) noexcept {
    switch (delivery) {
    case MediaArtifactDelivery::None:
        return L"None";
    case MediaArtifactDelivery::HardLinked:
        return L"HardLinked";
    case MediaArtifactDelivery::Copied:
        return L"Copied";
    }
    return L"Unknown";
}

bool MediaExporter::CanUsePassthrough(const ExportRequest& request) noexcept {
    if (request.includeSystemAudio ||
        !IsWholeRangeMp4(request) || request.destinationPath.empty()) {
        return false;
    }

    try {
        if (LowercaseExtension(request.destinationPath) != L".mp4") {
            return false;
        }
    } catch (...) {
        return false;
    }
    return true;
}

bool MediaExporter::CopyFileToClipboard(
    const HWND owner,
    const std::filesystem::path& filePath,
    std::wstring* errorMessage) {
    std::error_code code;
    const std::filesystem::path absolutePath = std::filesystem::absolute(filePath, code);
    if (code || !std::filesystem::is_regular_file(absolutePath, code)) {
        if (errorMessage != nullptr) {
            *errorMessage = L"要复制的导出文件不存在：" + filePath.wstring();
        }
        return false;
    }

    ClipboardGuard clipboard(owner);
    if (!clipboard.IsOpen()) {
        if (errorMessage != nullptr) {
            *errorMessage = L"剪贴板当前被其他程序占用，请稍后重试。";
        }
        return false;
    }
    if (::EmptyClipboard() == FALSE) {
        if (errorMessage != nullptr) {
            *errorMessage = L"无法清空剪贴板：" + win32::FormatLastError();
        }
        return false;
    }

    const std::wstring pathText = absolutePath.wstring();
    const std::size_t characterCount = pathText.size() + 2;
    const std::size_t allocationSize = sizeof(DROPFILES) + characterCount * sizeof(wchar_t);
    HGLOBAL memory = ::GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, allocationSize);
    if (memory == nullptr) {
        if (errorMessage != nullptr) {
            *errorMessage = L"无法分配文件剪贴板内存。";
        }
        return false;
    }
    auto* dropFiles = static_cast<DROPFILES*>(::GlobalLock(memory));
    if (dropFiles == nullptr) {
        ::GlobalFree(memory);
        if (errorMessage != nullptr) {
            *errorMessage = L"无法锁定文件剪贴板内存。";
        }
        return false;
    }
    dropFiles->pFiles = sizeof(DROPFILES);
    dropFiles->fWide = TRUE;
    auto* destination = reinterpret_cast<wchar_t*>(
        reinterpret_cast<std::byte*>(dropFiles) + sizeof(DROPFILES));
    std::memcpy(
        destination,
        pathText.c_str(),
        (pathText.size() + 1) * sizeof(wchar_t));
    ::GlobalUnlock(memory);

    if (::SetClipboardData(CF_HDROP, memory) == nullptr) {
        ::GlobalFree(memory);
        if (errorMessage != nullptr) {
            *errorMessage = L"无法写入文件剪贴板：" + win32::FormatLastError();
        }
        return false;
    }
    return true;
}

MediaExportResult MediaExporter::RunExport(
    const ExportRequest& request,
    const ProgressCallback& progress,
    const std::stop_token stopToken) {
    MediaExportResult result{};
    result.outputPath = request.destinationPath;

    if (request.recording.sourcePath.empty() || request.destinationPath.empty()) {
        result.nativeError = E_INVALIDARG;
        result.errorMessage = L"源录屏或导出路径为空。";
        return result;
    }
    std::error_code fileError;
    if (!std::filesystem::is_regular_file(request.recording.sourcePath, fileError)) {
        result.nativeError = HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND);
        result.errorMessage = L"源录屏不存在：" + request.recording.sourcePath.wstring();
        return result;
    }
    result.sourceBytes = FileSizeOrZero(request.recording.sourcePath);
    if (PathsReferToSameFile(request.recording.sourcePath, request.destinationPath)) {
        result.nativeError = HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS);
        result.errorMessage = L"导出路径不能覆盖源录屏；请选择其他文件名。";
        return result;
    }

    std::wstring directoryError;
    if (!win32::EnsureDirectory(request.destinationPath.parent_path(), &directoryError)) {
        result.nativeError = E_ACCESSDENIED;
        result.errorMessage = std::move(directoryError);
        return result;
    }

    const std::filesystem::path partialPath = PartialPathFor(request.destinationPath);
    HRESULT exportResult = E_FAIL;
    if (CanUsePassthrough(request)) {
        const auto deliveryStarted = std::chrono::steady_clock::now();
        exportResult = MaterializePassthroughMp4(
            request.recording.sourcePath,
            partialPath,
            progress,
            stopToken,
            &result.disposition,
            &result.delivery);
        result.deliveryElapsed = std::chrono::duration_cast<
            std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - deliveryStarted);
        if (FAILED(exportResult)) {
            result.errorMessage = HResultMessage(
                L"无法快速提交原始 MP4",
                exportResult);
        }
    } else {
        MediaExportResult cached = PrepareCachedArtifact(
            request,
            progress,
            stopToken);
        result.cacheHit = cached.cacheHit;
        result.waitedForCacheBuilder = cached.waitedForCacheBuilder;
        result.cacheBuilderWait = cached.cacheBuilderWait;
        result.cacheGeneration = cached.cacheGeneration;
        result.cacheKey = std::move(cached.cacheKey);
        result.disposition = cached.disposition;
        result.sourceBytes = cached.sourceBytes;
        result.diagnosticSummary = std::move(cached.diagnosticSummary);
        if (!cached.success) {
            result.cancelled = cached.cancelled;
            result.nativeError = cached.nativeError;
            result.errorMessage = std::move(cached.errorMessage);
            return result;
        }
        ReportProgress(progress, 0.975, L"正在提交导出成片…");
        const auto deliveryStarted = std::chrono::steady_clock::now();
        exportResult = ExportArtifactCache::Materialize(
            cached.outputPath,
            partialPath,
            stopToken,
            &result.delivery,
            [&progress](
                const std::uint64_t transferredBytes,
                const std::uint64_t totalBytes) {
                const double fraction = totalBytes > 0
                    ? static_cast<double>(transferredBytes) /
                        static_cast<double>(totalBytes)
                    : 0.0;
                ReportProgress(
                    progress,
                    0.975 + fraction * 0.02,
                    L"正在复制导出成片…");
            });
        result.deliveryElapsed = std::chrono::duration_cast<
            std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - deliveryStarted);
        if (FAILED(exportResult)) {
            result.errorMessage = HResultMessage(
                L"无法提交导出成片",
                exportResult);
        }
    }
    result.nativeError = exportResult;

    if (stopToken.stop_requested() || exportResult == HRESULT_FROM_WIN32(ERROR_CANCELLED)) {
        result.cancelled = true;
        result.nativeError = HRESULT_FROM_WIN32(ERROR_CANCELLED);
        result.errorMessage = L"导出已取消，源录屏已保留。";
    } else if (SUCCEEDED(exportResult)) {
        if (::MoveFileExW(
                partialPath.c_str(),
                request.destinationPath.c_str(),
                MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != FALSE) {
            result.success = true;
            result.nativeError = S_OK;
            result.outputBytes = FileSizeOrZero(request.destinationPath);
            result.errorMessage.clear();
            ReportProgress(progress, 1.0, L"导出完成");
        } else {
            result.nativeError = HRESULT_FROM_WIN32(::GetLastError());
            result.errorMessage = L"无法写入最终文件：" +
                win32::FormatError(result.nativeError);
        }
    }

    if (!result.success) {
        std::error_code cleanupError;
        std::filesystem::remove(partialPath, cleanupError);
    }
    return result;
}

}  // namespace qrec
