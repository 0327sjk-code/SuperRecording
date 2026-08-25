#include "media/Mp4BoundaryTrimmer.h"

#include "common/Win32Helpers.h"
#include "media/Mp4BoundaryInternal.h"

#include <mfapi.h>

#include <chrono>
#include <exception>
#include <limits>
#include <memory>
#include <system_error>
#include <utility>

namespace qrec {
namespace {

constexpr std::int64_t kTicksPerMillisecond = 10'000;
constexpr std::int64_t kNanosecondsPerMfTick = 100;

class PathCleanup final {
public:
    explicit PathCleanup(std::filesystem::path path)
        : path_(std::move(path)) {}

    ~PathCleanup() noexcept {
        if (!active_) {
            return;
        }
        std::error_code ignored;
        std::filesystem::remove(path_, ignored);
    }

    PathCleanup(const PathCleanup&) = delete;
    PathCleanup& operator=(const PathCleanup&) = delete;

    void Activate() noexcept { active_ = true; }
    void Release() noexcept { active_ = false; }

private:
    std::filesystem::path path_;
    bool active_{true};
};

class MediaFoundationSession final {
public:
    MediaFoundationSession() noexcept
        : result_(::MFStartup(MF_VERSION, MFSTARTUP_FULL)) {}

    ~MediaFoundationSession() {
        if (SUCCEEDED(result_)) {
            ::MFShutdown();
        }
    }

    [[nodiscard]] HRESULT Result() const noexcept { return result_; }

private:
    HRESULT result_{};
};

[[nodiscard]] bool ToTicks(
    const std::chrono::milliseconds value,
    LONGLONG* output) noexcept {
    if (output == nullptr || value.count() < 0 ||
        value.count() >
            std::numeric_limits<LONGLONG>::max() / kTicksPerMillisecond) {
        return false;
    }
    *output = value.count() * kTicksPerMillisecond;
    return true;
}

[[nodiscard]] std::filesystem::path MakeTemporaryPath(
    const std::filesystem::path& destinationPath,
    HRESULT* nativeError) {
    if (nativeError == nullptr) {
        return {};
    }
    GUID identifier{};
    *nativeError = ::CoCreateGuid(&identifier);
    if (FAILED(*nativeError)) {
        return {};
    }
    wchar_t identifierText[40]{};
    if (::StringFromGUID2(
            identifier,
            identifierText,
            static_cast<int>(std::size(identifierText))) == 0) {
        *nativeError = E_FAIL;
        return {};
    }
    std::filesystem::path temporary = destinationPath;
    temporary += L".boundary-";
    temporary += identifierText;
    temporary += L".mp4";
    *nativeError = S_OK;
    return temporary;
}

[[nodiscard]] Mp4BoundaryTrimResult FromStep(
    detail::BoundaryStepResult step,
    Mp4BoundaryTrimResult current) {
    current.outcome = step.outcome;
    current.nativeError = step.nativeError;
    current.errorMessage = std::move(step.errorMessage);
    return current;
}

[[nodiscard]] std::chrono::milliseconds ElapsedMilliseconds(
    const std::chrono::steady_clock::time_point started) noexcept {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - started);
}

}  // namespace

Mp4BoundaryTrimResult Mp4BoundaryTrimmer::Trim(
    const std::filesystem::path& sourcePath,
    const std::filesystem::path& destinationPath,
    const std::chrono::milliseconds trimStart,
    const std::chrono::milliseconds trimEnd,
    const std::stop_token stopToken) noexcept {
    Mp4BoundaryTrimResult result{};
    try {
        LONGLONG requestedStart = 0;
        LONGLONG requestedEnd = 0;
        if (sourcePath.empty() || destinationPath.empty() ||
            !ToTicks(trimStart, &requestedStart) ||
            !ToTicks(trimEnd, &requestedEnd) ||
            requestedEnd <= requestedStart) {
            result.outcome = Mp4BoundaryTrimOutcome::Unsupported;
            result.nativeError = E_INVALIDARG;
            result.errorMessage = L"边界裁剪参数无效。";
            return result;
        }
        if (stopToken.stop_requested()) {
            result.outcome = Mp4BoundaryTrimOutcome::Cancelled;
            result.nativeError = HRESULT_FROM_WIN32(ERROR_CANCELLED);
            return result;
        }

        std::error_code pathError;
        const std::filesystem::path absoluteSource =
            std::filesystem::absolute(sourcePath, pathError).lexically_normal();
        if (pathError) {
            result.nativeError = HRESULT_FROM_WIN32(ERROR_INVALID_NAME);
            result.errorMessage = L"无法规范化边界裁剪源路径。";
            return result;
        }
        const std::filesystem::path absoluteDestination =
            std::filesystem::absolute(destinationPath, pathError)
                .lexically_normal();
        if (pathError ||
            _wcsicmp(
                absoluteSource.c_str(),
                absoluteDestination.c_str()) == 0) {
            result.outcome = Mp4BoundaryTrimOutcome::Unsupported;
            result.nativeError = E_INVALIDARG;
            result.errorMessage = L"边界裁剪源路径与目标路径必须不同。";
            return result;
        }
        const bool destinationExists =
            std::filesystem::exists(absoluteDestination, pathError);
        if (pathError) {
            result.nativeError = HRESULT_FROM_WIN32(
                static_cast<DWORD>(pathError.value()));
            result.errorMessage = L"检查边界裁剪目标文件失败。";
            return result;
        }
        if (destinationExists) {
            result.outcome = Mp4BoundaryTrimOutcome::Failed;
            result.nativeError = HRESULT_FROM_WIN32(ERROR_FILE_EXISTS);
            result.errorMessage = L"边界裁剪目标文件已经存在。";
            return result;
        }
        const std::filesystem::path destinationParent =
            absoluteDestination.parent_path();
        if (!destinationParent.empty()) {
            std::filesystem::create_directories(destinationParent, pathError);
            if (pathError) {
                result.nativeError = HRESULT_FROM_WIN32(
                    static_cast<DWORD>(pathError.value()));
                result.errorMessage = L"创建边界裁剪目标目录失败。";
                return result;
            }
        }

        const win32::ScopedCoInitialize apartment(COINIT_MULTITHREADED);
        if (FAILED(apartment.Result()) &&
            apartment.Result() != RPC_E_CHANGED_MODE) {
            result.nativeError = apartment.Result();
            result.errorMessage = L"初始化边界裁剪 MTA 线程失败：" +
                win32::FormatError(apartment.Result());
            return result;
        }

        const MediaFoundationSession mediaFoundation;
        if (FAILED(mediaFoundation.Result())) {
            result.nativeError = mediaFoundation.Result();
            result.errorMessage = L"启动 Media Foundation 失败：" +
                win32::FormatError(mediaFoundation.Result());
            return result;
        }

        HRESULT temporaryError = S_OK;
        const std::filesystem::path temporaryPath = MakeTemporaryPath(
            absoluteDestination,
            &temporaryError);
        if (FAILED(temporaryError) || temporaryPath.empty()) {
            result.nativeError = FAILED(temporaryError)
                ? temporaryError
                : E_FAIL;
            result.errorMessage = L"创建边界临时文件名失败。";
            return result;
        }
        PathCleanup temporaryCleanup(temporaryPath);
        PathCleanup destinationCleanup(absoluteDestination);

        const auto scanStarted = std::chrono::steady_clock::now();
        detail::BoundarySourcePlan plan{};
        detail::BoundaryStepResult step = detail::AnalyzeBoundarySource(
            absoluteSource,
            requestedStart,
            requestedEnd,
            stopToken,
            &plan);
        result.scanDuration = ElapsedMilliseconds(scanStarted);
        if (!step.Succeeded()) {
            return FromStep(std::move(step), std::move(result));
        }
        result.boundaryDuration = std::chrono::nanoseconds(
            (plan.spliceTime - plan.visibleStart) *
            kNanosecondsPerMfTick);

        const auto encodeStarted = std::chrono::steady_clock::now();
        detail::BoundaryEncodeResult encoded{};
        std::filesystem::path encodedPath = temporaryPath;
        std::unique_ptr<PathCleanup> encodedPathCleanup;
        if (plan.encodeBoundary) {
            step = detail::EncodeBoundarySegment(
                absoluteSource,
                temporaryPath,
                plan,
                stopToken,
                &encoded);
            result.encodedFrames = encoded.encodedFrames;
            result.usedPrewarmedEncoder = encoded.usedPrewarmedEncoder;
            result.encoderPrepareWait = encoded.encoderPrepareWait;
            result.encoderOpen = encoded.encoderOpen;
            plan.encodedFrames = encoded.encodedFrames;
            if (step.Succeeded()) {
                if (encoded.actualPath.empty()) {
                    step = detail::MakeBoundaryStep(
                        Mp4BoundaryTrimOutcome::Failed,
                        E_UNEXPECTED,
                        L"边界编码器未返回临时成片路径。");
                } else {
                    encodedPath = encoded.actualPath;
                    if (_wcsicmp(
                            encodedPath.c_str(),
                            temporaryPath.c_str()) != 0) {
                        encodedPathCleanup =
                            std::make_unique<PathCleanup>(encodedPath);
                    }
                }
            }
            if (step.Succeeded()) {
                step = detail::ValidateBoundaryCompatibility(
                    encodedPath,
                    plan);
            }
        }
        result.decodeEncodeDuration = ElapsedMilliseconds(encodeStarted);
        if (!step.Succeeded()) {
            return FromStep(std::move(step), std::move(result));
        }

        const auto remuxStarted = std::chrono::steady_clock::now();
        detail::BoundaryRemuxResult remuxed{};
        step = detail::RemuxBoundarySegments(
            absoluteSource,
            encodedPath,
            absoluteDestination,
            plan,
            stopToken,
            &remuxed);
        result.remuxDuration = ElapsedMilliseconds(remuxStarted);
        result.passthroughSamples = remuxed.passthroughSamples;
        if (!step.Succeeded()) {
            return FromStep(std::move(step), std::move(result));
        }

        destinationCleanup.Release();
        if (encoded.encoderGeneration != 0) {
            detail::Mp4BoundaryEncoderPool::Shared().Replenish(
                encoded.encoderKey,
                encoded.encoderGeneration);
        }
        result.outcome = Mp4BoundaryTrimOutcome::Succeeded;
        result.nativeError = S_OK;
        result.errorMessage.clear();
        return result;
    } catch (const std::bad_alloc&) {
        result.outcome = Mp4BoundaryTrimOutcome::Failed;
        result.nativeError = E_OUTOFMEMORY;
        result.errorMessage = L"边界裁剪内存不足。";
        return result;
    } catch (const std::exception&) {
        result.outcome = Mp4BoundaryTrimOutcome::Failed;
        result.nativeError = E_FAIL;
        result.errorMessage = L"边界裁剪发生未预期异常。";
        return result;
    }
}

}  // namespace qrec
