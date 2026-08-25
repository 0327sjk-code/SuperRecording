#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "AacAudioWriter.h"

#include <windows.h>

#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <wrl/client.h>

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <cwctype>
#include <limits>
#include <mutex>
#include <new>
#include <system_error>
#include <utility>

#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfreadwrite.lib")
#pragma comment(lib, "mfuuid.lib")
#pragma comment(lib, "ole32.lib")

namespace qrec::media {
namespace {

using Microsoft::WRL::ComPtr;

inline constexpr std::uint32_t kMinimumSampleRate = 8'000;
inline constexpr std::uint32_t kMaximumSampleRate = 96'000;
inline constexpr std::uint16_t kMinimumChannelCount = 1;
inline constexpr std::uint16_t kMaximumChannelCount = 2;
inline constexpr std::uint16_t kEncodedBitsPerSample = 16;
inline constexpr std::uint32_t kMinimumAacBitrate = 64'000;
inline constexpr std::uint32_t kMaximumAacBitrate = 320'000;
inline constexpr std::uint32_t kMonoAacBitrate = 128'000;
inline constexpr std::uint32_t kStereoAacBitrate = 192'000;
inline constexpr std::uint32_t kBitsPerByte = 8;
inline constexpr std::uint32_t kAacLcProfileLevel2 = 0x29;
inline constexpr std::uint32_t kRawAacPayloadType = 0;

[[nodiscard]] std::wstring HResultText(const HRESULT result) {
    wchar_t* systemText = nullptr;
    const DWORD length = ::FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
            FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr,
        static_cast<DWORD>(result),
        MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
        reinterpret_cast<wchar_t*>(&systemText),
        0,
        nullptr);

    std::wstring text;
    if (length != 0 && systemText != nullptr) {
        text.assign(systemText, length);
        while (!text.empty() && (text.back() == L'\r' || text.back() == L'\n')) {
            text.pop_back();
        }
    }
    if (systemText != nullptr) {
        ::LocalFree(systemText);
    }

    wchar_t code[24]{};
    ::swprintf_s(code, L"0x%08X", static_cast<unsigned int>(result));
    return text.empty() ? std::wstring(code) : text + L" (" + code + L")";
}

void SetFailure(
    std::wstring& errorMessage,
    long& nativeError,
    std::wstring message,
    const HRESULT result) {
    errorMessage = std::move(message);
    nativeError = static_cast<long>(result);
}

[[nodiscard]] bool IsSupportedInputFormat(
    const InterleavedAudioFormat& format) noexcept {
    if (format.sampleRate < kMinimumSampleRate ||
        format.sampleRate > kMaximumSampleRate ||
        format.channelCount < kMinimumChannelCount ||
        format.channelCount > kMaximumChannelCount) {
        return false;
    }

    if (format.encoding == InterleavedAudioEncoding::IeeeFloat) {
        return format.containerBitsPerSample == 32 &&
               format.validBitsPerSample == 32;
    }

    const bool supportedContainer = format.containerBitsPerSample == 16 ||
                                    format.containerBitsPerSample == 24 ||
                                    format.containerBitsPerSample == 32;
    return supportedContainer && format.validBitsPerSample >= 8 &&
           format.validBitsPerSample <= format.containerBitsPerSample;
}

[[nodiscard]] std::uint32_t BytesPerInputSample(
    const InterleavedAudioFormat& format) noexcept {
    return static_cast<std::uint32_t>(format.containerBitsPerSample) /
           kBitsPerByte;
}

[[nodiscard]] std::int32_t ReadSignedPcmContainer(
    const std::byte* source,
    const std::uint16_t containerBits) noexcept {
    if (containerBits == 16) {
        std::int16_t value{};
        std::memcpy(&value, source, sizeof(value));
        return value;
    }
    if (containerBits == 24) {
        const auto byte0 = static_cast<std::uint32_t>(
            std::to_integer<std::uint8_t>(source[0]));
        const auto byte1 = static_cast<std::uint32_t>(
            std::to_integer<std::uint8_t>(source[1]));
        const auto byte2 = static_cast<std::uint32_t>(
            std::to_integer<std::uint8_t>(source[2]));
        std::uint32_t bits = byte0 | (byte1 << 8U) | (byte2 << 16U);
        if ((bits & 0x0080'0000U) != 0) {
            bits |= 0xFF00'0000U;
        }
        return std::bit_cast<std::int32_t>(bits);
    }

    std::int32_t value{};
    std::memcpy(&value, source, sizeof(value));
    return value;
}

[[nodiscard]] std::int16_t ConvertSignedPcmToInt16(
    const std::byte* source,
    const InterleavedAudioFormat& format) noexcept {
    std::int32_t sample = ReadSignedPcmContainer(
        source,
        format.containerBitsPerSample);

    const unsigned int alignmentShift = static_cast<unsigned int>(
        format.containerBitsPerSample - format.validBitsPerSample);
    if (alignmentShift != 0) {
        sample >>= alignmentShift;
    }

    if (format.validBitsPerSample > kEncodedBitsPerSample) {
        sample >>= static_cast<unsigned int>(
            format.validBitsPerSample - kEncodedBitsPerSample);
    } else if (format.validBitsPerSample < kEncodedBitsPerSample) {
        sample *= static_cast<std::int32_t>(
            1U << static_cast<unsigned int>(
                kEncodedBitsPerSample - format.validBitsPerSample));
    }

    sample = std::clamp(
        sample,
        static_cast<std::int32_t>(std::numeric_limits<std::int16_t>::min()),
        static_cast<std::int32_t>(std::numeric_limits<std::int16_t>::max()));
    return static_cast<std::int16_t>(sample);
}

[[nodiscard]] std::int16_t ConvertFloatToInt16(
    const std::byte* source) noexcept {
    float value{};
    std::memcpy(&value, source, sizeof(value));
    if (!std::isfinite(value)) {
        return 0;
    }
    if (value <= -1.0F) {
        return std::numeric_limits<std::int16_t>::min();
    }
    if (value >= 1.0F) {
        return std::numeric_limits<std::int16_t>::max();
    }
    return static_cast<std::int16_t>(std::lround(value * 32'767.0F));
}

[[nodiscard]] std::int16_t ConvertInputSample(
    const std::byte* source,
    const InterleavedAudioFormat& format) noexcept {
    return format.encoding == InterleavedAudioEncoding::IeeeFloat
               ? ConvertFloatToInt16(source)
               : ConvertSignedPcmToInt16(source, format);
}

}  // namespace

struct AacAudioWriter::Impl final {
    mutable std::mutex mutex;
    ComPtr<IMFSinkWriter> writer;
    DWORD streamIndex{};
    AacAudioWriterConfig config{};
    std::uint32_t selectedBitrate{};
    std::int64_t nextTimestamp{};
    bool mediaFoundationStarted{};
    bool open{};

    void ShutdownUnlocked() noexcept {
        writer.Reset();
        streamIndex = 0;
        selectedBitrate = 0;
        nextTimestamp = 0;
        open = false;
        if (mediaFoundationStarted) {
            ::MFShutdown();
            mediaFoundationStarted = false;
        }
    }

    [[nodiscard]] bool WriteFramesUnlocked(
        const std::span<const std::byte> interleavedSamples,
        const std::uint32_t frameCount,
        const bool silent,
        const std::int64_t timestamp100Nanoseconds,
        const std::int64_t duration100Nanoseconds,
        std::wstring& errorMessage,
        long& nativeError) noexcept {
        if (!open || !writer) {
            SetFailure(
                errorMessage,
                nativeError,
                L"AAC 写入器尚未打开。",
                MF_E_INVALIDREQUEST);
            return false;
        }
        if (frameCount == 0 || timestamp100Nanoseconds != nextTimestamp ||
            duration100Nanoseconds <= 0 ||
            duration100Nanoseconds >
                std::numeric_limits<std::int64_t>::max() -
                    timestamp100Nanoseconds) {
            SetFailure(
                errorMessage,
                nativeError,
                L"音频帧数量或连续时间戳无效。",
                E_INVALIDARG);
            return false;
        }

        const std::uint64_t sampleCount64 =
            static_cast<std::uint64_t>(frameCount) *
            config.inputFormat.channelCount;
        const std::uint64_t sourceBytes64 =
            sampleCount64 * BytesPerInputSample(config.inputFormat);
        const std::uint64_t destinationBytes64 =
            sampleCount64 * sizeof(std::int16_t);
        if ((!silent && interleavedSamples.size() < sourceBytes64) ||
            destinationBytes64 > std::numeric_limits<DWORD>::max()) {
            SetFailure(
                errorMessage,
                nativeError,
                L"交错音频缓冲区长度不足或帧尺寸过大。",
                E_INVALIDARG);
            return false;
        }

        ComPtr<IMFMediaBuffer> mediaBuffer;
        HRESULT result = ::MFCreateMemoryBuffer(
            static_cast<DWORD>(destinationBytes64),
            &mediaBuffer);
        if (FAILED(result)) {
            SetFailure(
                errorMessage,
                nativeError,
                L"分配 AAC 输入缓冲区失败：" + HResultText(result),
                result);
            return false;
        }

        BYTE* destination = nullptr;
        DWORD maximumLength = 0;
        result = mediaBuffer->Lock(&destination, &maximumLength, nullptr);
        if (FAILED(result)) {
            SetFailure(
                errorMessage,
                nativeError,
                L"锁定 AAC 输入缓冲区失败：" + HResultText(result),
                result);
            return false;
        }

        struct BufferUnlock final {
            IMFMediaBuffer* buffer{};
            void Unlock() noexcept {
                if (buffer != nullptr) {
                    static_cast<void>(buffer->Unlock());
                    buffer = nullptr;
                }
            }
            ~BufferUnlock() {
                Unlock();
            }
        } bufferUnlock{mediaBuffer.Get()};

        if (maximumLength < destinationBytes64) {
            SetFailure(
                errorMessage,
                nativeError,
                L"AAC 输入缓冲区容量不足。",
                E_UNEXPECTED);
            return false;
        }

        if (silent) {
            std::memset(destination, 0, static_cast<std::size_t>(destinationBytes64));
        } else {
            const std::uint32_t sourceSampleBytes =
                BytesPerInputSample(config.inputFormat);
            for (std::uint64_t sampleIndex = 0;
                 sampleIndex < sampleCount64;
                 ++sampleIndex) {
                const std::byte* source = interleavedSamples.data() +
                    static_cast<std::size_t>(sampleIndex * sourceSampleBytes);
                const std::int16_t converted =
                    ConvertInputSample(source, config.inputFormat);
                std::memcpy(
                    destination + sampleIndex * sizeof(converted),
                    &converted,
                    sizeof(converted));
            }
        }

        result = mediaBuffer->SetCurrentLength(
            static_cast<DWORD>(destinationBytes64));
        if (FAILED(result)) {
            SetFailure(
                errorMessage,
                nativeError,
                L"设置 AAC 输入缓冲区长度失败：" + HResultText(result),
                result);
            return false;
        }
        bufferUnlock.Unlock();

        ComPtr<IMFSample> sample;
        result = ::MFCreateSample(&sample);
        if (SUCCEEDED(result)) {
            result = sample->AddBuffer(mediaBuffer.Get());
        }
        if (SUCCEEDED(result)) {
            result = sample->SetSampleTime(timestamp100Nanoseconds);
        }
        if (SUCCEEDED(result)) {
            result = sample->SetSampleDuration(duration100Nanoseconds);
        }
        if (SUCCEEDED(result)) {
            result = writer->WriteSample(streamIndex, sample.Get());
        }
        if (FAILED(result)) {
            SetFailure(
                errorMessage,
                nativeError,
                L"写入 AAC 音频样本失败：" + HResultText(result),
                result);
            return false;
        }

        nextTimestamp = timestamp100Nanoseconds + duration100Nanoseconds;
        return true;
    }
};

AacAudioWriter::AacAudioWriter() : impl_(std::make_unique<Impl>()) {}

AacAudioWriter::~AacAudioWriter() {
    std::wstring ignoredMessage;
    long ignoredError = 0;
    if (!Finalize(ignoredMessage, ignoredError)) {
        std::scoped_lock lock(impl_->mutex);
        impl_->ShutdownUnlocked();
    }
}

std::uint32_t AacAudioWriter::RecommendBitrate(
    const std::uint16_t channelCount) noexcept {
    if (channelCount == 1) {
        return kMonoAacBitrate;
    }
    if (channelCount == 2) {
        return kStereoAacBitrate;
    }
    return 0;
}

bool AacAudioWriter::Open(
    const AacAudioWriterConfig& config,
    std::wstring& errorMessage,
    long& nativeError) noexcept {
    std::scoped_lock lock(impl_->mutex);
    errorMessage.clear();
    nativeError = 0;

    if (impl_->open) {
        SetFailure(
            errorMessage,
            nativeError,
            L"AAC 写入器已经打开。",
            MF_E_INVALIDREQUEST);
        return false;
    }
    impl_->ShutdownUnlocked();

    const std::uint32_t selectedBitrate = config.averageBitrate != 0
                                              ? config.averageBitrate
                                              : RecommendBitrate(
                                                    config.inputFormat.channelCount);
    if (config.outputPath.empty() ||
        !IsSupportedInputFormat(config.inputFormat) ||
        selectedBitrate < kMinimumAacBitrate ||
        selectedBitrate > kMaximumAacBitrate ||
        selectedBitrate % kBitsPerByte != 0) {
        SetFailure(
            errorMessage,
            nativeError,
            L"AAC 输出路径、采样格式或码率无效。",
            E_INVALIDARG);
        return false;
    }

    std::wstring extension = config.outputPath.extension().wstring();
    std::transform(
        extension.begin(),
        extension.end(),
        extension.begin(),
        [](const wchar_t character) {
            return static_cast<wchar_t>(std::towlower(character));
        });
    if (extension != L".m4a") {
        SetFailure(
            errorMessage,
            nativeError,
            L"AAC 音频输出路径必须使用 .m4a 扩展名。",
            E_INVALIDARG);
        return false;
    }

    const std::filesystem::path parent = config.outputPath.parent_path();
    if (!parent.empty()) {
        std::error_code directoryError;
        std::filesystem::create_directories(parent, directoryError);
        if (directoryError) {
            errorMessage = L"创建 M4A 输出目录失败：" + parent.wstring();
            nativeError = static_cast<long>(directoryError.value());
            return false;
        }
    }

    HRESULT result = ::MFStartup(MF_VERSION, MFSTARTUP_FULL);
    if (FAILED(result)) {
        SetFailure(
            errorMessage,
            nativeError,
            L"启动 Media Foundation 音频编码失败：" + HResultText(result),
            result);
        return false;
    }
    impl_->mediaFoundationStarted = true;

    ComPtr<IMFAttributes> writerAttributes;
    result = ::MFCreateAttributes(&writerAttributes, 3);
    if (SUCCEEDED(result)) {
        result = writerAttributes->SetUINT32(
            MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS,
            config.preferHardwareEncoder ? TRUE : FALSE);
    }
    if (SUCCEEDED(result)) {
        result = writerAttributes->SetUINT32(
            MF_SINK_WRITER_DISABLE_THROTTLING,
            TRUE);
    }
    if (SUCCEEDED(result)) {
        result = writerAttributes->SetUINT32(MF_LOW_LATENCY, TRUE);
    }
    if (FAILED(result)) {
        SetFailure(
            errorMessage,
            nativeError,
            L"配置 Media Foundation AAC 写入器失败：" + HResultText(result),
            result);
        impl_->ShutdownUnlocked();
        return false;
    }

    result = ::MFCreateSinkWriterFromURL(
        config.outputPath.c_str(),
        nullptr,
        writerAttributes.Get(),
        &impl_->writer);
    if (FAILED(result)) {
        SetFailure(
            errorMessage,
            nativeError,
            L"创建 M4A 文件失败：" + HResultText(result),
            result);
        impl_->ShutdownUnlocked();
        return false;
    }

    ComPtr<IMFMediaType> outputType;
    result = ::MFCreateMediaType(&outputType);
    if (SUCCEEDED(result)) {
        result = outputType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
    }
    if (SUCCEEDED(result)) {
        result = outputType->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_AAC);
    }
    if (SUCCEEDED(result)) {
        result = outputType->SetUINT32(
            MF_MT_AUDIO_NUM_CHANNELS,
            config.inputFormat.channelCount);
    }
    if (SUCCEEDED(result)) {
        result = outputType->SetUINT32(
            MF_MT_AUDIO_SAMPLES_PER_SECOND,
            config.inputFormat.sampleRate);
    }
    if (SUCCEEDED(result)) {
        result = outputType->SetUINT32(
            MF_MT_AUDIO_BITS_PER_SAMPLE,
            kEncodedBitsPerSample);
    }
    if (SUCCEEDED(result)) {
        result = outputType->SetUINT32(
            MF_MT_AUDIO_AVG_BYTES_PER_SECOND,
            selectedBitrate / kBitsPerByte);
    }
    if (SUCCEEDED(result)) {
        result = outputType->SetUINT32(
            MF_MT_AAC_PAYLOAD_TYPE,
            kRawAacPayloadType);
    }
    if (SUCCEEDED(result)) {
        result = outputType->SetUINT32(
            MF_MT_AAC_AUDIO_PROFILE_LEVEL_INDICATION,
            kAacLcProfileLevel2);
    }
    if (SUCCEEDED(result)) {
        result = impl_->writer->AddStream(outputType.Get(), &impl_->streamIndex);
    }
    if (FAILED(result)) {
        SetFailure(
            errorMessage,
            nativeError,
            L"创建 AAC-LC 输出流失败：" + HResultText(result),
            result);
        impl_->ShutdownUnlocked();
        return false;
    }

    const std::uint32_t encodedBlockAlignment =
        static_cast<std::uint32_t>(config.inputFormat.channelCount) *
        kEncodedBitsPerSample / kBitsPerByte;
    const std::uint64_t encodedBytesPerSecond64 =
        static_cast<std::uint64_t>(config.inputFormat.sampleRate) *
        encodedBlockAlignment;
    if (encodedBytesPerSecond64 > std::numeric_limits<std::uint32_t>::max()) {
        SetFailure(
            errorMessage,
            nativeError,
            L"PCM 输入字节率超出 Media Foundation 支持范围。",
            E_INVALIDARG);
        impl_->ShutdownUnlocked();
        return false;
    }

    ComPtr<IMFMediaType> inputType;
    result = ::MFCreateMediaType(&inputType);
    if (SUCCEEDED(result)) {
        result = inputType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
    }
    if (SUCCEEDED(result)) {
        result = inputType->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM);
    }
    if (SUCCEEDED(result)) {
        result = inputType->SetUINT32(
            MF_MT_AUDIO_NUM_CHANNELS,
            config.inputFormat.channelCount);
    }
    if (SUCCEEDED(result)) {
        result = inputType->SetUINT32(
            MF_MT_AUDIO_SAMPLES_PER_SECOND,
            config.inputFormat.sampleRate);
    }
    if (SUCCEEDED(result)) {
        result = inputType->SetUINT32(
            MF_MT_AUDIO_BITS_PER_SAMPLE,
            kEncodedBitsPerSample);
    }
    if (SUCCEEDED(result)) {
        result = inputType->SetUINT32(
            MF_MT_AUDIO_BLOCK_ALIGNMENT,
            encodedBlockAlignment);
    }
    if (SUCCEEDED(result)) {
        result = inputType->SetUINT32(
            MF_MT_AUDIO_AVG_BYTES_PER_SECOND,
            static_cast<std::uint32_t>(encodedBytesPerSecond64));
    }
    if (SUCCEEDED(result)) {
        result = inputType->SetUINT32(MF_MT_FIXED_SIZE_SAMPLES, TRUE);
    }
    if (SUCCEEDED(result)) {
        result = inputType->SetUINT32(MF_MT_ALL_SAMPLES_INDEPENDENT, TRUE);
    }
    if (SUCCEEDED(result) && config.inputFormat.channelMask != 0) {
        result = inputType->SetUINT32(
            MF_MT_AUDIO_CHANNEL_MASK,
            config.inputFormat.channelMask);
    }
    if (SUCCEEDED(result)) {
        result = impl_->writer->SetInputMediaType(
            impl_->streamIndex,
            inputType.Get(),
            nullptr);
    }
    if (FAILED(result)) {
        SetFailure(
            errorMessage,
            nativeError,
            L"配置 PCM 到 AAC 的实时编码链失败：" + HResultText(result),
            result);
        impl_->ShutdownUnlocked();
        return false;
    }

    result = impl_->writer->BeginWriting();
    if (FAILED(result)) {
        SetFailure(
            errorMessage,
            nativeError,
            L"启动 AAC 实时编码失败：" + HResultText(result),
            result);
        impl_->ShutdownUnlocked();
        return false;
    }

    impl_->config = config;
    impl_->config.averageBitrate = selectedBitrate;
    impl_->selectedBitrate = selectedBitrate;
    impl_->nextTimestamp = 0;
    impl_->open = true;
    return true;
}

bool AacAudioWriter::WriteInterleavedFrames(
    const std::span<const std::byte> interleavedSamples,
    const std::uint32_t frameCount,
    const std::int64_t timestamp100Nanoseconds,
    const std::int64_t duration100Nanoseconds,
    std::wstring& errorMessage,
    long& nativeError) noexcept {
    std::scoped_lock lock(impl_->mutex);
    errorMessage.clear();
    nativeError = 0;
    return impl_->WriteFramesUnlocked(
        interleavedSamples,
        frameCount,
        false,
        timestamp100Nanoseconds,
        duration100Nanoseconds,
        errorMessage,
        nativeError);
}

bool AacAudioWriter::WriteSilentFrames(
    const std::uint32_t frameCount,
    const std::int64_t timestamp100Nanoseconds,
    const std::int64_t duration100Nanoseconds,
    std::wstring& errorMessage,
    long& nativeError) noexcept {
    std::scoped_lock lock(impl_->mutex);
    errorMessage.clear();
    nativeError = 0;
    return impl_->WriteFramesUnlocked(
        {},
        frameCount,
        true,
        timestamp100Nanoseconds,
        duration100Nanoseconds,
        errorMessage,
        nativeError);
}

bool AacAudioWriter::Finalize(
    std::wstring& errorMessage,
    long& nativeError) noexcept {
    std::scoped_lock lock(impl_->mutex);
    errorMessage.clear();
    nativeError = 0;
    if (!impl_->open || !impl_->writer) {
        return true;
    }

    const HRESULT result = impl_->writer->Finalize();
    impl_->ShutdownUnlocked();
    if (FAILED(result)) {
        SetFailure(
            errorMessage,
            nativeError,
            L"完成 M4A 封装失败：" + HResultText(result),
            result);
        return false;
    }
    return true;
}

bool AacAudioWriter::IsOpen() const noexcept {
    std::scoped_lock lock(impl_->mutex);
    return impl_->open;
}

std::uint32_t AacAudioWriter::AverageBitrate() const noexcept {
    std::scoped_lock lock(impl_->mutex);
    return impl_->selectedBitrate;
}

InterleavedAudioFormat AacAudioWriter::InputFormat() const noexcept {
    std::scoped_lock lock(impl_->mutex);
    return impl_->config.inputFormat;
}

}  // namespace qrec::media
