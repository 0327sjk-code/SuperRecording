#include "media/AudioVideoMuxer.h"
#include "media/CompressedTimelineAlignment.h"
#include "media/MediaExporter.h"

#include <windows.h>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <string_view>

namespace {

using qrec::media::CompressedTimelineAlignment;

int gFailures = 0;

void Expect(const bool condition, const std::wstring_view message) {
    if (condition) {
        return;
    }
    ++gFailures;
    std::wcerr << L"FAIL: " << message << L'\n';
}

void TestFailedProductionRangeNowAligns() {
    CompressedTimelineAlignment alignment{};
    const bool aligned = qrec::media::TryAlignCompressedVideoToRange(
        6'730'000,
        14'880'000,
        8'046'667,
        &alignment);
    Expect(aligned, L"673-1488 ms range should align");
    Expect(
        alignment.requestedDurationTicks == 8'150'000,
        L"requested duration should remain 815 ms");
    Expect(
        alignment.outputDurationTicks == 8'046'667,
        L"measured video duration should be authoritative");
    Expect(
        alignment.videoStartAdjustmentTicks == 103'333,
        L"start should advance by 10.3333 ms");
    Expect(
        alignment.effectiveAudioTrimStartTicks == 6'833'333,
        L"audio should align to the effective video start");
    Expect(
        alignment.effectiveAudioTrimEndTicks == 14'880'000,
        L"requested end should be preserved");
    Expect(
        qrec::media::CoversFrameQuantizedRequestedSpan(
            alignment.requestedDurationTicks,
            alignment.outputDurationTicks,
            166'667,
            166'667,
            2),
        L"one-frame start quantization should be accepted");
}

void TestMaterialTruncationStillFails() {
    Expect(
        !qrec::media::CoversFrameQuantizedRequestedSpan(
            8'150'000,
            7'800'000,
            166'667,
            166'667,
            2),
        L"35 ms truncation must not be accepted as frame quantization");
}

void TestLongRetimedFramesUseMeasuredSampleDuration() {
    Expect(
        qrec::media::CoversFrameQuantizedRequestedSpan(
            8'150'000,
            7'100'000,
            166'667,
            1'666'667,
            2),
        L"slow-motion frame duration should define the quantization bound");
}

void TestThirtyFpsBoundaryQuantization() {
    Expect(
        qrec::media::CoversFrameQuantizedRequestedSpan(
            20'000'000,
            19'700'000,
            333'333,
            333'333,
            2),
        L"30 FPS start quantization below one frame should be accepted");
    Expect(
        !qrec::media::CoversFrameQuantizedRequestedSpan(
            20'000'000,
            19'300'000,
            333'333,
            333'333,
            2),
        L"30 FPS shortfall above one frame should be rejected");
}

void TestLongerVideoClampsToRequestedRange() {
    CompressedTimelineAlignment alignment{};
    const bool aligned = qrec::media::TryAlignCompressedVideoToRange(
        0,
        21'000'000,
        21'120'000,
        &alignment);
    Expect(aligned, L"whole-range video should align");
    Expect(
        alignment.outputDurationTicks == 21'000'000,
        L"video longer than the request should be clipped to the request");
    Expect(
        alignment.videoStartAdjustmentTicks == 0,
        L"longer video must not shift the audio start");
}

void TestInvalidRangesAreRejected() {
    CompressedTimelineAlignment alignment{};
    Expect(
        !qrec::media::TryAlignCompressedVideoToRange(
            100,
            100,
            100,
            &alignment),
        L"empty requested range must be rejected");
    Expect(
        !qrec::media::TryAlignCompressedVideoToRange(
            0,
            100,
            0,
            &alignment),
        L"empty measured video must be rejected");
}

int RunUnitTests() {
    TestFailedProductionRangeNowAligns();
    TestMaterialTruncationStillFails();
    TestLongRetimedFramesUseMeasuredSampleDuration();
    TestThirtyFpsBoundaryQuantization();
    TestLongerVideoClampsToRequestedRange();
    TestInvalidRangesAreRejected();
    if (gFailures == 0) {
        std::wcout << L"PASS: media timeline unit tests\n";
    }
    return gFailures == 0 ? 0 : 1;
}

int RunMuxIntegration(const int argumentCount, wchar_t* arguments[]) {
    if (argumentCount != 7) {
        std::wcerr <<
            L"Usage: MediaTimelineTests --mux <video> <audio> <output> "
            L"<trim-start-ms> <trim-end-ms>\n";
        return 64;
    }
    const std::int64_t trimStart = _wtoi64(arguments[5]);
    const std::int64_t trimEnd = _wtoi64(arguments[6]);
    const qrec::AudioVideoMuxResult result = qrec::AudioVideoMuxer::Mux(
        qrec::AudioVideoMuxRequest{
            std::filesystem::path(arguments[2]),
            std::filesystem::path(arguments[3]),
            std::filesystem::path(arguments[4]),
            std::chrono::milliseconds(trimStart),
            std::chrono::milliseconds(trimEnd),
        });
    std::wcout
        << L"outcome=" << static_cast<int>(result.outcome)
        << L" nativeError=0x" << std::hex
        << static_cast<unsigned long>(result.nativeError) << std::dec
        << L" videoSamples=" << result.videoSamples
        << L" audioSamples=" << result.audioSamples
        << L" requestedDurationNs=" << result.requestedDuration.count()
        << L" videoDurationNs=" << result.videoDuration.count()
        << L" videoStartAdjustmentNs="
        << result.videoStartAdjustment.count()
        << L" effectiveAudioTrimStartNs="
        << result.effectiveAudioTrimStart.count()
        << L" effectiveAudioTrimEndNs="
        << result.effectiveAudioTrimEnd.count()
        << L" audioLeadingGapNs=" << result.audioLeadingGap.count()
        << L" audioTrailingGapNs=" << result.audioTrailingGap.count()
        << L" error=" << result.errorMessage << L'\n';
    return result.outcome == qrec::AudioVideoMuxOutcome::Succeeded ? 0 : 2;
}

int RunWarmCacheIntegration(const int argumentCount, wchar_t* arguments[]) {
    if (argumentCount < 10 || argumentCount > 12) {
        std::wcerr <<
            L"Usage: MediaTimelineTests --warm-cache <video> <audio> "
            L"<trim-start-ms> <trim-end-ms> <width> <height> <fps> "
            L"<recording-duration-ms> [playback-speed-tenths] "
            L"[quality-percent]\n";
        return 64;
    }

    qrec::ExportRequest request{};
    request.recording.sourcePath = std::filesystem::path(arguments[2]);
    request.recording.systemAudio.sourcePath =
        std::filesystem::path(arguments[3]);
    request.recording.systemAudio.available = true;
    request.trimStart = std::chrono::milliseconds(_wtoi64(arguments[4]));
    request.trimEnd = std::chrono::milliseconds(_wtoi64(arguments[5]));
    request.recording.width = static_cast<std::uint32_t>(_wtoi(arguments[6]));
    request.recording.height = static_cast<std::uint32_t>(_wtoi(arguments[7]));
    request.recording.framesPerSecond = _wtoi(arguments[8]);
    request.recording.duration = std::chrono::milliseconds(
        _wtoi64(arguments[9]));
    request.recording.systemAudio.duration = request.recording.duration;
    request.format = qrec::OutputFormat::Mp4;
    request.includeSystemAudio = true;
    request.playbackSpeedTenths = argumentCount >= 11
        ? _wtoi(arguments[10])
        : 10;
    request.qualityPercent = argumentCount == 12
        ? _wtoi(arguments[11])
        : 100;

    const qrec::MediaExportResult first = qrec::MediaExporter::WarmCache(
        request,
        {},
        {});
    const qrec::MediaExportResult second = first.success
        ? qrec::MediaExporter::WarmCache(request, {}, {})
        : qrec::MediaExportResult{};
    std::wcout
        << L"firstSuccess=" << (first.success ? L"true" : L"false")
        << L" firstCacheHit=" << (first.cacheHit ? L"true" : L"false")
        << L" firstOutput=" << first.outputPath.wstring()
        << L" firstDiagnostic=" << first.diagnosticSummary
        << L" firstError=" << first.errorMessage
        << L" secondSuccess=" << (second.success ? L"true" : L"false")
        << L" secondCacheHit=" << (second.cacheHit ? L"true" : L"false")
        << L" secondOutput=" << second.outputPath.wstring()
        << L" secondError=" << second.errorMessage << L'\n';
    return first.success && second.success && second.cacheHit ? 0 : 3;
}

}  // namespace

int wmain(const int argumentCount, wchar_t* arguments[]) {
    if (argumentCount >= 2 && std::wstring_view(arguments[1]) == L"--mux") {
        return RunMuxIntegration(argumentCount, arguments);
    }
    if (argumentCount >= 2 &&
        std::wstring_view(arguments[1]) == L"--warm-cache") {
        return RunWarmCacheIntegration(argumentCount, arguments);
    }
    return RunUnitTests();
}
