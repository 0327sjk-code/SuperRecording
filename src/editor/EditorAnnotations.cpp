#include "editor/EditorAnnotationsInternal.h"
#include "annotations/AnnotationRenderer.h"
#include <windowsx.h>
#include <commctrl.h>
#include <algorithm>
#include <cstring>
#include <utility>

namespace qrec {
namespace {
constexpr wchar_t kOverlayClass[]=L"SuperRecording.AnnotationOverlay";
constexpr UINT_PTR kPaintTimer=1;
constexpr UINT kCommitTextMessage=WM_APP+0x351;
}
EditorAnnotations::EditorAnnotations():impl_(std::make_unique<Impl>()) {}
EditorAnnotations::~EditorAnnotations()=default;
bool EditorAnnotations::Create(HWND p,HINSTANCE i,EditorChrome* c,annotations::Size s,annotations::Time d,EditorAnnotationCallbacks cb) {
    return impl_->Create(p,i,c,s,d,std::move(cb));
}
void EditorAnnotations::Destroy() noexcept { impl_->Destroy(); }
void EditorAnnotations::Layout(const EditorChromeLayout& l) { impl_->Layout(l); }
void EditorAnnotations::SetPosition(annotations::Time t,bool p) { impl_->SetPosition(t,p); }
void EditorAnnotations::SetEnabled(bool e) { impl_->SetEnabled(e); }
void EditorAnnotations::CommitText() { impl_->CommitProperty(); impl_->CommitText(); }
bool EditorAnnotations::HandleKey(const MSG& m) { return impl_->HandleKey(m); }
annotations::Snapshot EditorAnnotations::Snapshot() const noexcept { return impl_->Snapshot(); }

EditorAnnotations::Impl::~Impl() { Destroy(); }
void EditorAnnotations::Impl::Destroy() noexcept {
    destroying_=true; callbacks_={};
    if (IsWindow(text_)) DestroyWindow(text_);
    text_=nullptr;
    textBounds_={}; compositionText_.clear(); compositionBase_.clear();
    composing_=committingText_=false;
    if (textFont_) DeleteObject(textFont_);
    textFont_=nullptr;
    if (IsWindow(overlay_)) DestroyWindow(overlay_);
    overlay_=nullptr;
    controls_.Destroy(); ReleaseBitmap();
    draft_.reset(); moveOriginal_.reset(); propertyOriginal_.reset();
    gesture_=paintPending_=propertyEditing_=false; parent_=nullptr; chrome_=nullptr;
}
bool EditorAnnotations::Impl::Create(HWND parent,HINSTANCE instance,EditorChrome* chrome,
    annotations::Size canvas,annotations::Time duration,EditorAnnotationCallbacks callbacks) {
    Destroy(); destroying_=false; parent_=parent; instance_=instance; callbacks_=std::move(callbacks);
    chrome_=chrome;
    tool_=annotations::Tool::Select; selected_=0; color_=0xFFFF595E;
    time_={}; enabled_=true; playing_=false; document_.Reset(canvas,duration);
    width_=document_.NewMark(annotations::Tool::Pen,{}).strokeWidth;
    WNDCLASSEXW wc{sizeof(wc)}; wc.hInstance=instance; wc.lpfnWndProc=WindowProc;
    wc.lpszClassName=kOverlayClass; wc.hCursor=LoadCursorW(nullptr,IDC_CROSS); wc.style=CS_DBLCLKS;
    if (!RegisterClassExW(&wc) && GetLastError()!=ERROR_CLASS_ALREADY_EXISTS) return false;
    // A layered child is scoped to the editor; it never becomes a desktop topmost window.
    overlay_=CreateWindowExW(WS_EX_LAYERED,kOverlayClass,L"视频标注画布",
        WS_CHILD|WS_VISIBLE|WS_TABSTOP|WS_CLIPSIBLINGS,0,0,1,1,parent,nullptr,instance,this);
    if (!overlay_) return false;
    AnnotationControlCallbacks cb;
    cb.action=[this](AnnotationAction action){SelectTool(action);};
    cb.select=[this](std::uint64_t id){SelectMark(id);};
    cb.rangeChanged=[this](std::uint64_t id,annotations::Time start,annotations::Time end,AnnotationEditPhase phase){ChangeRange(id,start,end,phase);};
    cb.colorChanged=[this](std::uint32_t color){ChangeColor(color);};
    cb.widthChanged=[this](float width,AnnotationEditPhase phase){ChangeWidth(width,phase);};
    if (!controls_.Create(parent,instance,chrome,std::move(cb))) return false;
    RefreshControls(); return true;
}
void EditorAnnotations::Impl::ReleaseBitmap() noexcept {
    if (surfaceDc_ && previousBitmap_) SelectObject(surfaceDc_,previousBitmap_);
    if (bitmap_) DeleteObject(bitmap_);
    if (surfaceDc_) DeleteDC(surfaceDc_);
    bitmap_=nullptr; surfaceDc_=nullptr; previousBitmap_=nullptr; pixels_=nullptr;
    bitmapWidth_=bitmapHeight_=0;
}
bool EditorAnnotations::Impl::EnsureBitmap(int width,int height) {
    if (width<=0 || height<=0) return false;
    if (bitmap_ && width==bitmapWidth_ && height==bitmapHeight_) return true;
    ReleaseBitmap();
    surfaceDc_=CreateCompatibleDC(nullptr); if (!surfaceDc_) return false;
    BITMAPINFO info{}; info.bmiHeader.biSize=sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth=width; info.bmiHeader.biHeight=-height;
    info.bmiHeader.biPlanes=1; info.bmiHeader.biBitCount=32; info.bmiHeader.biCompression=BI_RGB;
    void* memory=nullptr;
    bitmap_=CreateDIBSection(surfaceDc_,&info,DIB_RGB_COLORS,&memory,nullptr,0);
    if (!bitmap_) { ReleaseBitmap(); return false; }
    previousBitmap_=SelectObject(surfaceDc_,bitmap_); pixels_=static_cast<std::uint8_t*>(memory);
    bitmapWidth_=width; bitmapHeight_=height; return true;
}
void EditorAnnotations::Impl::Layout(const EditorChromeLayout& layout) {
    if (!overlay_) return;
    previewBounds_=layout.preview;
    controls_.Layout(layout);
    SetWindowPos(overlay_,HWND_TOP,previewBounds_.left,previewBounds_.top,
        previewBounds_.right-previewBounds_.left,previewBounds_.bottom-previewBounds_.top,SWP_NOACTIVATE);
    LayoutText(); Paint();
}
void EditorAnnotations::Impl::SetPosition(annotations::Time time,bool playing) {
    bool repaint=playing_!=playing;
    const auto scene=document_.Current();
    if (scene) for (const auto& mark : scene->Marks())
        if (annotations::Visible(mark,time_)!=annotations::Visible(mark,time)) { repaint=true; break; }
    time_=time; playing_=playing; controls_.SetPosition(time);
    if (repaint) RequestPaint();
}
void EditorAnnotations::Impl::SetEnabled(bool enabled) {
    if (!enabled) { CommitProperty(); CommitText(); CancelGesture(); }
    enabled_=enabled; RefreshControls();
}
void EditorAnnotations::Impl::RefreshControls() {
    const auto* selected=propertyEditing_ && draft_ ? &*draft_ : document_.Find(selected_);
    if (selected) { color_=selected->argb; width_=selected->strokeWidth; }
    controls_.Refresh(tool_,document_,selected,enabled_,color_,width_);
    controls_.SetPosition(time_);
}
void EditorAnnotations::Impl::Changed() {
    if (selected_!=0 && !document_.Find(selected_)) selected_=0;
    RefreshControls(); RequestPaint();
    if (!destroying_ && callbacks_.changed) callbacks_.changed();
}
void EditorAnnotations::Impl::RequestPaint() {
    if (!overlay_ || paintPending_) return;
    paintPending_=SetTimer(overlay_,kPaintTimer,16,nullptr)!=0;
    if (!paintPending_) Paint();
}
void EditorAnnotations::Impl::Paint() {
    if (!overlay_ || !EnsureBitmap(previewBounds_.right-previewBounds_.left,previewBounds_.bottom-previewBounds_.top)) return;
    const std::size_t bytes=static_cast<std::size_t>(bitmapWidth_)*bitmapHeight_*4;
    // Alpha=1 preserves mouse hit-testing across the transparent canvas without obscuring the video.
    auto* packed=reinterpret_cast<std::uint32_t*>(pixels_);
    std::fill_n(packed,bytes/4,0x01000000U);
    const auto textDraft=text_ && draft_ ? std::optional{DisplayTextDraft()} : std::nullopt;
    if (!annotations::Render(std::span(pixels_,bytes),static_cast<unsigned>(bitmapWidth_),
            static_cast<unsigned>(bitmapHeight_),static_cast<unsigned>(bitmapWidth_)*4,
            document_.Current(),time_,annotations::Surface::PremultipliedOverlay,
            textDraft ? &*textDraft : draft_ ? &*draft_ : nullptr)) {
        if (callbacks_.error) callbacks_.error(L"标注预览绘制失败，源录屏未改动。");
        return;
    }
    if (text_) DrawTextInputGuide();
    POINT origin{}; SIZE size{bitmapWidth_,bitmapHeight_}; BLENDFUNCTION blend{AC_SRC_OVER,0,255,AC_SRC_ALPHA};
    if (!UpdateLayeredWindow(overlay_,nullptr,nullptr,&size,surfaceDc_,&origin,0,&blend,ULW_ALPHA) && callbacks_.error)
        callbacks_.error(L"无法刷新标注预览图层。");
}
LRESULT CALLBACK EditorAnnotations::Impl::WindowProc(HWND window,UINT message,WPARAM wParam,LPARAM lParam) {
    auto* self=reinterpret_cast<Impl*>(GetWindowLongPtrW(window,GWLP_USERDATA));
    if (message==WM_NCCREATE) {
        self=static_cast<Impl*>(reinterpret_cast<CREATESTRUCTW*>(lParam)->lpCreateParams);
        SetWindowLongPtrW(window,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(self));
    }
    return self ? self->Message(window,message,wParam,lParam) : DefWindowProcW(window,message,wParam,lParam);
}
LRESULT EditorAnnotations::Impl::Message(HWND window,UINT message,WPARAM wParam,LPARAM lParam) {
    switch (message) {
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: { PAINTSTRUCT ps{}; BeginPaint(window,&ps); EndPaint(window,&ps); return 0; }
    case WM_COMMAND:
        if (reinterpret_cast<HWND>(lParam)==text_ && HIWORD(wParam)==EN_CHANGE) { UpdateTextDraft(); return 0; }
        break;
    case WM_SETCURSOR: {
        POINT cursor{}; GetCursorPos(&cursor); ScreenToClient(window,&cursor);
        const bool hit=Pick(SourcePoint(cursor))!=nullptr;
        SetCursor(LoadCursorW(nullptr,hit ? IDC_SIZEALL : tool_==annotations::Tool::Select ? IDC_ARROW :
            (tool_==annotations::Tool::Text ? IDC_IBEAM : IDC_CROSS))); return TRUE;
    }
    case WM_LBUTTONDOWN: PointerDown({GET_X_LPARAM(lParam),GET_Y_LPARAM(lParam)}); return 0;
    case WM_MOUSEMOVE: PointerMove({GET_X_LPARAM(lParam),GET_Y_LPARAM(lParam)}); return 0;
    case WM_LBUTTONUP: PointerUp({GET_X_LPARAM(lParam),GET_Y_LPARAM(lParam)}); return 0;
    case WM_LBUTTONDBLCLK: {
        if (!enabled_) return 0;
        const auto point=SourcePoint({GET_X_LPARAM(lParam),GET_Y_LPARAM(lParam)});
        const auto* mark=Pick(point);
        if (mark && mark->tool==annotations::Tool::Text) { CancelGesture(); BeginText(point,mark); }
        return 0;
    }
    case WM_CAPTURECHANGED: if (gesture_) CancelGesture(); return 0;
    case WM_CANCELMODE: CancelGesture(); return 0;
    case WM_TIMER:
        if (wParam==kPaintTimer) { KillTimer(window,kPaintTimer); paintPending_=false; Paint(); return 0; }
        break;
    case kCommitTextMessage:
        if (text_==reinterpret_cast<HWND>(wParam)) CommitText();
        return 0;
    case WM_KEYDOWN: {
        MSG key{window,message,wParam,lParam,0,{}};
        if (HandleKey(key)) return 0;
        break;
    }
    case WM_NCDESTROY:
        KillTimer(window,kPaintTimer); paintPending_=false;
        SetWindowLongPtrW(window,GWLP_USERDATA,0); break;
    default: break;
    }
    return DefWindowProcW(window,message,wParam,lParam);
}
}  // namespace qrec
