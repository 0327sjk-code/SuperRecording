#include "editor/AnnotationTimeline.h"
#include <windowsx.h>
#include <algorithm>
#include <cmath>
#include <utility>

namespace qrec {
namespace {
using annotations::Time;
constexpr wchar_t kClass[]=L"SuperRecording.AnnotationTimeline";
constexpr int kInsetDip=24,kRowTopDip=8,kBarHeightDip=22;
}
AnnotationTimeline::~AnnotationTimeline() { Destroy(); }
void AnnotationTimeline::Destroy() noexcept {
    callbacks_={};
    if (IsWindow(window_)) DestroyWindow(window_);
    window_=nullptr; scene_.reset(); draft_.reset();
    dragPart_=hoverPart_=Part::None; selected_=dragId_=hoverId_=0;
}
bool AnnotationTimeline::Create(HWND parent,HINSTANCE instance,int id,AnnotationTimelineCallbacks callbacks) {
    Destroy(); callbacks_=std::move(callbacks);
    WNDCLASSEXW wc{sizeof(wc)}; wc.hInstance=instance; wc.lpszClassName=kClass;
    wc.lpfnWndProc=WindowProc; wc.hCursor=LoadCursorW(nullptr,IDC_ARROW);
    if (!RegisterClassExW(&wc) && GetLastError()!=ERROR_CLASS_ALREADY_EXISTS) return false;
    window_=CreateWindowExW(0,kClass,L"标注片段 · 拖两端调整时长，拖中间移动",WS_CHILD|WS_VISIBLE|WS_TABSTOP|WS_CLIPSIBLINGS,
        0,0,1,1,parent,reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),instance,this);
    return window_!=nullptr;
}
int AnnotationTimeline::Dip(int value) const { return MulDiv(value,static_cast<int>(GetDpiForWindow(window_)),96); }
int AnnotationTimeline::TrackWidth() const {
    RECT r{}; GetClientRect(window_,&r); return std::max(1,static_cast<int>(r.right)-Dip(kInsetDip)*2);
}
int AnnotationTimeline::ToX(Time time) const {
    return Dip(kInsetDip)+static_cast<int>(std::lround(static_cast<double>(time.count())/std::max<std::int64_t>(1,duration_.count())*TrackWidth()));
}
const annotations::Mark* AnnotationTimeline::Find(std::uint64_t id) const {
    if (draft_ && draft_->id==id) return &*draft_;
    if (scene_) for (const auto& mark : scene_->Marks()) if (mark.id==id) return &mark;
    return nullptr;
}
RECT AnnotationTimeline::Bounds() const {
    const auto* mark=Find(selected_); if (!mark) return {};
    const int left=ToX(mark->start),right=ToX(mark->end);
    const int visibleWidth=std::max(Dip(14),right-left);
    const int visibleLeft=std::max(Dip(kInsetDip),std::min(left,ToX(duration_)-visibleWidth));
    const int top=Dip(kRowTopDip);
    return {visibleLeft,top,visibleLeft+visibleWidth,top+Dip(kBarHeightDip)};
}
AnnotationTimeline::Hit AnnotationTimeline::HitTest(POINT point) const {
    if (!selected_ || !Find(selected_)) return {};
    const RECT r=Bounds();
    if (point.y<r.top || point.y>=r.bottom || point.x<r.left-Dip(2) || point.x>r.right+Dip(2)) return {};
    const int grip=std::min(Dip(6),std::max(Dip(3),static_cast<int>(r.right-r.left)/4));
    const Part part=point.x<=r.left+grip ? Part::Start : point.x>=r.right-grip ? Part::End : Part::Body;
    return {selected_,part};
}
void AnnotationTimeline::Layout(const RECT& rectangle,HFONT font) {
    font_=font;
    SetWindowPos(window_,nullptr,rectangle.left,rectangle.top,rectangle.right-rectangle.left,rectangle.bottom-rectangle.top,SWP_NOACTIVATE|SWP_NOZORDER);
    InvalidateRect(window_,nullptr,FALSE);
}
void AnnotationTimeline::SetState(annotations::Snapshot scene,const annotations::Mark* selected,Time duration,bool enabled) {
    const std::uint64_t next=selected ? selected->id : 0;
    scene_=std::move(scene); duration_=std::max(duration,Time{1}); selected_=next;
    if (selected) draft_=*selected; else draft_.reset();
    EnableWindow(window_,enabled);
    ShowWindow(window_,selected ? SW_SHOWNOACTIVATE : SW_HIDE);
    InvalidateRect(window_,nullptr,FALSE);
}
void AnnotationTimeline::SetPosition(Time position) {
    // Kept for snap calculations only; annotations have no separate playhead.
    position_=position;
}
void AnnotationTimeline::Begin(POINT point) {
    if (!IsWindowEnabled(window_)) return;
    Finish(); SetFocus(window_);
    const Hit hit=HitTest(point); if (hit.part==Part::None) return;
    if (callbacks_.select) callbacks_.select(hit.id);
    const auto* mark=Find(hit.id); if (!mark) return;
    dragId_=hit.id; dragPart_=hit.part; downX_=point.x; dragMoved_=false;
    originalStart_=mark->start; originalEnd_=mark->end;
    SetCapture(window_); InvalidateRect(window_,nullptr,FALSE);
}
void AnnotationTimeline::Move(int x) {
    if (dragPart_==Part::None) return;
    if (!dragMoved_ && std::abs(x-downX_)<Dip(2)) return;
    dragMoved_=true;
    const auto delta=Time{static_cast<std::int64_t>(std::llround(static_cast<double>(x-downX_)*duration_.count()/TrackWidth()))};
    Time start=originalStart_,end=originalEnd_;
    if (dragPart_==Part::Start) start=std::clamp(start+delta,Time{},end-Time{1});
    else if (dragPart_==Part::End) end=std::clamp(end+delta,start+Time{1},duration_);
    else { const auto offset=std::clamp(delta,-start,duration_-end); start+=offset; end+=offset; }
    snap_.reset();
    if (!(GetKeyState(VK_SHIFT)&0x8000)) {
        std::vector<Time> targets{Time{},duration_,position_};
        if (scene_) for (const auto& mark : scene_->Marks()) if (mark.id!=dragId_) { targets.push_back(mark.start); targets.push_back(mark.end); }
        auto closest=Time{static_cast<std::int64_t>(std::ceil(static_cast<double>(Dip(6))*duration_.count()/TrackWidth()))};
        Time correction{};
        for (const auto target : targets) for (const bool left : {true,false}) {
            if ((left && dragPart_==Part::End) || (!left && dragPart_==Part::Start)) continue;
            const auto offset=target-(left ? start : end);
            const auto distance=Time{std::abs(offset.count())};
            const auto a=dragPart_==Part::End ? start : start+offset;
            const auto b=dragPart_==Part::Start ? end : end+offset;
            if (distance<=closest && a>=Time{} && b<=duration_ && b>a) {
                closest=distance; correction=offset; snap_=target;
            }
        }
        if (snap_) { if (dragPart_!=Part::End) start+=correction; if (dragPart_!=Part::Start) end+=correction; }
    }
    if (callbacks_.change) callbacks_.change(dragId_,start,end,AnnotationEditPhase::Preview);
    InvalidateRect(window_,nullptr,FALSE);
}
void AnnotationTimeline::Finish(bool cancel) {
    if (dragPart_==Part::None) return;
    const auto id=dragId_; dragPart_=Part::None; snap_.reset();
    if (GetCapture()==window_) ReleaseCapture();
    if (callbacks_.change) callbacks_.change(id,originalStart_,originalEnd_,cancel ? AnnotationEditPhase::Cancel : AnnotationEditPhase::Commit);
    InvalidateRect(window_,nullptr,FALSE);
}
void AnnotationTimeline::Nudge(WPARAM key) {
    const auto* mark=Find(selected_); if (!mark) return;
    const Time start=mark->start,end=mark->end;
    Time offset{key==VK_LEFT ? -10 : 10};
    if (key==VK_HOME) offset=-start;
    if (key==VK_END) offset=duration_-end;
    offset=std::clamp(offset,-start,duration_-end);
    if (callbacks_.change) {
        callbacks_.change(selected_,start+offset,end+offset,AnnotationEditPhase::Preview);
        callbacks_.change(selected_,start+offset,end+offset,AnnotationEditPhase::Commit);
    }
}
LRESULT CALLBACK AnnotationTimeline::WindowProc(HWND window,UINT message,WPARAM wParam,LPARAM lParam) {
    auto* self=reinterpret_cast<AnnotationTimeline*>(GetWindowLongPtrW(window,GWLP_USERDATA));
    if (message==WM_NCCREATE) {
        self=static_cast<AnnotationTimeline*>(reinterpret_cast<CREATESTRUCTW*>(lParam)->lpCreateParams);
        self->window_=window; SetWindowLongPtrW(window,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(self));
    }
    return self ? self->Message(message,wParam,lParam) : DefWindowProcW(window,message,wParam,lParam);
}
LRESULT AnnotationTimeline::Message(UINT message,WPARAM wParam,LPARAM lParam) {
    switch (message) {
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: { PAINTSTRUCT ps{}; HDC dc=BeginPaint(window_,&ps); Paint(dc); EndPaint(window_,&ps); return 0; }
    case WM_LBUTTONDOWN: Begin({GET_X_LPARAM(lParam),GET_Y_LPARAM(lParam)}); return 0;
    case WM_MOUSEMOVE: {
        if (dragPart_!=Part::None) Move(GET_X_LPARAM(lParam));
        else {
            const auto hit=HitTest({GET_X_LPARAM(lParam),GET_Y_LPARAM(lParam)});
            if (hoverId_!=hit.id || hoverPart_!=hit.part) { hoverId_=hit.id; hoverPart_=hit.part; InvalidateRect(window_,nullptr,FALSE); }
            TRACKMOUSEEVENT track{sizeof(track),TME_LEAVE,window_,0}; TrackMouseEvent(&track);
        }
        return 0;
    }
    case WM_MOUSELEAVE: hoverId_=0; hoverPart_=Part::None; InvalidateRect(window_,nullptr,FALSE); return 0;
    case WM_LBUTTONUP: if (dragPart_!=Part::None) { if (GET_X_LPARAM(lParam)!=downX_) Move(GET_X_LPARAM(lParam)); Finish(); } return 0;
    case WM_SETCURSOR: {
        POINT p{}; GetCursorPos(&p); ScreenToClient(window_,&p);
        const auto part=dragPart_==Part::None ? HitTest(p).part : dragPart_;
        SetCursor(LoadCursorW(nullptr,part==Part::Body ? IDC_SIZEALL : part==Part::None ? IDC_ARROW : IDC_SIZEWE)); return TRUE;
    }
    case WM_GETDLGCODE: return DLGC_WANTARROWS;
    case WM_KEYDOWN:
        if (wParam==VK_ESCAPE) { Finish(true); return 0; }
        if (wParam==VK_LEFT || wParam==VK_RIGHT || wParam==VK_HOME || wParam==VK_END) { Nudge(wParam); return 0; }
        break;
    case WM_CAPTURECHANGED: if (dragPart_!=Part::None) Finish(true); return 0;
    case WM_CANCELMODE: Finish(true); return 0;
    case WM_KILLFOCUS: Finish(); [[fallthrough]];
    case WM_SETFOCUS: InvalidateRect(window_,nullptr,FALSE); return 0;
    case WM_ENABLE: if (!wParam) Finish(true); InvalidateRect(window_,nullptr,FALSE); return 0;
    case WM_NCDESTROY: SetWindowLongPtrW(window_,GWLP_USERDATA,0); break;
    default: break;
    }
    return DefWindowProcW(window_,message,wParam,lParam);
}
}  // namespace qrec
