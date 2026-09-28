#include "AnnotationTestSupport.h"
#include "media/Mp4Writer.h"
#include "media/AacAudioWriter.h"
#include "media/MediaExporter.h"
#include "media/InstantArtifactDelivery.h"
#include "media/Mp4BoundaryEncoderPool.h"
#include "media/AnnotatedMp4.h"
#include "editor/PreparedExportArtifact.h"
#include <iostream>
#include <cmath>
#include <chrono>
#include <format>
#include <thread>

namespace annotation_tests {
namespace {
using namespace qrec;
using namespace qrec::annotations;
using Clock=std::chrono::steady_clock;
constexpr unsigned kWidth=1280,kHeight=720;
constexpr int kFps=60,kSeconds=6;
bool MakeSource(const std::filesystem::path& path,unsigned width=kWidth,unsigned height=kHeight,int fps=kFps,int seconds=kSeconds) {
    media::Mp4Writer writer; media::Mp4WriterConfig config{path,width,height,fps,0,true,500};
    config.averageBitrate=media::Mp4Writer::RecommendBitrate(width,height,fps);
    std::wstring error; long nativeError=0;
    if (!writer.Open(config,error,nativeError)) { std::wcerr<<error<<L'\n'; return false; }
    std::vector<std::uint8_t> pixels(width*height*4);
    for (int frame=0;frame<fps*seconds;++frame) {
        for (unsigned y=0;y<height;++y) for (unsigned x=0;x<width;++x) {
            const auto offset=(static_cast<std::size_t>(y)*width+x)*4;
            pixels[offset]=static_cast<std::uint8_t>(35+x/16);
            pixels[offset+1]=static_cast<std::uint8_t>(28+y/12);
            pixels[offset+2]=static_cast<std::uint8_t>(25+(frame/10)%20);
            pixels[offset+3]=255;
        }
        const auto begin=static_cast<std::int64_t>(frame)*10'000'000/fps;
        const auto end=static_cast<std::int64_t>(frame+1)*10'000'000/fps;
        if (!writer.WriteBgraFrame(pixels,width*4,begin,end-begin,error,nativeError)) return false;
    }
    return writer.Finalize(error,nativeError);
}
bool MakeAudio(const std::filesystem::path& path) {
    media::AacAudioWriter writer; media::AacAudioWriterConfig config{}; config.outputPath=path;
    std::wstring error; long nativeError=0;
    if (!writer.Open(config,error,nativeError)) return false;
    std::vector<float> samples(960*2);
    for (int packet=0;packet<kSeconds*50;++packet) {
        for (int frame=0;frame<960;++frame) {
            const float value=0.10F*std::sin(static_cast<float>(2*3.141592653589793*440*(packet*960+frame)/48000.0));
            samples[static_cast<std::size_t>(frame)*2]=value;
            samples[static_cast<std::size_t>(frame)*2+1]=value;
        }
        if (!writer.WriteInterleavedFrames(std::as_bytes(std::span(samples)),960,static_cast<std::int64_t>(packet)*200'000,200'000,error,nativeError)) return false;
    }
    return writer.Finalize(error,nativeError);
}
MediaExportResult Prepare(const ExportRequest& request,const std::filesystem::path& root,const wchar_t* name) {
    const auto begin=Clock::now();
    auto result=MediaExporter::WarmCache(request,{},{});
    const auto elapsed=std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now()-begin).count();
    std::wcout<<name<<L" elapsedMs="<<elapsed<<L" success="<<result.success<<L" cache="<<result.cacheHit
        <<L" diagnostic="<<result.diagnosticSummary<<L" error="<<result.errorMessage<<L'\n';
    Expect(result.success,name);
    if (result.success) {
        std::error_code error;
        std::filesystem::copy_file(result.outputPath,root/name,std::filesystem::copy_options::overwrite_existing,error);
        Expect(!error,L"Copy test artifact");
    }
    return result;
}
}

qrec::ExportRequest MediaTests(const std::filesystem::path& root) {
    using namespace qrec;
    ExportRequest request;
    request.recording.sourcePath=root/L"source.mp4";
    request.recording.width=kWidth; request.recording.height=kHeight;
    request.recording.framesPerSecond=kFps; request.recording.duration=Time{kSeconds*1000};
    request.trimEnd=request.recording.duration;
    request.destinationPath=root/L"destination.mp4";
    Expect(MakeSource(request.recording.sourcePath),L"Synthetic recording created with production writer");
    request.recording.systemAudio.sourcePath=root/L"source-audio.m4a";
    request.recording.systemAudio.available=MakeAudio(request.recording.systemAudio.sourcePath);
    request.recording.systemAudio.duration=request.recording.duration;
    Expect(request.recording.systemAudio.available,L"Synthetic tone encoded without desktop capture");
    Expect(MediaExporter::CanUsePassthrough(request),L"Unannotated MP4 retains instant passthrough");
    Document document; document.Reset({kWidth,kHeight},request.recording.duration);
    auto circle=document.NewMark(Tool::Circle,Time{1000}); circle.points={{400,300},{510,300}};
    Expect(document.Put(circle),L"Media circle added");
    request.annotations=document.Current();
    const auto encoderGeneration=detail::Mp4BoundaryEncoderPool::Shared().Prepare(request.recording.sourcePath);
    // Models the editor's existing asynchronous prewarm while the user selects a tool.
    std::this_thread::sleep_for(std::chrono::milliseconds(1100));
    static_cast<void>(Prepare(request,root,L"single-prewarmed.mp4"));
    auto pen=document.NewMark(Tool::Pen,Time{2000}); pen.points={{700,200},{750,260},{710,320},{810,370}};
    Expect(document.Put(pen),L"Media pen added");
    auto arrow=document.NewMark(Tool::Arrow,Time{3000}); arrow.points={{220,450},{500,400}};
    Expect(document.Put(arrow),L"Media arrow added");
    auto text=document.NewMark(Tool::Text,Time{4000}); text.points={{550,500}}; text.text=L"孙道长 · 标注测试";
    Expect(document.Put(text),L"Media Chinese text added");
    request.annotations=document.Current();
    Expect(!MediaExporter::CanUsePassthrough(request),L"Annotated recording cannot incorrectly reuse unmarked source");
    auto base=Prepare(request,root,L"marked.mp4");
    Expect(base.diagnosticSummary.find(L"generator=AnnotationHybrid")!=std::wstring::npos &&
        base.diagnosticSummary.find(L"fallback=")==std::wstring::npos,L"Affected GOP path works without fallback");
    PreparedExportArtifact ready;
    Expect(ready.Store(request,base),L"Ready export is accepted by editor artifact cache");
    const auto start=Clock::now();
    const auto resolved=ready.Resolve(request);
    Expect(resolved.has_value(),L"Clipboard can resolve prepared file without encode");
    std::wcout<<L"clipboardPrepareUs="<<std::chrono::duration_cast<std::chrono::microseconds>(Clock::now()-start).count()<<L'\n';
    if (resolved) {
        // A previous test run must not turn this fresh-file delivery check into
        // an overwrite request: production correctly refuses an existing path.
        const auto destination=root/(L"instant-save-"+std::to_wstring(GetCurrentProcessId())+L"-"+std::to_wstring(GetTickCount64())+L".mp4");
        const auto delivery=media::InstantArtifactDelivery::TryHardLink(*resolved,destination);
        Expect(delivery.outcome==media::InstantDeliveryOutcome::Delivered,L"Prepared save uses hardlink");
        std::wcout<<L"saveDeliveryUs="<<delivery.elapsed.count()<<L'\n';
    }
    const auto repeat=Prepare(request,root,L"cached.mp4"); Expect(repeat.cacheHit,L"Second export is a cache hit");
    auto edit=*document.Find(1); edit.argb=0xFF80ED99;
    Expect(document.Put(edit),L"Change only first circle color"); request.annotations=document.Current();
    Expect(!ready.Resolve(request).has_value(),L"Edited mark invalidates ready artifact");
    const auto edited=Prepare(request,root,L"edited.mp4");
    const auto cachedIndex=edited.diagnosticSummary.find(L"cachedSegments=");
    Expect(cachedIndex!=std::wstring::npos && std::stoull(edited.diagnosticSummary.substr(cachedIndex+15))>=3,L"Other marked GOPs are reused");
    auto trimmed=request; trimmed.trimStart=Time{233}; trimmed.trimEnd=Time{4791};
    const auto trimmedResult=Prepare(trimmed,root,L"trimmed.mp4");
    Expect(trimmedResult.diagnosticSummary.find(L"fallback=")==std::wstring::npos,L"Fractional frame endpoint retains incremental path");
    trimmed.includeSystemAudio=true;
    static_cast<void>(Prepare(trimmed,root,L"trimmed-audio.mp4"));
    auto scaled=request; scaled.qualityPercent=50;
    static_cast<void>(Prepare(scaled,root,L"quality50.mp4"));
    scaled.playbackSpeedTenths=20;
    static_cast<void>(Prepare(scaled,root,L"quality50-speed2.mp4"));
    auto slow=trimmed; slow.playbackSpeedTenths=1;
    static_cast<void>(Prepare(slow,root,L"slow-audio.mp4"));
    auto gif=request; gif.format=OutputFormat::Gif; gif.qualityPercent=50;
    static_cast<void>(Prepare(gif,root,L"marked.gif"));
    auto excluded=request; excluded.trimStart=Time{5000};
    const auto empty=Prepare(excluded,root,L"outside-marks.mp4");
    Expect(empty.diagnosticSummary.find(L"generator=Annotation")==std::wstring::npos,L"Annotations outside trim do not trigger annotation encoding");
    auto portrait=request;
    portrait.recording.sourcePath=root/L"portrait-source.mp4";
    portrait.recording.width=686; portrait.recording.height=1234; portrait.recording.framesPerSecond=30;
    portrait.recording.duration=Time{2000}; portrait.trimStart=Time{133}; portrait.trimEnd=Time{1871};
    Expect(MakeSource(portrait.recording.sourcePath,686,1234,30,2),L"Portrait 30 FPS source with macroblock padding");
    Document portraitMarks; portraitMarks.Reset({686,1234},Time{2000});
    auto verticalCircle=portraitMarks.NewMark(Tool::Circle,Time{1000}); verticalCircle.points={{330,600},{480,600}};
    Expect(portraitMarks.Put(verticalCircle),L"Portrait circle"); portrait.annotations=portraitMarks.Current();
    static_cast<void>(Prepare(portrait,root,L"portrait30.mp4"));
    std::stop_source cancel; cancel.request_stop();
    const auto cancelled=MediaExporter::WarmCache(request,{},cancel.get_token());
    Expect(cancelled.cancelled && !cancelled.success,L"Cancelled export never reports a ready artifact");
    detail::Mp4BoundaryEncoderPool::Shared().Discard(request.recording.sourcePath,encoderGeneration);
    return request;
}
void HybridProbe(const std::filesystem::path& root) {
    using namespace qrec;
    ExportRequest request;
    request.recording.sourcePath=root/L"source.mp4";
    request.recording.width=kWidth; request.recording.height=kHeight; request.recording.framesPerSecond=kFps;
    request.recording.duration=Time{6000}; request.trimStart=Time{233}; request.trimEnd=Time{4791};
    Document document; document.Reset({kWidth,kHeight},Time{6000});
    auto circle=document.NewMark(Tool::Circle,Time{1000}); circle.points={{400,300},{510,300}}; circle.argb=0xFF80ED99;
    static_cast<void>(document.Put(circle));
    auto pen=document.NewMark(Tool::Pen,Time{2000}); pen.points={{700,200},{750,260},{710,320},{810,370}};
    static_cast<void>(document.Put(pen));
    auto arrow=document.NewMark(Tool::Arrow,Time{3000}); arrow.points={{220,450},{500,400}};
    static_cast<void>(document.Put(arrow));
    auto text=document.NewMark(Tool::Text,Time{4000}); text.points={{550,500}}; text.text=L"孙道长 · 标注测试";
    static_cast<void>(document.Put(text)); request.annotations=document.Current();
    const auto result=ExportAnnotatedMp4(request,root/L"hybrid-probe.mp4",{},{});
    std::wcout<<L"hybridOutcome="<<static_cast<int>(result.outcome)<<L" error="<<result.error
        <<L" encoded="<<result.encodedFrames<<L" cached="<<result.cachedSegments<<L" copied="<<result.copiedSamples<<std::endl;
    Expect(result.outcome==Mp4BoundaryTrimOutcome::Succeeded,L"Arbitrary trim must retain incremental path");
}
}
