#include "media/AnnotatedMp4.h"
#include "media/Mp4BoundaryInternal.h"
#include "media/ExportArtifactCache.h"
#include <mfapi.h>
#include <mferror.h>
#include <algorithm>
#include <format>

namespace qrec {
namespace {
using namespace detail;
constexpr LONGLONG kTicksPerMillisecond=10'000;
constexpr DWORD kVideo=static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM);
struct Window final {
    LONGLONG begin{},end{};
    bool encode{};
    std::vector<std::uint64_t> marks;
    LONGLONG encodedEnd{};
};

BoundaryStepResult PlanWindows(const ExportRequest& request,BoundarySourcePlan* geometry,
    std::vector<Window>* windows,std::stop_token stop) {
    const LONGLONG begin=request.trimStart.count()*kTicksPerMillisecond;
    const LONGLONG end=request.trimEnd.count()*kTicksPerMillisecond;
    auto step=AnalyzeBoundarySource(request.recording.sourcePath,begin,end,stop,geometry);
    if (!step.Succeeded()) return step;
    ComPtr<IMFSourceReader> reader; ComPtr<IMFMediaType> type;
    step=OpenNativeH264Source(request.recording.sourcePath,&reader,&type);
    if (!step.Succeeded()) return step;
    HRESULT result=SeekBoundaryReader(reader.Get(),begin);
    if (FAILED(result)) return MakeBoundaryStep(Mp4BoundaryTrimOutcome::Failed,result,L"无法扫描标注关键帧。");
    LONGLONG current=geometry->visibleStart,previous=-1,lastEnd=0;
    bool nonClean=geometry->encodeBoundary;
    const auto append=[&](LONGLONG stopTime) {
        if (stopTime<=current) return;
        std::vector<std::uint64_t> marks;
        for (const auto& mark : request.annotations->Marks())
            if (mark.start.count()*kTicksPerMillisecond<stopTime && mark.end.count()*kTicksPerMillisecond>current)
                marks.push_back(mark.id);
        const bool marked=!marks.empty();
        const bool encode=nonClean || marked;
        // Adjacent GOPs influenced by the same marks share one encoder session.
        // Different marks retain independent cache entries for incremental edits.
        if (!windows->empty() && windows->back().encode==encode && windows->back().end==current && windows->back().marks==marks)
            windows->back().end=windows->back().encodedEnd=stopTime;
        else windows->push_back({current,stopTime,encode,std::move(marks),stopTime});
        current=stopTime; nonClean=false;
    };
    for (;;) {
        if (stop.stop_requested()) return MakeBoundaryStep(Mp4BoundaryTrimOutcome::Cancelled,HRESULT_FROM_WIN32(ERROR_CANCELLED));
        DWORD stream=0,flags=0; LONGLONG time=0; ComPtr<IMFSample> sample;
        result=reader->ReadSample(kVideo,0,&stream,&flags,&time,&sample);
        if (FAILED(result) || (flags&MF_SOURCE_READERF_ERROR)) return MakeBoundaryStep(Mp4BoundaryTrimOutcome::Failed,FAILED(result)?result:E_FAIL,L"扫描标注片段失败。");
        if (flags&(MF_SOURCE_READERF_CURRENTMEDIATYPECHANGED|MF_SOURCE_READERF_NATIVEMEDIATYPECHANGED|MF_SOURCE_READERF_STREAMTICK))
            return MakeBoundaryStep(Mp4BoundaryTrimOutcome::Unsupported,MF_E_INVALIDMEDIATYPE,L"源视频不适合标注快速拼接。");
        if (flags&MF_SOURCE_READERF_ENDOFSTREAM) {
            append(std::min(end,lastEnd));
            if (!windows->empty() && windows->back().encode) windows->back().encodedEnd=lastEnd;
            break;
        }
        if (!sample) continue;
        step=ValidateNoBFrameOrder(sample.Get(),time,&previous,false);
        if (!step.Succeeded()) return step;
        LONGLONG duration=geometry->nominalFrameDuration;
        static_cast<void>(sample->GetSampleDuration(&duration));
        lastEnd=time+std::max<LONGLONG>(1,duration);
        if (time>=end) {
            append(end);
            // Encode complete source frames, then clip the last compressed sample
            // while muxing. A partial encoder frame changes MF's native frame-rate
            // metadata and would unnecessarily fail the strict splice gate.
            if (!windows->empty() && windows->back().encode) windows->back().encodedEnd=time;
            break;
        }
        if (time>current && IsCleanPoint(sample.Get())) append(time);
    }
    if (windows->empty()) return MakeBoundaryStep(Mp4BoundaryTrimOutcome::Unsupported,MF_E_END_OF_STREAM,L"没有可导出的标注片段。");
    return MakeBoundaryStep(Mp4BoundaryTrimOutcome::Succeeded,S_OK);
}

ExportArtifactCacheResult PreparePatch(const ExportRequest& request,const Window& window,
    BoundarySourcePlan geometry,std::stop_token stop,std::uint64_t* frames) {
    ExportRequest patch=request;
    patch.trimStart=annotations::Time{window.begin/kTicksPerMillisecond};
    patch.trimEnd=annotations::Time{(window.end+kTicksPerMillisecond-1)/kTicksPerMillisecond};
    patch.includeSystemAudio=false; patch.playbackSpeedTenths=10; patch.qualityPercent=100;
    std::vector<annotations::Mark> relevant;
    for (const auto& mark : request.annotations->Marks())
        if (mark.start.count()*kTicksPerMillisecond<window.end && mark.end.count()*kTicksPerMillisecond>window.begin)
            relevant.push_back(mark);
    patch.annotations=annotations::Scene::Create(request.annotations->Canvas(),std::move(relevant));
    std::wstring error;
    auto key=ExportArtifactCache::BuildKey(patch,&error);
    if (!key) { ExportArtifactCacheResult result; result.errorMessage=std::move(error); return result; }
    key->annotationIdentity+=std::format(L"|gop-patch-v1:{}:{}",window.begin,window.end);
    geometry.visibleStart=window.begin; geometry.spliceTime=window.end;
    geometry.requestedStart=window.begin; geometry.requestedEnd=window.end; geometry.encodeBoundary=true;
    return ExportArtifactCache::Shared().GetOrCreate(*key,L".mp4",stop,
        [&](const std::filesystem::path& staging,std::stop_token token,std::wstring* message) -> HRESULT {
            BoundaryEncodeResult encoded;
            auto step=EncodeBoundarySegment(request.recording.sourcePath,staging,geometry,token,&encoded,patch.annotations);
            if (step.Succeeded()) {
                geometry.encodedFrames=encoded.encodedFrames;
                step=ValidateBoundaryCompatibility(encoded.actualPath,geometry);
            }
            HRESULT result=step.nativeError;
            if (step.Succeeded()) {
                *frames+=encoded.encodedFrames;
                if (encoded.actualPath!=staging && !CopyFileW(encoded.actualPath.c_str(),staging.c_str(),FALSE))
                    result=HRESULT_FROM_WIN32(GetLastError());
            }
            if (!encoded.actualPath.empty() && encoded.actualPath!=staging) {
                std::error_code ignored; std::filesystem::remove(encoded.actualPath,ignored);
            }
            if (encoded.encoderGeneration!=0) Mp4BoundaryEncoderPool::Shared().Replenish(encoded.encoderKey,encoded.encoderGeneration);
            if (FAILED(result) && message) *message=step.errorMessage.empty() ? L"无法准备标注片段缓存。" : step.errorMessage;
            return result;
        });
}
}  // namespace

AnnotatedMp4Result ExportAnnotatedMp4(const ExportRequest& request,const std::filesystem::path& output,
    std::stop_token stop,const std::function<void(const ExportProgress&)>& progress) {
    AnnotatedMp4Result result;
    if (!request.annotations || request.qualityPercent!=100 || request.playbackSpeedTenths!=10) return result;
    BoundarySourcePlan geometry; std::vector<Window> windows;
    auto step=PlanWindows(request,&geometry,&windows,stop);
    if (!step.Succeeded()) { result.outcome=step.outcome; result.nativeError=step.nativeError; result.error=step.errorMessage; return result; }
    std::vector<CompressedVideoSegment> segments;
    const LONGLONG base=windows.front().begin;
    for (std::size_t i=0;i<windows.size();++i) {
        const Window& window=windows[i];
        if (progress) progress({0.05+0.85*static_cast<double>(i)/static_cast<double>(windows.size()),L"正在增量合成标注片段…"});
        if (!window.encode) {
            segments.push_back({request.recording.sourcePath,window.begin,window.end,window.begin-base,true});
            continue;
        }
        Window completeFrames=window;
        completeFrames.end=window.encodedEnd;
        auto patch=PreparePatch(request,completeFrames,geometry,stop,&result.encodedFrames);
        if (!patch.success) {
            result.outcome=patch.cancelled ? Mp4BoundaryTrimOutcome::Cancelled : Mp4BoundaryTrimOutcome::Unsupported;
            result.nativeError=patch.nativeError; result.error=patch.errorMessage; return result;
        }
        if (patch.cacheHit) ++result.cachedSegments; else ++result.encodedSegments;
        segments.push_back({patch.artifactPath,0,window.end-window.begin,window.begin-base,false});
    }
    BoundaryRemuxResult remuxed;
    step=RemuxVideoSegments(geometry.nativeType.Get(),segments,output,windows.back().end-base,stop,&remuxed);
    result.outcome=step.outcome; result.nativeError=step.nativeError; result.error=step.errorMessage;
    result.copiedSamples=remuxed.passthroughSamples;
    if (!step.Succeeded()) { std::error_code ignored; std::filesystem::remove(output,ignored); }
    return result;
}
}  // namespace qrec
