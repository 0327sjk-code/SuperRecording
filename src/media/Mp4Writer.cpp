#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "Mp4Writer.h"

#include <windows.h>

#include <strmif.h>
#include <codecapi.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <oleauto.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <mutex>
#include <system_error>

#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfreadwrite.lib")
#pragma comment(lib, "mfuuid.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "oleaut32.lib")

namespace qrec::media {
namespace {

using Microsoft::WRL::ComPtr;

inline constexpr std::uint32_t kMinimumBitrate = 3'000'000;
inline constexpr std::uint32_t kMaximumBitrate = 100'000'000;
inline constexpr std::uint32_t kBitrateStep = 250'000;
inline constexpr long double kBitsPerPixelAt60Fps = 0.160L;
inline constexpr long double kBitsPerPixelAt30Fps = 0.190L;
inline constexpr std::uint32_t kPeakBitratePercent = 145;
inline constexpr std::uint32_t kQualityVersusSpeed = 30;
inline constexpr std::uint32_t kMinimumKeyframeIntervalMilliseconds = 50;
inline constexpr std::uint32_t kMaximumKeyframeIntervalMilliseconds = 10'000;
inline constexpr std::int64_t kHundredNanosecondsPerMillisecond = 10'000;

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
    if (text.empty()) {
        return code;
    }
    return text + L" (" + code + L")";
}

void SetFailure(
    std::wstring& errorMessage,
    long& nativeError,
    std::wstring message,
    const HRESULT result) {
    errorMessage = std::move(message);
    nativeError = static_cast<long>(result);
}

enum class CodecSettingOutcome {
    Applied,
    Unsupported,
    Rejected,
    ReadbackUnavailable,
    Mismatch,
};

struct CodecSettingResult final {
    CodecSettingOutcome outcome{CodecSettingOutcome::Rejected};
    HRESULT nativeError{E_FAIL};
    std::wstring expectedValue;
    std::wstring actualValue;
};

[[nodiscard]] bool IsUnsupportedCodecResult(const HRESULT result) noexcept {
    return result == E_NOTIMPL || result == E_NOINTERFACE ||
           result == HRESULT_FROM_WIN32(ERROR_SET_NOT_FOUND) ||
           result == HRESULT_FROM_WIN32(ERROR_NOT_FOUND) ||
           result == HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED);
}

[[nodiscard]] VARIANT MakeUnsignedCodecValue(const std::uint32_t value) noexcept {
    VARIANT variant;
    ::VariantInit(&variant);
    variant.vt = VT_UI4;
    variant.ulVal = value;
    return variant;
}

[[nodiscard]] VARIANT MakeBooleanCodecValue(const bool value) noexcept {
    VARIANT variant;
    ::VariantInit(&variant);
    variant.vt = VT_BOOL;
    variant.boolVal = value ? VARIANT_TRUE : VARIANT_FALSE;
    return variant;
}

[[nodiscard]] std::wstring CodecValueText(const VARIANT& value) {
    switch (value.vt) {
    case VT_UI4:
        return std::to_wstring(value.ulVal);
    case VT_I4:
        return std::to_wstring(value.lVal);
    case VT_BOOL:
        return value.boolVal != VARIANT_FALSE ? L"TRUE" : L"FALSE";
    default:
        return L"VT=" + std::to_wstring(static_cast<unsigned int>(value.vt));
    }
}

[[nodiscard]] bool CodecValuesMatch(
    const VARIANT& expected,
    const VARIANT& actual) noexcept {
    if (expected.vt == VT_UI4) {
        if (actual.vt == VT_UI4) {
            return actual.ulVal == expected.ulVal;
        }
        return actual.vt == VT_I4 && actual.lVal >= 0 &&
               static_cast<std::uint32_t>(actual.lVal) == expected.ulVal;
    }
    if (expected.vt == VT_BOOL) {
        const bool expectedBoolean = expected.boolVal != VARIANT_FALSE;
        if (actual.vt == VT_BOOL) {
            return (actual.boolVal != VARIANT_FALSE) == expectedBoolean;
        }
        if (actual.vt == VT_UI4) {
            return (actual.ulVal != 0) == expectedBoolean;
        }
        if (actual.vt == VT_I4) {
            return (actual.lVal != 0) == expectedBoolean;
        }
    }
    return false;
}

[[nodiscard]] CodecSettingResult SetCodecValueAndReadBack(
    ICodecAPI* codec,
    const GUID& key,
    VARIANT desiredValue) {
    CodecSettingResult setting{};
    setting.expectedValue = CodecValueText(desiredValue);
    if (codec == nullptr) {
        setting.nativeError = E_POINTER;
        return setting;
    }

    const HRESULT setResult = codec->SetValue(&key, &desiredValue);
    if (FAILED(setResult)) {
        setting.outcome = IsUnsupportedCodecResult(setResult)
                              ? CodecSettingOutcome::Unsupported
                              : CodecSettingOutcome::Rejected;
        setting.nativeError = setResult;
        return setting;
    }

    VARIANT actualValue;
    ::VariantInit(&actualValue);
    const HRESULT getResult = codec->GetValue(&key, &actualValue);
    if (FAILED(getResult)) {
        ::VariantClear(&actualValue);
        setting.outcome = IsUnsupportedCodecResult(getResult)
                              ? CodecSettingOutcome::Unsupported
                              : CodecSettingOutcome::ReadbackUnavailable;
        setting.nativeError = getResult;
        return setting;
    }

    setting.actualValue = CodecValueText(actualValue);
    setting.outcome = CodecValuesMatch(desiredValue, actualValue)
                          ? CodecSettingOutcome::Applied
                          : CodecSettingOutcome::Mismatch;
    setting.nativeError = setting.outcome == CodecSettingOutcome::Applied
                              ? S_OK
                              : HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
    ::VariantClear(&actualValue);
    return setting;
}

[[nodiscard]] const wchar_t* CodecSettingOutcomeText(
    const CodecSettingOutcome outcome) noexcept {
    switch (outcome) {
    case CodecSettingOutcome::Applied:
        return L"applied";
    case CodecSettingOutcome::Unsupported:
        return L"unsupported";
    case CodecSettingOutcome::Rejected:
        return L"rejected";
    case CodecSettingOutcome::ReadbackUnavailable:
        return L"readback-unavailable";
    case CodecSettingOutcome::Mismatch:
        return L"readback-mismatch";
    }
    return L"unknown";
}

void TraceCodecSetting(
    const wchar_t* name,
    const CodecSettingResult& setting) {
    std::wstring message = L"[SuperRecording.Mp4Writer] codec-setting name=";
    message += name;
    message += L" outcome=";
    message += CodecSettingOutcomeText(setting.outcome);
    message += L" expected=";
    message += setting.expectedValue;
    if (!setting.actualValue.empty()) {
        message += L" actual=";
        message += setting.actualValue;
    }
    if (FAILED(setting.nativeError)) {
        message += L" result=";
        message += HResultText(setting.nativeError);
    }
    message += L"\n";
    ::OutputDebugStringW(message.c_str());
}

[[nodiscard]] bool ApplyRequiredCodecSetting(
    ICodecAPI* codec,
    const GUID& key,
    const wchar_t* name,
    VARIANT desiredValue,
    std::wstring& errorMessage,
    long& nativeError) {
    const CodecSettingResult setting =
        SetCodecValueAndReadBack(codec, key, desiredValue);
    TraceCodecSetting(name, setting);
    if (setting.outcome != CodecSettingOutcome::Mismatch) {
        return true;
    }

    const HRESULT mismatchError = HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
    SetFailure(
        errorMessage,
        nativeError,
        L"H.264 编码器接受了关键参数“" + std::wstring(name) +
            L"”，但回读值不一致（期望 " + setting.expectedValue +
            L"，实际 " + setting.actualValue +
            L"）；无法保证快速裁剪所需的编码结构。",
        mismatchError);
    return false;
}

void ApplyBestEffortCodecSetting(
    ICodecAPI* codec,
    const GUID& key,
    const wchar_t* name,
    VARIANT desiredValue) {
    const CodecSettingResult setting =
        SetCodecValueAndReadBack(codec, key, desiredValue);
    TraceCodecSetting(name, setting);
}

void TraceForcedKeyframeRequest(
    const HRESULT result,
    const std::uint64_t successfulRequestCount,
    const std::int64_t timestamp100Nanoseconds) noexcept {
    wchar_t message[256]{};
    if (FAILED(result)) {
        ::swprintf_s(
            message,
            L"[SuperRecording.Mp4Writer] forced-keyframe "
            L"timestamp100ns=%lld request=%llu outcome=disabled "
            L"result=0x%08X\n",
            static_cast<long long>(timestamp100Nanoseconds),
            static_cast<unsigned long long>(successfulRequestCount),
            static_cast<unsigned int>(result));
    } else {
        ::swprintf_s(
            message,
            L"[SuperRecording.Mp4Writer] forced-keyframe "
            L"timestamp100ns=%lld request=%llu outcome=accepted\n",
            static_cast<long long>(timestamp100Nanoseconds),
            static_cast<unsigned long long>(successfulRequestCount));
    }
    ::OutputDebugStringW(message);
}

[[nodiscard]] bool ConfigureCodec(
    ICodecAPI* codec,
    const Mp4WriterConfig& config,
    const std::uint32_t selectedBitrate,
    std::wstring& errorMessage,
    long& nativeError) {
    const auto rateControl = static_cast<std::uint32_t>(
        eAVEncCommonRateControlMode_PeakConstrainedVBR);
    ApplyBestEffortCodecSetting(
        codec,
        CODECAPI_AVEncCommonRateControlMode,
        L"RateControlMode",
        MakeUnsignedCodecValue(rateControl));
    ApplyBestEffortCodecSetting(
        codec,
        CODECAPI_AVEncCommonMeanBitRate,
        L"MeanBitRate",
        MakeUnsignedCodecValue(selectedBitrate));

    const std::uint64_t peakBitrate =
        static_cast<std::uint64_t>(selectedBitrate) * kPeakBitratePercent / 100;
    ApplyBestEffortCodecSetting(
        codec,
        CODECAPI_AVEncCommonMaxBitRate,
        L"MaxBitRate",
        MakeUnsignedCodecValue(static_cast<std::uint32_t>(
            std::min<std::uint64_t>(
                peakBitrate,
                std::numeric_limits<std::uint32_t>::max()))));
    ApplyBestEffortCodecSetting(
        codec,
        CODECAPI_AVEncCommonQualityVsSpeed,
        L"QualityVsSpeed",
        MakeUnsignedCodecValue(kQualityVersusSpeed));
    ApplyBestEffortCodecSetting(
        codec,
        CODECAPI_AVEncCommonLowLatency,
        L"LowLatency",
        MakeBooleanCodecValue(true));
    ApplyBestEffortCodecSetting(
        codec,
        CODECAPI_AVEncCommonRealTime,
        L"RealTime",
        MakeBooleanCodecValue(true));

    // GOP length is only a negotiation preference. Some Windows hardware
    // encoders legally widen the requested GOP. The
    // one-shot requests issued while frames are written provide the tighter
    // real-time bound; this value remains a best-effort encoder fallback.
    const std::uint64_t requestedGopFrames =
        (static_cast<std::uint64_t>(config.framesPerSecond) *
             config.forcedKeyframeIntervalMilliseconds +
         999ULL) /
        1'000ULL;
    ApplyBestEffortCodecSetting(
        codec,
        CODECAPI_AVEncMPVGOPSize,
        L"GOPSize",
        MakeUnsignedCodecValue(
            static_cast<std::uint32_t>(std::max<std::uint64_t>(
                1ULL,
                requestedGopFrames))));
    if (!ApplyRequiredCodecSetting(
            codec,
            CODECAPI_AVEncMPVDefaultBPictureCount,
            L"BPictureCount",
            MakeUnsignedCodecValue(0),
            errorMessage,
            nativeError)) {
        return false;
    }
    return ApplyRequiredCodecSetting(
        codec,
        CODECAPI_AVEncMPVGOPOpen,
        L"GOPOpen",
        MakeBooleanCodecValue(false),
        errorMessage,
        nativeError);
}

}  // namespace

struct Mp4Writer::Impl final {
    mutable std::mutex mutex;
    ComPtr<IMFSinkWriter> writer;
    ComPtr<ICodecAPI> codec;
    DWORD streamIndex{};
    Mp4WriterConfig config{};
    std::uint32_t selectedBitrate{};
    std::int64_t lastTimestamp{-1};
    std::int64_t nextForcedKeyframeTimestamp{-1};
    std::uint64_t successfulForcedKeyframeRequests{};
    bool forcedKeyframeRequestsEnabled{};
    bool forcedKeyframeAcceptanceTraced{};
    bool mediaFoundationStarted{};
    bool open{};

    void ScheduleNextForcedKeyframeUnlocked(
        const std::int64_t currentTimestamp) noexcept {
        const std::int64_t interval =
            static_cast<std::int64_t>(
                config.forcedKeyframeIntervalMilliseconds) *
            kHundredNanosecondsPerMillisecond;
        if (currentTimestamp >
            std::numeric_limits<std::int64_t>::max() -
                interval) {
            nextForcedKeyframeTimestamp =
                std::numeric_limits<std::int64_t>::max();
            return;
        }
        nextForcedKeyframeTimestamp = currentTimestamp + interval;
    }

    void RequestForcedKeyframeUnlocked(
        const std::int64_t timestamp100Nanoseconds) noexcept {
        if (!forcedKeyframeRequestsEnabled || !codec) {
            return;
        }

        VARIANT request = MakeUnsignedCodecValue(1);
        const HRESULT result = codec->SetValue(
            &CODECAPI_AVEncVideoForceKeyFrame,
            &request);
        if (FAILED(result)) {
            forcedKeyframeRequestsEnabled = false;
            TraceForcedKeyframeRequest(
                result,
                successfulForcedKeyframeRequests,
                timestamp100Nanoseconds);
            return;
        }

        ++successfulForcedKeyframeRequests;
        if (!forcedKeyframeAcceptanceTraced) {
            forcedKeyframeAcceptanceTraced = true;
            TraceForcedKeyframeRequest(
                S_OK,
                successfulForcedKeyframeRequests,
                timestamp100Nanoseconds);
        }
    }

    void ShutdownUnlocked() noexcept {
        codec.Reset();
        writer.Reset();
        open = false;
        lastTimestamp = -1;
        nextForcedKeyframeTimestamp = -1;
        successfulForcedKeyframeRequests = 0;
        forcedKeyframeRequestsEnabled = false;
        forcedKeyframeAcceptanceTraced = false;
        if (mediaFoundationStarted) {
            ::MFShutdown();
            mediaFoundationStarted = false;
        }
    }
};

Mp4Writer::Mp4Writer() : impl_(std::make_unique<Impl>()) {}

Mp4Writer::~Mp4Writer() {
    std::wstring ignoredMessage;
    long ignoredError = 0;
    if (!Finalize(ignoredMessage, ignoredError)) {
        std::scoped_lock lock(impl_->mutex);
        impl_->ShutdownUnlocked();
    }
}

std::uint32_t Mp4Writer::RecommendBitrate(
    const std::uint32_t width,
    const std::uint32_t height,
    const int framesPerSecond) noexcept {
    if (width == 0 || height == 0 || framesPerSecond <= 0) {
        return 0;
    }

    const long double bitsPerPixel = framesPerSecond >= 60
                                         ? kBitsPerPixelAt60Fps
                                         : kBitsPerPixelAt30Fps;
    const long double calculated = static_cast<long double>(width) *
                                   static_cast<long double>(height) *
                                   static_cast<long double>(framesPerSecond) * bitsPerPixel;
    const auto bounded = static_cast<std::uint64_t>(std::clamp(
        calculated,
        static_cast<long double>(kMinimumBitrate),
        static_cast<long double>(kMaximumBitrate)));
    const std::uint64_t rounded =
        ((bounded + kBitrateStep / 2) / kBitrateStep) * kBitrateStep;
    return static_cast<std::uint32_t>(
        std::min<std::uint64_t>(rounded, kMaximumBitrate));
}

bool Mp4Writer::Open(
    const Mp4WriterConfig& config,
    std::wstring& errorMessage,
    long& nativeError) noexcept {
    std::scoped_lock lock(impl_->mutex);
    errorMessage.clear();
    nativeError = 0;

    if (impl_->open) {
        SetFailure(errorMessage, nativeError, L"MP4 写入器已经打开。", MF_E_INVALIDREQUEST);
        return false;
    }
    impl_->ShutdownUnlocked();

    if (config.outputPath.empty() || config.width < 16 || config.height < 16 ||
        config.width % 2 != 0 || config.height % 2 != 0 ||
        (config.framesPerSecond != 30 && config.framesPerSecond != 60) ||
        config.forcedKeyframeIntervalMilliseconds <
            kMinimumKeyframeIntervalMilliseconds ||
        config.forcedKeyframeIntervalMilliseconds >
            kMaximumKeyframeIntervalMilliseconds ||
        config.width > std::numeric_limits<std::uint32_t>::max() / 4) {
        SetFailure(errorMessage, nativeError, L"MP4 输出参数无效。", E_INVALIDARG);
        return false;
    }

    std::wstring extension = config.outputPath.extension().wstring();
    std::transform(extension.begin(), extension.end(), extension.begin(), ::towlower);
    if (extension != L".mp4") {
        SetFailure(errorMessage, nativeError, L"MP4 输出路径必须使用 .mp4 扩展名。", E_INVALIDARG);
        return false;
    }

    const std::filesystem::path parent = config.outputPath.parent_path();
    if (!parent.empty()) {
        std::error_code directoryError;
        std::filesystem::create_directories(parent, directoryError);
        if (directoryError) {
            errorMessage = L"创建 MP4 输出目录失败：" + parent.wstring();
            nativeError = static_cast<long>(directoryError.value());
            return false;
        }
    }

    HRESULT result = ::MFStartup(MF_VERSION, MFSTARTUP_FULL);
    if (FAILED(result)) {
        SetFailure(
            errorMessage,
            nativeError,
            L"启动 Media Foundation 失败：" + HResultText(result),
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
        result = writerAttributes->SetUINT32(MF_SINK_WRITER_DISABLE_THROTTLING, TRUE);
    }
    if (FAILED(result)) {
        SetFailure(
            errorMessage,
            nativeError,
            L"配置 Media Foundation 写入器失败：" + HResultText(result),
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
            L"创建 MP4 文件失败：" + HResultText(result),
            result);
        impl_->ShutdownUnlocked();
        return false;
    }

    impl_->selectedBitrate = config.averageBitrate != 0
                                 ? config.averageBitrate
                                 : RecommendBitrate(
                                       config.width,
                                       config.height,
                                       config.framesPerSecond);

    ComPtr<IMFMediaType> outputType;
    result = ::MFCreateMediaType(&outputType);
    if (SUCCEEDED(result)) {
        result = outputType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    }
    if (SUCCEEDED(result)) {
        result = outputType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
    }
    if (SUCCEEDED(result)) {
        result = outputType->SetUINT32(MF_MT_AVG_BITRATE, impl_->selectedBitrate);
    }
    if (SUCCEEDED(result)) {
        result = outputType->SetUINT32(
            MF_MT_INTERLACE_MODE,
            MFVideoInterlace_Progressive);
    }
    if (SUCCEEDED(result)) {
        result = outputType->SetUINT32(
            MF_MT_MPEG2_PROFILE,
            eAVEncH264VProfile_High);
    }
    if (SUCCEEDED(result)) {
        result = ::MFSetAttributeSize(
            outputType.Get(),
            MF_MT_FRAME_SIZE,
            config.width,
            config.height);
    }
    if (SUCCEEDED(result)) {
        result = ::MFSetAttributeRatio(
            outputType.Get(),
            MF_MT_FRAME_RATE,
            static_cast<UINT32>(config.framesPerSecond),
            1);
    }
    if (SUCCEEDED(result)) {
        result = ::MFSetAttributeRatio(outputType.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
    }
    if (SUCCEEDED(result)) {
        result = impl_->writer->AddStream(outputType.Get(), &impl_->streamIndex);
    }
    if (FAILED(result) && outputType) {
        // 少数旧编码器不接受显式 High Profile，让系统重新协商一次。
        outputType->DeleteItem(MF_MT_MPEG2_PROFILE);
        result = impl_->writer->AddStream(outputType.Get(), &impl_->streamIndex);
    }
    if (FAILED(result)) {
        SetFailure(
            errorMessage,
            nativeError,
            L"创建 H.264 输出流失败：" + HResultText(result),
            result);
        impl_->ShutdownUnlocked();
        return false;
    }

    ComPtr<IMFMediaType> inputType;
    const std::uint32_t packedStride = config.width * 4;
    const std::uint64_t frameBytes64 =
        static_cast<std::uint64_t>(packedStride) * config.height;
    if (frameBytes64 > std::numeric_limits<DWORD>::max()) {
        SetFailure(errorMessage, nativeError, L"录制帧尺寸过大。", E_INVALIDARG);
        impl_->ShutdownUnlocked();
        return false;
    }

    result = ::MFCreateMediaType(&inputType);
    if (SUCCEEDED(result)) {
        result = inputType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    }
    if (SUCCEEDED(result)) {
        result = inputType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_ARGB32);
    }
    if (SUCCEEDED(result)) {
        result = inputType->SetUINT32(
            MF_MT_INTERLACE_MODE,
            MFVideoInterlace_Progressive);
    }
    if (SUCCEEDED(result)) {
        result = inputType->SetUINT32(MF_MT_FIXED_SIZE_SAMPLES, TRUE);
    }
    if (SUCCEEDED(result)) {
        result = inputType->SetUINT32(
            MF_MT_SAMPLE_SIZE,
            static_cast<UINT32>(frameBytes64));
    }
    if (SUCCEEDED(result)) {
        result = inputType->SetUINT32(MF_MT_DEFAULT_STRIDE, packedStride);
    }
    if (SUCCEEDED(result)) {
        result = ::MFSetAttributeSize(
            inputType.Get(),
            MF_MT_FRAME_SIZE,
            config.width,
            config.height);
    }
    if (SUCCEEDED(result)) {
        result = ::MFSetAttributeRatio(
            inputType.Get(),
            MF_MT_FRAME_RATE,
            static_cast<UINT32>(config.framesPerSecond),
            1);
    }
    if (SUCCEEDED(result)) {
        result = ::MFSetAttributeRatio(inputType.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
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
            L"配置 BGRA 到 H.264 的颜色转换失败：" + HResultText(result),
            result);
        impl_->ShutdownUnlocked();
        return false;
    }

    const HRESULT codecServiceResult = impl_->writer->GetServiceForStream(
        impl_->streamIndex,
        GUID_NULL,
        IID_PPV_ARGS(&impl_->codec));
    if (SUCCEEDED(codecServiceResult)) {
        if (!ConfigureCodec(
                impl_->codec.Get(),
                config,
                impl_->selectedBitrate,
                errorMessage,
                nativeError)) {
            impl_->ShutdownUnlocked();
            return false;
        }
        impl_->forcedKeyframeRequestsEnabled = true;
    } else {
        const std::wstring diagnostic =
            L"[SuperRecording.Mp4Writer] codec-setting service=unavailable result=" +
            HResultText(codecServiceResult) + L"\n";
        ::OutputDebugStringW(diagnostic.c_str());
    }

    result = impl_->writer->BeginWriting();
    if (FAILED(result)) {
        SetFailure(
            errorMessage,
            nativeError,
            L"启动 H.264 编码失败：" + HResultText(result),
            result);
        impl_->ShutdownUnlocked();
        return false;
    }

    impl_->config = config;
    impl_->config.averageBitrate = impl_->selectedBitrate;
    impl_->lastTimestamp = -1;
    impl_->nextForcedKeyframeTimestamp = -1;
    impl_->successfulForcedKeyframeRequests = 0;
    impl_->forcedKeyframeAcceptanceTraced = false;
    impl_->open = true;
    return true;
}

bool Mp4Writer::WriteBgraFrame(
    const std::span<const std::uint8_t> bgra,
    const std::uint32_t sourceStride,
    const std::int64_t timestamp100Nanoseconds,
    const std::int64_t duration100Nanoseconds,
    std::wstring& errorMessage,
    long& nativeError) noexcept {
    std::scoped_lock lock(impl_->mutex);
    errorMessage.clear();
    nativeError = 0;

    if (!impl_->open || !impl_->writer) {
        SetFailure(errorMessage, nativeError, L"MP4 写入器尚未打开。", MF_E_INVALIDREQUEST);
        return false;
    }
    const std::uint32_t packedStride = impl_->config.width * 4;
    if (sourceStride < packedStride || duration100Nanoseconds <= 0 ||
        timestamp100Nanoseconds < 0 ||
        (impl_->lastTimestamp >= 0 && timestamp100Nanoseconds <= impl_->lastTimestamp) ||
        duration100Nanoseconds >
            std::numeric_limits<std::int64_t>::max() - timestamp100Nanoseconds) {
        SetFailure(errorMessage, nativeError, L"视频帧参数或时间戳无效。", E_INVALIDARG);
        return false;
    }

    const std::uint64_t minimumSourceBytes =
        static_cast<std::uint64_t>(sourceStride) * (impl_->config.height - 1) +
        packedStride;
    const std::uint64_t destinationBytes64 =
        static_cast<std::uint64_t>(packedStride) * impl_->config.height;
    if (bgra.size() < minimumSourceBytes ||
        destinationBytes64 > std::numeric_limits<DWORD>::max()) {
        SetFailure(errorMessage, nativeError, L"BGRA 帧缓冲区长度不足。", E_INVALIDARG);
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
            L"分配编码帧缓冲区失败：" + HResultText(result),
            result);
        return false;
    }

    BYTE* destinationBytes = nullptr;
    DWORD maximumLength = 0;
    result = mediaBuffer->Lock(&destinationBytes, &maximumLength, nullptr);
    if (FAILED(result)) {
        SetFailure(
            errorMessage,
            nativeError,
            L"锁定编码帧缓冲区失败：" + HResultText(result),
            result);
        return false;
    }

    struct BufferUnlock final {
        IMFMediaBuffer* buffer{};
        void Unlock() noexcept {
            if (buffer != nullptr) {
                buffer->Unlock();
                buffer = nullptr;
            }
        }
        ~BufferUnlock() {
            Unlock();
        }
    } bufferUnlock{mediaBuffer.Get()};

    if (maximumLength < destinationBytes64) {
        SetFailure(errorMessage, nativeError, L"编码帧缓冲区容量不足。", E_UNEXPECTED);
        return false;
    }
    for (std::uint32_t row = 0; row < impl_->config.height; ++row) {
        std::memcpy(
            destinationBytes + static_cast<std::size_t>(row) * packedStride,
            bgra.data() + static_cast<std::size_t>(row) * sourceStride,
            packedStride);
    }
    result = mediaBuffer->SetCurrentLength(static_cast<DWORD>(destinationBytes64));
    if (FAILED(result)) {
        SetFailure(
            errorMessage,
            nativeError,
            L"设置编码帧长度失败：" + HResultText(result),
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
        if (impl_->nextForcedKeyframeTimestamp < 0) {
            impl_->ScheduleNextForcedKeyframeUnlocked(timestamp100Nanoseconds);
        } else if (
            impl_->forcedKeyframeRequestsEnabled &&
            timestamp100Nanoseconds >= impl_->nextForcedKeyframeTimestamp) {
            impl_->RequestForcedKeyframeUnlocked(timestamp100Nanoseconds);
            impl_->ScheduleNextForcedKeyframeUnlocked(timestamp100Nanoseconds);
        }
    }
    if (SUCCEEDED(result)) {
        result = impl_->writer->WriteSample(impl_->streamIndex, sample.Get());
    }
    if (FAILED(result)) {
        SetFailure(
            errorMessage,
            nativeError,
            L"写入 H.264 视频帧失败：" + HResultText(result),
            result);
        return false;
    }

    impl_->lastTimestamp = timestamp100Nanoseconds;
    return true;
}

bool Mp4Writer::Finalize(std::wstring& errorMessage, long& nativeError) noexcept {
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
            L"完成 MP4 文件封装失败：" + HResultText(result),
            result);
        return false;
    }
    return true;
}

bool Mp4Writer::IsOpen() const noexcept {
    std::scoped_lock lock(impl_->mutex);
    return impl_->open;
}

std::uint32_t Mp4Writer::AverageBitrate() const noexcept {
    std::scoped_lock lock(impl_->mutex);
    return impl_->selectedBitrate;
}

}  // namespace qrec::media
