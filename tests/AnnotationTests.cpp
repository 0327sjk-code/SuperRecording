#include "AnnotationTestSupport.h"
#include "annotations/AnnotationCompositor.h"
#include "annotations/AnnotationRenderer.h"
#include "annotations/ArrowGeometry.h"
#include "common/Win32Helpers.h"
#include <mfapi.h>
#include <atomic>
#include <iostream>
#include <algorithm>
#include <io.h>
#include <fcntl.h>

namespace annotation_tests {
namespace { std::atomic_int failures{}; }
void Expect(bool condition,std::wstring_view message) {
    if (!condition) { ++failures; std::wcerr<<L"FAIL: "<<message<<L'\n'; }
}
int Failures() { return failures.load(); }
void ModelTests() {
    using namespace qrec::annotations;
    Document document; document.Reset({640,360},Time{4000});
    auto circle=document.NewMark(Tool::Circle,Time{1000}); circle.points={{200,120},{250,120}};
    Expect(circle.end-circle.start==Time{500},L"Default duration is exactly 500 ms");
    Expect(document.Put(circle),L"Circle can commit");
    const auto first=document.Current();
    const auto id=document.LastId();
    const auto bounds=MeasureBounds(*document.Find(id));
    Expect(std::abs((bounds.left+bounds.right)/2-200)<0.01F,L"Mouse-down is the circle center");
    Expect(Visible(*document.Find(id),Time{1000}) && !Visible(*document.Find(id),Time{1500}),L"Half-open annotation interval");
    auto changed=*document.Find(id); changed.argb=0xFF80ED99;
    Expect(document.Put(changed),L"Style edits commit");
    Expect(document.Current()->Identity()!=first->Identity(),L"Style invalidates cache identity");
    Expect(document.Undo() && document.Current()->Identity()==first->Identity(),L"Undo restores immutable snapshot");
    Expect(document.Redo() && document.Find(id)->argb==changed.argb,L"Redo restores style");
    auto nearEnd=document.NewMark(Tool::Pen,Time{3999}); nearEnd.points={{1,1}};
    Expect(nearEnd.end==Time{4000} && nearEnd.start==Time{3999},L"Tail interval clamps safely");
    std::vector<std::uint8_t> pixels(640*360*4,30),original=pixels;
    FrameCompositor compositor(first);
    Expect(compositor.Apply(pixels,640,360,640*4,Time{999}) && pixels==original,L"Unmarked time stays byte-identical");
    Expect(compositor.Apply(pixels,640,360,640*4,Time{1000}) && pixels!=original,L"Circle appears at start");
    const auto rendered=pixels;
    pixels=original;
    Expect(compositor.Apply(pixels,640,360,640*4,Time{1200}) && pixels==rendered,L"Rasterized overlay is reused deterministically");
    pixels=original;
    Expect(compositor.Apply(pixels,640,360,640*4,Time{1500}) && pixels==original,L"Circle disappears at end");
    Expect(document.Erase(id) && document.Current()->Marks().empty(),L"Delete removes mark");
    Expect(document.Undo() && document.Find(id)!=nullptr,L"Deletion is undoable");
    auto arrow=document.NewMark(Tool::Arrow,Time{1000}); arrow.points={{100,200},{400,200}}; arrow.strokeWidth=8;
    const auto arrowShape=ArrowVertices(arrow);
    Expect(std::abs(arrowShape[0].y-200)<std::abs(arrowShape[1].y-200)*0.4F,L"Arrow tail is narrower than its neck");
    Expect(HitTest(arrow,{arrowShape[2].x+1,arrowShape[2].y-1},0),L"Filled arrowhead is draggable at its wing");
    Expect(!HitTest(arrow,{250,240},2),L"Arrow hit test rejects empty space beside shaft");
    Expect(document.Current()->Identity().starts_with(L"annotations-v2:"),L"Changed geometry invalidates old render caches");
    std::vector<std::uint8_t> overlay(640*360*4,0);
    Expect(Render(overlay,640,360,640*4,first,Time{1100},Surface::PremultipliedOverlay),L"Clean overlay renders");
    bool blueOutline=false;
    for (std::size_t i=0;i<overlay.size();i+=4)
        if (overlay[i]>overlay[i+2] && overlay[i+3]>100) { blueOutline=true; break; }
    Expect(!blueOutline,L"No selection rectangle pixels are drawn into the overlay");
    std::wcout<<L"PASS: annotation geometry, lifetime, identity, undo and cached compositor\n";
}
}

int wmain(int argc,wchar_t* argv[]) {
    if (argc<2 || argc>3) return 64;
    _setmode(_fileno(stdout),_O_U8TEXT);
    _setmode(_fileno(stderr),_O_U8TEXT);
    std::wcout<<std::unitbuf;
    const std::filesystem::path root=argv[1]; std::filesystem::create_directories(root);
    const auto isolatedAppData=root/L"appdata"; std::filesystem::create_directories(isolatedAppData);
    SetEnvironmentVariableW(L"SUPERRECORDING_TEST_DATA_ROOT",isolatedAppData.c_str());
    const qrec::win32::ScopedCoInitialize com(COINIT_MULTITHREADED);
    if (FAILED(com.Result()) || FAILED(MFStartup(MF_VERSION))) return 70;
    if (argc==3 && std::wstring_view(argv[2])==L"--probe-hybrid") {
        annotation_tests::HybridProbe(root); MFShutdown(); return annotation_tests::Failures()==0 ? 0 : 1;
    }
    annotation_tests::ModelTests();
    qrec::ExportRequest request;
    const bool uiOnly=argc==3 && std::wstring_view(argv[2])==L"--ui-only";
    const bool mediaOnly=argc==3 && std::wstring_view(argv[2])==L"--media-only";
    if (!uiOnly) request=annotation_tests::MediaTests(root);
    else {
        request.recording.sourcePath=root/L"source.mp4";
        request.recording.width=1280; request.recording.height=720;
        request.recording.framesPerSecond=60; request.recording.duration=std::chrono::milliseconds{6000};
    }
    if (!mediaOnly) annotation_tests::UiTests(root,request);
    MFShutdown();
    std::wcout<<L"annotationFailures="<<annotation_tests::Failures()<<L'\n';
    return annotation_tests::Failures()==0 ? 0 : 1;
}
