#pragma once

#include "media/Mp4BoundaryEncoderPool.h"
#include "media/Mp4BoundaryTrimmer.h"

#include <mfidl.h>
#include <mfreadwrite.h>
#include <wrl/client.h>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <stop_token>
#include <string>

namespace qrec::detail {

using Microsoft::WRL::ComPtr;

struct BoundaryStepResult final {
    Mp4BoundaryTrimOutcome outcome{Mp4BoundaryTrimOutcome::Succeeded};
    HRESULT nativeError{S_OK};
    std::wstring errorMessage;

    [[nodiscard]] bool Succeeded() const noexcept {
        return outcome == Mp4BoundaryTrimOutcome::Succeeded;
    }
};

struct BoundarySourcePlan final {
    ComPtr<IMFMediaType> nativeType;
    LONGLONG requestedStart{};
    LONGLONG requestedEnd{};
    LONGLONG visibleStart{};
    LONGLONG spliceTime{};
    LONGLONG nominalFrameDuration{};
    UINT32 width{};
    UINT32 height{};
    UINT32 frameRateNumerator{};
    UINT32 frameRateDenominator{};
    std::uint32_t averageBitrate{};
    std::uint64_t encodedFrames{};
    int framesPerSecond{};
    bool encodeBoundary{};
};

struct BoundaryEncodeResult final {
    std::filesystem::path actualPath;
    Mp4BoundaryEncoderKey encoderKey;
    std::uint64_t encoderGeneration{};
    bool usedPrewarmedEncoder{};
    std::chrono::milliseconds encoderPrepareWait{};
    std::chrono::milliseconds encoderOpen{};
    std::uint64_t encodedFrames{};
    LONGLONG duration{};
};

struct BoundaryRemuxResult final {
    std::uint64_t passthroughSamples{};
};

[[nodiscard]] BoundaryStepResult MakeBoundaryStep(
    Mp4BoundaryTrimOutcome outcome,
    HRESULT nativeError,
    std::wstring errorMessage = {});

[[nodiscard]] BoundaryStepResult OpenNativeH264Source(
    const std::filesystem::path& path,
    ComPtr<IMFSourceReader>* reader,
    ComPtr<IMFMediaType>* nativeType);

[[nodiscard]] HRESULT SeekBoundaryReader(
    IMFSourceReader* reader,
    LONGLONG position) noexcept;

[[nodiscard]] BoundaryStepResult ValidateNoBFrameOrder(
    IMFSample* sample,
    LONGLONG presentationTime,
    LONGLONG* previousPresentationTime,
    bool removeDecodeTimestamp);

[[nodiscard]] bool IsCleanPoint(IMFSample* sample) noexcept;

[[nodiscard]] LONGLONG BoundaryFrameDuration(
    IMFMediaType* mediaType) noexcept;

[[nodiscard]] BoundaryStepResult ProbeBoundaryEncoderKey(
    const std::filesystem::path& sourcePath,
    Mp4BoundaryEncoderKey* output);

[[nodiscard]] BoundaryStepResult AnalyzeBoundarySource(
    const std::filesystem::path& sourcePath,
    LONGLONG requestedStart,
    LONGLONG requestedEnd,
    std::stop_token stopToken,
    BoundarySourcePlan* output);

[[nodiscard]] BoundaryStepResult EncodeBoundarySegment(
    const std::filesystem::path& sourcePath,
    const std::filesystem::path& temporaryPath,
    const BoundarySourcePlan& plan,
    std::stop_token stopToken,
    BoundaryEncodeResult* output);

[[nodiscard]] BoundaryStepResult ValidateBoundaryCompatibility(
    const std::filesystem::path& temporaryPath,
    const BoundarySourcePlan& plan);

[[nodiscard]] BoundaryStepResult CompareBoundaryNativeTypes(
    IMFMediaType* sourceType,
    IMFMediaType* boundaryType);

[[nodiscard]] BoundaryStepResult RemuxBoundarySegments(
    const std::filesystem::path& sourcePath,
    const std::filesystem::path& temporaryPath,
    const std::filesystem::path& destinationPath,
    const BoundarySourcePlan& plan,
    std::stop_token stopToken,
    BoundaryRemuxResult* output);

}  // namespace qrec::detail
