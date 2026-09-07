#include "capture/SystemAudioCapture.h"
#include "capture/SystemAudioTimelinePolicy.h"
#include "media/AudioVideoMuxer.h"
#include "media/CompressedTimelineAlignment.h"
#include "media/MediaExporter.h"

#include <windows.h>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <string_view>
#include <thread>

namespace {

using qrec::media::CompressedTimelineAlignment;
using namespace std::chrono_literals;

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

void TestPacketCadenceJitterDoesNotBecomeSilence() {
    constexpr std::uint64_t encodedFrames = 35'872;
    const qrec::capture::SystemAudioCatchUpPlan unguarded =
        qrec::capture::PlanSystemAudioSilenceCatchUp(
            1s,
            encodedFrames,
            48'000,
            250ms,
            0ns);
    Expect(
        unguarded.targetFrames == 36'000 &&
            unguarded.silenceFrames == 128,
        L"unguarded packet cadence should reproduce the 128-frame dropout");

    const qrec::capture::SystemAudioCatchUpPlan guarded =
        qrec::capture::PlanSystemAudioSilenceCatchUp(
            1s,
            encodedFrames,
            48'000,
            250ms,
            200ms);
    Expect(
        guarded.targetFrames == 26'400,
        L"guarded catch-up should retain a 200 ms late-packet window");
    Expect(
        guarded.silenceFrames == 0,
        L"normal packet cadence jitter must not synthesize silence");
}

void TestConfirmedAudioOutageStillFillsTimeline() {
    const qrec::capture::SystemAudioCatchUpPlan plan =
        qrec::capture::PlanSystemAudioSilenceCatchUp(
            1s,
            12'000,
            48'000,
            250ms,
            200ms);
    Expect(
        plan.targetFrames == 26'400,
        L"confirmed outage should advance through the protected boundary");
    Expect(
        plan.silenceFrames == 14'400,
        L"confirmed outage should synthesize only the old, unrecoverable gap");
}

void TestInvalidAudioCatchUpInputsAreSafe() {
    Expect(
        qrec::capture::PlanSystemAudioSilenceCatchUp(
            -1ns,
            0,
            48'000,
            250ms,
            200ms).silenceFrames == 0,
        L"negative active duration must not create silence");
    Expect(
        qrec::capture::PlanSystemAudioSilenceCatchUp(
            1s,
            0,
            0,
            250ms,
            200ms).silenceFrames == 0,
        L"zero sample rate must not create silence");
}

int RunUnitTests() {
    TestFailedProductionRangeNowAligns();
    TestMaterialTruncationStillFails();
    TestLongRetimedFramesUseMeasuredSampleDuration();
    TestThirtyFpsBoundaryQuantization();
    TestLongerVideoClampsToRequestedRange();
    TestInvalidRangesAreRejected();
    TestPacketCadenceJitterDoesNotBecomeSilence();
    TestConfirmedAudioOutageStillFillsTimeline();
    TestInvalidAudioCatchUpInputsAreSafe();
    if (gFailures == 0) {
        std::wcout <<
            L"PASS: media timeline and system-audio policy unit tests\n";
    }
    return gFailures == 0 ? 0 : 1;
}

int RunLoopbackCapture(const int argumentCount, wchar_t* arguments[]) {
    if (argumentCount != 4) {
        std::wcerr <<
            L"Usage: MediaTimelineTests --capture-loopback <output.m4a> "
            L"<duration-ms>\n";
        return 64;
    }

    const std::int64_t durationMilliseconds = _wtoi64(arguments[3]);
    if (durationMilliseconds < 500 || durationMilliseconds > 120'000) {
        std::wcerr << L"Capture duration must be between 500 and 120000 ms.\n";
        return 64;
    }

    qrec::capture::SystemAudioCapture capture;
    qrec::capture::SystemAudioCaptureError error;
    const qrec::capture::SystemAudioCaptureConfig config{
        std::filesystem::path(arguments[2]),
        48'000,
        2,
        0,
        qrec::capture::SystemAudioEndpointRole::Multimedia,
    };
    if (!capture.StartPrepared(config, {}, &error)) {
        std::wcerr << L"captureStartError=" << error.message << L'\n';
        return 4;
    }

    const std::optional<qrec::capture::SystemAudioQpcPosition> startQpc =
        qrec::capture::QuerySystemAudioQpcPosition100Nanoseconds();
    if (!capture.Resume(startQpc, &error)) {
        std::wcerr << L"captureResumeError=" << error.message << L'\n';
        return 5;
    }

    const std::chrono::milliseconds duration(durationMilliseconds);
    std::this_thread::sleep_for(duration);
    const std::optional<qrec::capture::SystemAudioQpcPosition> stopQpc =
        qrec::capture::QuerySystemAudioQpcPosition100Nanoseconds();
    const std::optional<qrec::capture::SystemAudioRecordingResult> result =
        capture.Stop(
            stopQpc,
            std::chrono::duration_cast<std::chrono::nanoseconds>(duration),
            &error);
    if (!result.has_value()) {
        std::wcerr << L"captureStopError=" << error.message << L'\n';
        return 6;
    }

    std::wcout
        << L"captureSuccess=true"
        << L" durationMs=" << result->duration.count()
        << L" bitrate=" << result->averageBitrate
        << L" encodedFrames=" << result->encodedFrames
        << L" silentFrames=" << result->silentFrames
        << L" syntheticSilentFrames=" << result->syntheticSilentFrames
        << L" catchUpSilentFrames=" << result->catchUpSilentFrames
        << L" catchUpEventCount=" << result->catchUpEventCount
        << L" discontinuityCount=" << result->discontinuityCount
        << L" output=" << result->outputPath.wstring() << L'\n';
    return 0;
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
    if (argumentCount >= 2 &&
        std::wstring_view(arguments[1]) == L"--capture-loopback") {
        return RunLoopbackCapture(argumentCount, arguments);
    }
    return RunUnitTests();
}
