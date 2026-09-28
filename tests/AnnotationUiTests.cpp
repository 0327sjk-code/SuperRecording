#include "AnnotationTestSupport.h"
#include "editor/EditorAnnotations.h"
#include "editor/EditorWindow.h"
#include "editor/EditorTheme.h"
#include "common/Win32Helpers.h"
#include "ui/AntiAliasedDrawing.h"
#include <commctrl.h>
#include <wincodec.h>
#include <wrl/client.h>
#include <thread>
#include <iostream>
#include <map>

namespace annotation_tests {
namespace {
using namespace qrec;
using namespace qrec::annotations;
constexpr wchar_t kHostClass[]=L"SuperRecording.AnnotationTestHost";
LRESULT CALLBACK HostProc(HWND window,UINT message,WPARAM wParam,LPARAM lParam) {
    if (message==WM_ERASEBKGND) return 1;
    if (message==WM_PAINT) { PAINTSTRUCT ps{}; HDC dc=BeginPaint(window,&ps); FillRect(dc,&ps.rcPaint,GetSysColorBrush(COLOR_3DDKSHADOW)); EndPaint(window,&ps); return 0; }
    return DefWindowProcW(window,message,wParam,lParam);
}
void Pump(unsigned milliseconds) {
    const auto end=GetTickCount64()+milliseconds;
    std::map<std::pair<std::wstring,UINT>,unsigned> counts;
    do {
        MSG message{};
        unsigned count=0;
        while (count++<100 && PeekMessageW(&message,nullptr,0,0,PM_REMOVE)) {
            wchar_t name[128]{}; GetClassNameW(message.hwnd,name,128); ++counts[{name,message.message}];
            TranslateMessage(&message); DispatchMessageW(&message);
        }
        MsgWaitForMultipleObjectsEx(0,nullptr,10,QS_ALLINPUT,MWMO_INPUTAVAILABLE);
    } while (GetTickCount64()<end);
    for (const auto& [key,count] : counts) if (count>500)
        std::wcout<<L"UI high-frequency message: "<<key.first<<L" message="<<key.second<<L" count="<<count<<std::endl;
}
HWND FindClass(HWND root,const wchar_t* name) { return FindWindowExW(root,nullptr,name,nullptr); }
void Choose(HWND parent,int index) {
    HWND rail=FindWindowExW(parent,nullptr,L"SuperRecording.AnnotationControls",L"标注工具");
    SendMessageW(rail,WM_COMMAND,MAKEWPARAM(8100+index,BN_CLICKED),reinterpret_cast<LPARAM>(GetDlgItem(rail,8100+index)));
}
void Drag(HWND overlay,int x,int y,int endX,int endY,bool freehand=false) {
    SendMessageW(overlay,WM_LBUTTONDOWN,MK_LBUTTON,MAKELPARAM(x,y));
    if (freehand) {
        for (int i=1;i<40;++i) SendMessageW(overlay,WM_MOUSEMOVE,MK_LBUTTON,MAKELPARAM(x+i*2,y+(i%9)*3));
    }
    SendMessageW(overlay,WM_MOUSEMOVE,MK_LBUTTON,MAKELPARAM(endX,endY));
    SendMessageW(overlay,WM_LBUTTONUP,0,MAKELPARAM(endX,endY)); Pump(30);
}
struct PaintSnapshot final {
    int width{},height{};
    std::vector<BYTE> pixels;
};
bool SaveWindowPng(HWND window,const std::filesystem::path& path,PaintSnapshot* snapshot=nullptr) {
    RECT r{}; GetClientRect(window,&r);
    HDC dc=CreateCompatibleDC(nullptr);
    BITMAPINFO info{}; info.bmiHeader.biSize=sizeof(BITMAPINFOHEADER); info.bmiHeader.biWidth=r.right;
    info.bmiHeader.biHeight=-r.bottom; info.bmiHeader.biPlanes=1; info.bmiHeader.biBitCount=32;
    void* bytes=nullptr; HBITMAP bitmap=CreateDIBSection(dc,&info,DIB_RGB_COLORS,&bytes,nullptr,0);
    if (!bitmap) { DeleteDC(dc); return false; }
    auto old=SelectObject(dc,bitmap);
    const BOOL printed=PrintWindow(window,dc,PW_CLIENTONLY|2);
    using Microsoft::WRL::ComPtr;
    ComPtr<IWICImagingFactory> factory; ComPtr<IWICStream> stream; ComPtr<IWICBitmapEncoder> encoder; ComPtr<IWICBitmapFrameEncode> frame;
    HRESULT result=CoCreateInstance(CLSID_WICImagingFactory,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&factory));
    if (SUCCEEDED(result)) result=factory->CreateStream(&stream);
    if (SUCCEEDED(result)) result=stream->InitializeFromFilename(path.c_str(),GENERIC_WRITE);
    if (SUCCEEDED(result)) result=factory->CreateEncoder(GUID_ContainerFormatPng,nullptr,&encoder);
    if (SUCCEEDED(result)) result=encoder->Initialize(stream.Get(),WICBitmapEncoderNoCache);
    if (SUCCEEDED(result)) result=encoder->CreateNewFrame(&frame,nullptr);
    if (SUCCEEDED(result)) result=frame->Initialize(nullptr);
    if (SUCCEEDED(result)) result=frame->SetSize(static_cast<UINT>(r.right),static_cast<UINT>(r.bottom));
    // GDI paints BGRX; publish an opaque BGRA PNG, not WIC's negotiated 24-bit format.
    auto* pixels=static_cast<BYTE*>(bytes);
    for (LONG i=0;i<r.right*r.bottom;++i) pixels[static_cast<std::size_t>(i)*4+3]=255;
    if (snapshot) { snapshot->width=r.right; snapshot->height=r.bottom; snapshot->pixels.assign(pixels,pixels+static_cast<std::size_t>(r.right)*r.bottom*4); }
    auto format=GUID_WICPixelFormat32bppBGRA;
    if (SUCCEEDED(result)) result=frame->SetPixelFormat(&format);
    if (SUCCEEDED(result)) result=frame->WritePixels(static_cast<UINT>(r.bottom),static_cast<UINT>(r.right)*4,
        static_cast<UINT>(r.right*r.bottom)*4,static_cast<BYTE*>(bytes));
    if (SUCCEEDED(result)) result=frame->Commit();
    if (SUCCEEDED(result)) result=encoder->Commit();
    SelectObject(dc,old); DeleteObject(bitmap); DeleteDC(dc);
    return printed && SUCCEEDED(result);
}
void VerifyInlinePixels(const PaintSnapshot& empty,const PaintSnapshot& editing,const PaintSnapshot& committed,RECT input) {
    if (empty.width!=editing.width || editing.width!=committed.width || editing.pixels.empty()) {
        Expect(false,L"Input pixel fixtures have matching dimensions"); return;
    }
    unsigned liveGlyphs=0,changedGlyphs=0,changedBackground=0;
    const LONG clearStart=input.left+(input.right-input.left)*2/3;
    for (LONG y=input.top+3;y<input.bottom-3;++y) for (LONG x=input.left+3;x<input.right-3;++x) {
        if (x<0 || y<0 || x>=editing.width || y>=editing.height) continue;
        const auto i=(static_cast<std::size_t>(y)*editing.width+x)*4;
        const auto* a=empty.pixels.data()+i; const auto* b=editing.pixels.data()+i; const auto* c=committed.pixels.data()+i;
        const bool red=b[2]>160 && b[2]>b[1]*1.3 && b[2]>b[0]*1.3;
        if (red) { ++liveGlyphs; if (b[0]!=c[0] || b[1]!=c[1] || b[2]!=c[2]) ++changedGlyphs; }
        if (x>=clearStart && (a[0]!=b[0] || a[1]!=b[1] || a[2]!=b[2])) ++changedBackground;
    }
    Expect(liveGlyphs>30,L"Characters appear during native typing, before commit");
    Expect(changedGlyphs==0,L"Typed and committed glyph pixels have identical positions and appearance");
    Expect(changedBackground==0,L"Empty input interior preserves video pixels exactly, without any opaque panel");
    std::wcout<<L"Inline pixels: liveGlyphs="<<liveGlyphs<<L" changedGlyphs="<<changedGlyphs<<L" changedBackground="<<changedBackground<<std::endl;
}
void VerifyPreviewResize(HWND window,const std::filesystem::path& root) {
    const HWND preview=GetDlgItem(window,1001);
    wchar_t previewClass[80]{}; GetClassNameW(preview,previewClass,80);
    Expect(std::wstring_view(previewClass)==L"SuperRecording.PreviewSurface",L"Video owns its repaint surface instead of an erasing static control");
    SendMessageW(window,WM_COMMAND,MAKEWPARAM(1003,BN_CLICKED),reinterpret_cast<LPARAM>(GetDlgItem(window,1003)));
    Pump(120);
    unsigned blackFrames=0;
    constexpr std::array<SIZE,6> sizes{{{1500,1000},{960,680},{1400,720},{900,1000},{760,640},{1040,720}}};
    constexpr unsigned kResizeSamples=30;
    for (unsigned i=0;i<kResizeSamples;++i) {
        if (i==24) {
            SendMessageW(window,WM_COMMAND,MAKEWPARAM(1003,BN_CLICKED),reinterpret_cast<LPARAM>(GetDlgItem(window,1003)));
            Pump(80);
        }
        const auto size=sizes[i%sizes.size()];
        RECT bounds{0,0,size.cx,size.cy};
        AdjustWindowRectExForDpi(&bounds,static_cast<DWORD>(GetWindowLongPtrW(window,GWL_STYLE)),FALSE,
            static_cast<DWORD>(GetWindowLongPtrW(window,GWL_EXSTYLE)),GetDpiForWindow(window));
        SendMessageW(window,WM_ENTERSIZEMOVE,0,0);
        for (int burst=0;burst<4;++burst)
            SetWindowPos(window,nullptr,0,0,bounds.right-bounds.left+burst,bounds.bottom-bounds.top,SWP_NOMOVE|SWP_NOZORDER|SWP_NOACTIVATE);
        SetWindowPos(window,nullptr,0,0,bounds.right-bounds.left,bounds.bottom-bounds.top,SWP_NOMOVE|SWP_NOZORDER|SWP_NOACTIVATE);
        Pump(30);
        SendMessageW(window,WM_EXITSIZEMOVE,0,0);
        if (i%6==0) { ShowWindow(window,SW_MINIMIZE); Pump(20); ShowWindow(window,SW_SHOWNOACTIVATE); Pump(40); }
        // Reproduce an exposure after the coalesced EVR update has completed.
        RedrawWindow(preview,nullptr,nullptr,RDW_INVALIDATE|RDW_ERASE|RDW_UPDATENOW);
        Pump(35);
        PaintSnapshot snapshot;
        Expect(SaveWindowPng(window,root/L"editor-resize.png",&snapshot),L"Resize regression frame captured on isolated desktop");
        RECT area{}; GetWindowRect(preview,&area); MapWindowPoints(nullptr,window,reinterpret_cast<POINT*>(&area),2);
        unsigned colored=0;
        for (LONG y=area.top+8;y<area.bottom-8;y+=13) for (LONG x=area.left+8;x<area.right-8;x+=13) {
            if (x<0 || y<0 || x>=snapshot.width || y>=snapshot.height) continue;
            const auto* p=snapshot.pixels.data()+(static_cast<std::size_t>(y)*snapshot.width+x)*4;
            if (std::max({p[0],p[1],p[2]})-std::min({p[0],p[1],p[2]})>25) ++colored;
        }
        if (colored<30) ++blackFrames;
    }
    SendMessageW(window,WM_COMMAND,MAKEWPARAM(1003,BN_CLICKED),reinterpret_cast<LPARAM>(GetDlgItem(window,1003)));
    Pump(80);
    std::wcout<<L"Resize preview: blackFrames="<<blackFrames<<L" / "<<kResizeSamples<<L" (150 resize events; paused, playing and restore)"<<std::endl;
    Expect(blackFrames==0,L"Preview survives aspect changes, restore and late child repaint without black frames");
}
void ExerciseUi(const std::filesystem::path& root,const ExportRequest& request) {
    std::wcout<<L"UI: starting isolated host"<<std::endl;
    const HINSTANCE instance=GetModuleHandleW(nullptr);
    INITCOMMONCONTROLSEX common{sizeof(common),ICC_STANDARD_CLASSES|ICC_WIN95_CLASSES}; InitCommonControlsEx(&common);
    WNDCLASSEXW wc{sizeof(wc)}; wc.hInstance=instance; wc.lpfnWndProc=HostProc; wc.lpszClassName=kHostClass;
    RegisterClassExW(&wc);
    HWND host=CreateWindowExW(0,kHostClass,L"Isolated annotation tests",WS_OVERLAPPEDWINDOW|WS_CLIPCHILDREN,
        30,30,1100,800,nullptr,nullptr,instance,nullptr);
    Expect(host!=nullptr,L"Background test host created"); if (!host) return;
    ShowWindow(host,SW_SHOWNOACTIVATE);
    EditorChrome chrome; Expect(chrome.Initialize(host),L"Editor chrome initialized");
    int changes=0; std::vector<std::wstring> errors;
    EditorAnnotations annotations;
    Expect(annotations.Create(host,instance,&chrome,{1280,720},Time{6000},
        {[]{return Time{1033};},[&]{++changes;},[&](const std::wstring& e){errors.push_back(e);}}),L"Annotation UI created");
    std::wcout<<L"UI: controls created"<<std::endl;
    const auto layout=chrome.CalculateLayout(host,1040,720,request.recording);
    annotations.Layout(layout); annotations.SetPosition(Time{1000}); Pump(50);
    HWND overlay=FindClass(host,L"SuperRecording.AnnotationOverlay");
    Expect(overlay!=nullptr,L"Layered preview child exists");
    Choose(host,2); Drag(overlay,160,120,215,120);
    std::wcout<<L"UI: circle gesture"<<std::endl;
    auto scene=annotations.Snapshot();
    Expect(scene->Marks().size()==1 && scene->Marks()[0].tool==Tool::Circle,L"Native input draws circle");
    HWND annotationTrack=GetDlgItem(host,8200);
    HWND rail=FindWindowExW(host,nullptr,L"SuperRecording.AnnotationControls",L"标注工具");
    HWND widthSlider=GetDlgItem(rail,8203),colorButton=GetDlgItem(rail,8202);
    Expect(annotationTrack && widthSlider && colorButton,L"Left styles and annotation clip track created");
    Expect(layout.annotationTrack.bottom<=layout.timeline.top && !GetDlgItem(host,8201),L"Clip track replaces both old duration sliders above the video timeline");
    SetFocus(annotationTrack); SetFocus(overlay);
    Expect(annotations.Snapshot()->Marks().front().start==Time{1033},L"Focusing a track never rounds source time");
    const auto beforeRange=annotations.Snapshot(); const int beforeRangeChanges=changes;
    RECT sliderRect{}; GetClientRect(annotationTrack,&sliderRect);
    const int inset=MulDiv(24,static_cast<int>(GetDpiForWindow(annotationTrack)),96);
    Expect(sliderRect.bottom==MulDiv(38,static_cast<int>(GetDpiForWindow(annotationTrack)),96),L"Selected annotation uses exactly one compact clip row without help or progress");
    const int clipY=sliderRect.bottom/2;
    const auto timeX=[&](int milliseconds){return inset+static_cast<int>(std::lround((sliderRect.right-inset*2)*milliseconds/6000.0));};
    const int endX=timeX(3600);
    Drag(annotationTrack,timeX(1283),clipY,timeX(1283),clipY);
    Expect(annotations.Snapshot()==beforeRange && changes==beforeRangeChanges,L"Clicking a clip only selects; no snapping, history or encoding");
    SendMessageW(annotationTrack,WM_LBUTTONDOWN,MK_LBUTTON,MAKELPARAM(timeX(1533)-1,clipY));
    for (int i=0;i<=50;++i) SendMessageW(annotationTrack,WM_MOUSEMOVE,MK_LBUTTON,MAKELPARAM(timeX(1533)+(endX-timeX(1533))*i/50,clipY));
    Expect(changes==beforeRangeChanges && annotations.Snapshot()==beforeRange,L"Duration drag previews without encoding or undo spam");
    SendMessageW(annotationTrack,WM_LBUTTONUP,0,MAKELPARAM(endX,clipY));
    Expect(changes==beforeRangeChanges+1 && std::abs((annotations.Snapshot()->Marks().front().end-Time{3600}).count())<20,L"Duration drag commits once");
    Choose(host,5); Expect(annotations.Snapshot()->Identity()==beforeRange->Identity(),L"One undo restores entire duration gesture");
    const auto beforeMove=annotations.Snapshot(); const int beforeMoveChanges=changes;
    Drag(annotationTrack,timeX(1283),clipY,timeX(2883),clipY);
    auto moved=annotations.Snapshot()->Marks().front();
    Expect(moved.end-moved.start==Time{500} && std::abs((moved.start-Time{2633}).count())<20,L"Dragging clip body moves both boundaries without changing duration");
    Expect(changes==beforeMoveChanges+1,L"Clip movement creates one export revision");
    Choose(host,5); Expect(annotations.Snapshot()->Identity()==beforeMove->Identity(),L"Clip movement is one undo step");
    Drag(annotationTrack,timeX(1033),clipY,timeX(500),clipY);
    const auto resized=annotations.Snapshot()->Marks().front();
    Expect(std::abs((resized.start-Time{500}).count())<20 && resized.end==Time{1533},L"Left clip edge adjusts start independently");
    Choose(host,5);
    Drag(annotationTrack,timeX(1283),clipY,timeX(2883),clipY);
    const auto movedStart=annotations.Snapshot()->Marks().front().start;
    const int center=timeX(static_cast<int>(movedStart.count())+250);
    Drag(annotationTrack,center,clipY,center-(timeX(static_cast<int>(movedStart.count()))-timeX(1033))+2,clipY);
    Expect(annotations.Snapshot()->Marks().front().start==Time{1033},L"Clip boundary snaps exactly to the playhead");
    Choose(host,5); Choose(host,5);
    // Re-select the circle with the pen tool: no explicit Select-mode switch.
    Choose(host,1); Drag(overlay,215,120,215,120);
    Expect(annotations.Snapshot()->Marks().size()==1,L"Click existing mark with drawing tool selects without adding");
    const float beforeWidth=annotations.Snapshot()->Marks().front().strokeWidth;
    const int beforeWheelChanges=changes;
    POINT wheelPoint{20,24}; ClientToScreen(widthSlider,&wheelPoint);
    SendMessageW(widthSlider,WM_MOUSEWHEEL,MAKEWPARAM(0,WHEEL_DELTA),MAKELPARAM(wheelPoint.x,wheelPoint.y));
    Expect(changes==beforeWheelChanges,L"Wheel changes are debounced"); Pump(300);
    Expect(std::abs(annotations.Snapshot()->Marks().front().strokeWidth-beforeWidth-0.25F)<0.001F,L"Wheel changes width gently by 0.25 pixels");
    Expect(changes==beforeWheelChanges+1,L"Wheel batch commits once");
    const auto beforeWidthDrag=annotations.Snapshot();
    Drag(widthSlider,30,30,55,30);
    Expect(annotations.Snapshot()->Marks().front().strokeWidth>beforeWidth+1,L"Continuous width slider replaces three fixed presets");
    Choose(host,5); Expect(annotations.Snapshot()->Identity()==beforeWidthDrag->Identity(),L"One undo restores continuous width gesture");
    const auto beforeCancel=annotations.Snapshot(); const int beforeCancelChanges=changes;
    SendMessageW(annotationTrack,WM_LBUTTONDOWN,MK_LBUTTON,MAKELPARAM(timeX(1033),clipY));
    SendMessageW(annotationTrack,WM_MOUSEMOVE,MK_LBUTTON,MAKELPARAM(timeX(500),clipY));
    SendMessageW(annotationTrack,WM_KEYDOWN,VK_ESCAPE,0);
    Expect(annotations.Snapshot()==beforeCancel && changes==beforeCancelChanges,L"Escape cancels range drag without history or encoding");
    if (!scene->Marks().empty()) {
        const float expected=160.0F*1280/static_cast<float>(layout.preview.right-layout.preview.left);
        Expect(std::abs(scene->Marks()[0].points[0].x-expected)<0.01F,L"Preview to source coordinates preserve center");
    }
    Choose(host,1); Drag(overlay,270,80,360,150,true);
    std::wcout<<L"UI: pen gesture"<<std::endl;
    Expect(annotations.Snapshot()->Marks().back().points.size()>20,L"Native pen preserves freehand movement");
    Choose(host,3); Drag(overlay,80,250,220,200);
    Expect(annotations.Snapshot()->Marks().back().tool==Tool::Arrow,L"Native drag creates arrow");
    Choose(host,4); SendMessageW(overlay,WM_LBUTTONDOWN,MK_LBUTTON,MAKELPARAM(350,260));
    std::wcout<<L"UI: inline text opened"<<std::endl;
    HWND text=FindClass(overlay,L"Edit");
    Expect(text && IsWindowVisible(text) && !FindClass(host,L"SuperRecording.AnnotationTextInput"),L"Transparent input lives directly on the annotation canvas, without a panel");
    if (text) {
        RECT textBounds{},previewBounds{}; GetWindowRect(text,&textBounds); GetWindowRect(overlay,&previewBounds);
        Expect(textBounds.left>=previewBounds.left && textBounds.right<=previewBounds.right && textBounds.top>=previewBounds.top && textBounds.bottom<=previewBounds.bottom,L"Text input stays inside preview");
        SetWindowTextW(text,L"圈出这里 · 中文文字");
        SendMessageW(text,EM_SETSEL,static_cast<WPARAM>(-1),-1);
        Pump(50); Expect(SaveWindowPng(host,root/L"text-input.png"),L"Transparent text input paint captured");
        SendMessageW(text,WM_KEYDOWN,VK_RETURN,0);
    }
    Expect(annotations.Snapshot()->Marks().size()==4 && annotations.Snapshot()->Marks().back().text==L"圈出这里 · 中文文字",L"Chinese text commits without switching to selection mode");
    Expect(!FindClass(overlay,L"Edit"),L"Text input closes immediately on commit");
    // Each mark can move while a DIFFERENT creation tool remains active.
    const std::array<std::array<int,5>,4> moves{{{3,215,120,225,130},{2,270,80,280,90},
        {1,150,225,160,235},{3,370,275,380,280}}};
    for (std::size_t i=0;i<moves.size();++i) {
        Choose(host,moves[i][0]);
        const auto before=annotations.Snapshot();
        Drag(overlay,moves[i][1],moves[i][2],moves[i][3],moves[i][4]);
        const auto after=annotations.Snapshot();
        Expect(after->Marks().size()==4 && after->Marks()[i].points!=before->Marks()[i].points,L"All mark types drag directly under other active tools");
    }
    const auto beforeTextEdit=annotations.Snapshot();
    SendMessageW(overlay,WM_LBUTTONDBLCLK,MK_LBUTTON,MAKELPARAM(380,280));
    text=FindClass(overlay,L"Edit");
    Expect(text!=nullptr,L"Double-click edits existing text with any drawing tool active");
    if (text) {
        SetWindowTextW(text,L"中文输入法完成后提交");
        SendMessageW(text,WM_IME_STARTCOMPOSITION,0,0);
        SendMessageW(text,WM_KEYDOWN,VK_RETURN,0);
        Expect(IsWindow(text)!=FALSE,L"IME candidate confirmation does not finish the annotation");
        SendMessageW(text,WM_IME_ENDCOMPOSITION,0,0);
        SendMessageW(text,WM_KEYDOWN,VK_ESCAPE,0);
    }
    Expect(annotations.Snapshot()==beforeTextEdit,L"Escape cancels text changes without changing existing mark");
    Choose(host,3); Drag(overlay,225,130,225,130);
    const auto beforeSelectedMove=annotations.Snapshot();
    Drag(annotationTrack,timeX(1283),clipY,timeX(2083),clipY);
    const auto afterSelectedMove=annotations.Snapshot();
    Expect(afterSelectedMove->Marks().size()==4 && afterSelectedMove->Marks()[0].start!=beforeSelectedMove->Marks()[0].start &&
        afterSelectedMove->Marks()[1].start==beforeSelectedMove->Marks()[1].start &&
        afterSelectedMove->Marks()[2].start==beforeSelectedMove->Marks()[2].start &&
        afterSelectedMove->Marks()[3].start==beforeSelectedMove->Marks()[3].start,L"Clicking a mark switches the single clip strip; dragging changes only that mark");
    Choose(host,5);
    Choose(host,0); Drag(overlay,30,30,30,30);
    Expect(!IsWindowVisible(annotationTrack),L"No selected annotation means no empty progress track or instructions");
    Choose(host,4);
    SendMessageW(overlay,WM_LBUTTONDOWN,MK_LBUTTON,MAKELPARAM(400,30));
    text=FindClass(overlay,L"Edit");
    if (text) SetWindowTextW(text,L"点击空白直接完成");
    SendMessageW(overlay,WM_LBUTTONDOWN,MK_LBUTTON,MAKELPARAM(30,30));
    SendMessageW(overlay,WM_LBUTTONUP,0,MAKELPARAM(30,30)); Pump(30);
    Expect(annotations.Snapshot()->Marks().size()==5 && !FindClass(overlay,L"Edit"),L"Clicking outside text commits without reopening an input");
    Choose(host,5); Expect(annotations.Snapshot()->Marks().size()==4,L"UI undo text insertion");
    Choose(host,6); Expect(annotations.Snapshot()->Marks().size()==5,L"UI redo text insertion");
    Choose(host,5);
    Choose(host,0);
    annotations.SetPosition(Time{1500}); Pump(30); annotations.SetPosition(Time{1000}); Pump(30);
    annotations.Layout(chrome.CalculateLayout(host,790,650,request.recording)); Pump(50);
    const auto compact=chrome.CalculateLayout(host,760,640,request.recording);
    Expect(compact.statusLabel.bottom<=640 && compact.annotationTools.bottom<=compact.playButton.top,L"Minimum-size layout keeps all annotation/footer controls inside window");
    annotations.Layout(chrome.CalculateLayout(host,1280,850,request.recording)); Pump(50);
    annotations.SetEnabled(false); Choose(host,2); Drag(overlay,30,30,60,60);
    Expect(annotations.Snapshot()->Marks().size()==4,L"Busy state rejects annotation edits");
    annotations.SetEnabled(true);
    Expect(errors.empty(),L"Layered overlay updates without Win32/render errors");
    Expect(changes>=6,L"Committed gestures notify export invalidation");
    annotations.Destroy(); chrome.Destroy(); DestroyWindow(host);
    std::wcout<<L"UI: standalone controls destroyed"<<std::endl;

    EditorWindow editor(instance);
    AppSettings settings; settings.saveDirectory=root; settings.outputQualityPercent=100;
    std::wstring error;
    Expect(editor.Open(request.recording,settings,{},&error),L"Complete editor opens on isolated desktop");
    std::wcout<<L"UI: full editor opened"<<std::endl;
    if (editor.IsOpen()) {
        Pump(800); HWND window=editor.WindowHandle();
        RECT initial{}; GetClientRect(window,&initial);
        std::wcout<<L"Initial editor client: "<<initial.right<<L" x "<<initial.bottom<<std::endl;
        const UINT dpi=GetDpiForWindow(window);
        RECT requested{0,0,MulDiv(1500,static_cast<int>(dpi),96),MulDiv(1000,static_cast<int>(dpi),96)};
        const LONG requestedWidth=requested.right,requestedHeight=requested.bottom;
        AdjustWindowRectExForDpi(&requested,static_cast<DWORD>(GetWindowLongPtrW(window,GWL_STYLE)),FALSE,
            static_cast<DWORD>(GetWindowLongPtrW(window,GWL_EXSTYLE)),dpi);
        MONITORINFO monitor{sizeof(monitor)}; GetMonitorInfoW(MonitorFromWindow(window,MONITOR_DEFAULTTONEAREST),&monitor);
        const LONG frameWidth=requested.right-requested.left-requestedWidth,frameHeight=requested.bottom-requested.top-requestedHeight;
        Expect(initial.right==std::min(requestedWidth,monitor.rcWork.right-monitor.rcWork.left-frameWidth) &&
            initial.bottom==std::min(requestedHeight,monitor.rcWork.bottom-monitor.rcWork.top-frameHeight),
            L"Initial editor uses 1500x1000 DIP, clamped to its monitor work area");
        Expect(SaveWindowPng(window,root/L"editor-default.png"),L"Larger default editor layout captured");
        VerifyPreviewResize(window,root);
        Choose(window,2);
        HWND canvas=FindClass(window,L"SuperRecording.AnnotationOverlay"); Drag(canvas,230,140,300,140);
        Choose(window,3); Drag(canvas,85,260,180,185);
        Choose(window,1); Drag(canvas,355,85,445,115,true);
        Choose(window,4); SendMessageW(canvas,WM_LBUTTONDOWN,MK_LBUTTON,MAKELPARAM(355,245));
        HWND input=FindClass(canvas,L"Edit");
        PaintSnapshot emptyInput,liveInput,committedInput; RECT inputRectangle{};
        if (input) {
            GetWindowRect(input,&inputRectangle); MapWindowPoints(nullptr,window,reinterpret_cast<POINT*>(&inputRectangle),2);
            Expect(SaveWindowPng(window,root/L"editor-empty-input.png",&emptyInput),L"Empty transparent input baseline captured");
            for (wchar_t ch : std::wstring_view(L"标注文字")) SendMessageW(input,WM_CHAR,ch,0);
            Pump(50);
            Expect(SaveWindowPng(window,root/L"editor-text-entry.png",&liveInput),L"Real editor input is composited above video");
            SendMessageW(input,WM_KEYDOWN,VK_RETURN,0);
        }
        Pump(500);
        Expect(SaveWindowPng(window,root/L"editor-isolated.png",&committedInput),L"Background editor paint snapshot generated");
        if (input) VerifyInlinePixels(emptyInput,liveInput,committedInput,inputRectangle);
        RECT smallBounds{0,0,760,640};
        AdjustWindowRectExForDpi(&smallBounds,static_cast<DWORD>(GetWindowLongPtrW(window,GWL_STYLE)),FALSE,
            static_cast<DWORD>(GetWindowLongPtrW(window,GWL_EXSTYLE)),GetDpiForWindow(window));
        SetWindowPos(window,nullptr,0,0,smallBounds.right-smallBounds.left,smallBounds.bottom-smallBounds.top,SWP_NOMOVE|SWP_NOZORDER|SWP_NOACTIVATE);
        Pump(100);
        Expect(SaveWindowPng(window,root/L"editor-compact.png"),L"Minimum-size real editor paint snapshot generated");
        editor.CloseForShutdown(); Pump(100);
    }
    std::wcout<<L"PASS: native annotation gestures and real editor on a non-input desktop\n";
}
}
void UiTests(const std::filesystem::path& root,const qrec::ExportRequest& request) {
    std::thread isolated([&] {
        std::wcout<<L"UI: creating isolated desktop"<<std::endl;
        // Never switches the input desktop, sends real input, or touches user windows.
        HDESK original=GetThreadDesktop(GetCurrentThreadId());
        const auto name=L"SuperRecordingAnnotationTests-"+std::to_wstring(GetCurrentProcessId());
        HDESK desktop=CreateDesktopW(name.c_str(),nullptr,nullptr,0,GENERIC_ALL,nullptr);
        if (!desktop || !SetThreadDesktop(desktop)) { Expect(false,L"Isolated desktop required; refusing foreground test"); if (desktop) CloseDesktop(desktop); return; }
        {
            const qrec::win32::ScopedCoInitialize com(COINIT_APARTMENTTHREADED);
            SelectionUiTests();
            ExerciseUi(root,request);
        }
        SetThreadDesktop(original); CloseDesktop(desktop);
    });
    isolated.join();
}
}
