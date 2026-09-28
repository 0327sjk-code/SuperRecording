#include "editor/AnnotationSlider.h"
#include "editor/EditorTheme.h"
#include "ui/AntiAliasedDrawing.h"
#include <windowsx.h>
#include <algorithm>
#include <cmath>
#include <format>
#include <utility>

namespace qrec {
namespace {
constexpr wchar_t kClass[]=L"SuperRecording.AnnotationSlider";
constexpr UINT_PTR kWheelCommit=1;
constexpr UINT kWheelQuietMilliseconds=220;
constexpr double kMinimumWidth=0.5;
constexpr double kWheelWidthStep=0.25;
constexpr double kTimeStepMilliseconds=10;
}
AnnotationSlider::~AnnotationSlider() { Destroy(); }
void AnnotationSlider::Destroy() noexcept {
    changed_={};
    if (IsWindow(window_)) DestroyWindow(window_);
    window_=nullptr; editing_=dragging_=hovered_=false;
}
bool AnnotationSlider::Create(HWND parent,HINSTANCE instance,int id,AnnotationSliderKind kind,
    std::function<void(double,AnnotationEditPhase)> changed) {
    Destroy(); kind_=kind; changed_=std::move(changed);
    WNDCLASSEXW wc{sizeof(wc)}; wc.hInstance=instance; wc.lpfnWndProc=WindowProc;
    wc.lpszClassName=kClass; wc.hCursor=LoadCursorW(nullptr,IDC_HAND);
    if (!RegisterClassExW(&wc) && GetLastError()!=ERROR_CLASS_ALREADY_EXISTS) return false;
    const auto title=kind==AnnotationSliderKind::Width ? L"粗细 · 滚轮微调" :
        kind==AnnotationSliderKind::Start ? L"标注起点" : L"标注终点";
    window_=CreateWindowExW(0,kClass,title,WS_CHILD|WS_VISIBLE|WS_TABSTOP|WS_CLIPSIBLINGS,
        0,0,1,1,parent,reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),instance,this);
    return window_!=nullptr;
}
int AnnotationSlider::Dip(int value) const { return MulDiv(value,static_cast<int>(GetDpiForWindow(window_)),96); }
int AnnotationSlider::Inset() const { return Dip(kind_==AnnotationSliderKind::Width ? 8 : 24); }
void AnnotationSlider::Layout(const RECT& rectangle,HFONT font) {
    font_=font;
    SetWindowPos(window_,nullptr,rectangle.left,rectangle.top,rectangle.right-rectangle.left,
        rectangle.bottom-rectangle.top,SWP_NOZORDER|SWP_NOACTIVATE);
    InvalidateRect(window_,nullptr,FALSE);
}
void AnnotationSlider::SetState(double value,double maximum,double start,double end,bool enabled) {
    if (value_==value && maximum_==maximum && start_==start && end_==end &&
        (IsWindowEnabled(window_)!=FALSE)==enabled) return;
    value_=value; maximum_=std::max(1.0,maximum); start_=start; end_=end;
    EnableWindow(window_,enabled); InvalidateRect(window_,nullptr,FALSE);
}
int AnnotationSlider::ValuePosition(double value) const {
    RECT r{}; GetClientRect(window_,&r);
    const double minimum=kind_==AnnotationSliderKind::Width ? kMinimumWidth : 0;
    double ratio=std::clamp((value-minimum)/(maximum_-minimum),0.0,1.0);
    // More travel at useful small widths, with full continuous control over thick strokes.
    if (kind_==AnnotationSliderKind::Width) ratio=std::sqrt(ratio);
    return Inset()+static_cast<int>(std::lround(ratio*std::max(1L,r.right-Inset()*2)));
}
double AnnotationSlider::PositionValue(int x) const {
    RECT r{}; GetClientRect(window_,&r);
    double ratio=std::clamp(static_cast<double>(x-Inset())/std::max(1L,r.right-Inset()*2),0.0,1.0);
    const double minimum=kind_==AnnotationSliderKind::Width ? kMinimumWidth : 0;
    if (kind_==AnnotationSliderKind::Width) ratio*=ratio;
    return minimum+ratio*(maximum_-minimum);
}
void AnnotationSlider::Preview(double value) {
    if (!editing_) { editing_=true; original_=value_; }
    const double minimum=kind_==AnnotationSliderKind::Width ? kMinimumWidth :
        kind_==AnnotationSliderKind::End ? start_+1 : 0;
    const double maximum=kind_==AnnotationSliderKind::Start ? end_-1 : maximum_;
    value_=std::clamp(value,minimum,std::max(minimum,maximum));
    if (changed_) changed_(value_,AnnotationEditPhase::Preview);
    InvalidateRect(window_,nullptr,FALSE);
}
void AnnotationSlider::Finish(bool cancel) {
    if (!window_) return;
    KillTimer(window_,kWheelCommit);
    const bool wasEditing=editing_;
    editing_=dragging_=false;
    if (GetCapture()==window_) ReleaseCapture();
    if (wasEditing) {
        if (cancel) value_=original_;
        if (changed_) changed_(value_,cancel ? AnnotationEditPhase::Cancel : AnnotationEditPhase::Commit);
        InvalidateRect(window_,nullptr,FALSE);
    }
}
void AnnotationSlider::Paint(HDC target) {
    RECT r{}; GetClientRect(window_,&r);
    HDC buffer=CreateCompatibleDC(target);
    HBITMAP bitmap=CreateCompatibleBitmap(target,std::max(1L,r.right),std::max(1L,r.bottom));
    HGDIOBJ old=buffer && bitmap ? SelectObject(buffer,bitmap) : nullptr;
    HDC dc=old ? buffer : target;
    const HBRUSH background=CreateSolidBrush(editor_theme::Panel);
    FillRect(dc,&r,background); DeleteObject(background);
    const bool enabled=IsWindowEnabled(window_)!=FALSE;
    {
        ui::Canvas canvas(dc);
        const int y=static_cast<int>(r.bottom)-Dip(8);
        const int left=Inset(), right=std::max(left+1,static_cast<int>(r.right)-Inset());
        const int thickness=std::max(1,Dip(3));
        RECT track{left,y-thickness/2,right,y+(thickness+1)/2};
        canvas.FillRoundedRectangle(track,static_cast<float>(Dip(2)),editor_theme::BorderStrong);
        RECT span=track;
        span.left=kind_==AnnotationSliderKind::Width ? left : ValuePosition(start_);
        span.right=kind_==AnnotationSliderKind::Width ? ValuePosition(value_) : ValuePosition(end_);
        if (enabled) canvas.FillRoundedRectangle(span,static_cast<float>(Dip(2)),editor_theme::Focus);
        if (enabled) {
            const float x=static_cast<float>(ValuePosition(value_));
            const float cy=static_cast<float>(y);
            if (hovered_ || dragging_ || GetFocus()==window_)
                canvas.FillEllipse(x,cy,static_cast<float>(Dip(7)),static_cast<float>(Dip(7)),editor_theme::SelectionBorder);
            canvas.FillEllipse(x,cy,static_cast<float>(Dip(4)),static_cast<float>(Dip(4)),
                dragging_ ? editor_theme::Focus : editor_theme::TextPrimary);
        }
    }
    const auto previous=SelectObject(dc,font_); SetBkMode(dc,TRANSPARENT);
    SetTextColor(dc,enabled ? editor_theme::TextSecondary : editor_theme::TextDisabled);
    RECT label{Inset(),0,r.right-Inset(),r.bottom-Dip(13)};
    std::wstring caption;
    if (kind_==AnnotationSliderKind::Width) caption=std::format(L"粗细 {:.1f}",value_);
    else {
        const wchar_t* name=kind_==AnnotationSliderKind::Start ? L"标注起点" : L"标注终点";
        caption=enabled ? std::format(L"{}  {:.3f} s",name,value_/1000.0) : std::format(L"{} · 点击标注后调整",name);
    }
    DrawTextW(dc,caption.c_str(),-1,&label,DT_LEFT|DT_SINGLELINE|DT_VCENTER|DT_END_ELLIPSIS|DT_NOPREFIX);
    if (kind_==AnnotationSliderKind::End && enabled) {
        caption=std::format(L"持续 {:.3f} s",(end_-start_)/1000.0);
        DrawTextW(dc,caption.c_str(),-1,&label,DT_RIGHT|DT_SINGLELINE|DT_VCENTER|DT_NOPREFIX);
    }
    SelectObject(dc,previous);
    if (old) { BitBlt(target,0,0,r.right,r.bottom,buffer,0,0,SRCCOPY); SelectObject(buffer,old); }
    if (bitmap) DeleteObject(bitmap); if (buffer) DeleteDC(buffer);
}
LRESULT CALLBACK AnnotationSlider::WindowProc(HWND window,UINT message,WPARAM wParam,LPARAM lParam) {
    auto* self=reinterpret_cast<AnnotationSlider*>(GetWindowLongPtrW(window,GWLP_USERDATA));
    if (message==WM_NCCREATE) {
        self=static_cast<AnnotationSlider*>(reinterpret_cast<CREATESTRUCTW*>(lParam)->lpCreateParams);
        self->window_=window; SetWindowLongPtrW(window,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(self));
    }
    return self ? self->Message(message,wParam,lParam) : DefWindowProcW(window,message,wParam,lParam);
}
LRESULT AnnotationSlider::Message(UINT message,WPARAM wParam,LPARAM lParam) {
    switch (message) {
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: { PAINTSTRUCT ps{}; HDC dc=BeginPaint(window_,&ps); Paint(dc); EndPaint(window_,&ps); return 0; }
    case WM_LBUTTONDOWN:
        SetFocus(window_); Finish(); dragging_=true; SetCapture(window_); Preview(PositionValue(GET_X_LPARAM(lParam))); return 0;
    case WM_MOUSEMOVE:
        if (!hovered_) { hovered_=true; TRACKMOUSEEVENT track{sizeof(track),TME_LEAVE,window_,0}; TrackMouseEvent(&track); InvalidateRect(window_,nullptr,FALSE); }
        if (dragging_) Preview(PositionValue(GET_X_LPARAM(lParam))); return 0;
    case WM_MOUSELEAVE: hovered_=false; InvalidateRect(window_,nullptr,FALSE); return 0;
    case WM_LBUTTONUP: if (dragging_) { Preview(PositionValue(GET_X_LPARAM(lParam))); Finish(); } return 0;
    case WM_MOUSEWHEEL: {
        POINT point{GET_X_LPARAM(lParam),GET_Y_LPARAM(lParam)}; ScreenToClient(window_,&point);
        RECT r{}; GetClientRect(window_,&r);
        if (!PtInRect(&r,point) || dragging_) break;
        const double step=kind_==AnnotationSliderKind::Width ? kWheelWidthStep : kTimeStepMilliseconds;
        Preview(value_+static_cast<double>(GET_WHEEL_DELTA_WPARAM(wParam))/WHEEL_DELTA*step);
        SetTimer(window_,kWheelCommit,kWheelQuietMilliseconds,nullptr); return 0;
    }
    case WM_TIMER: if (wParam==kWheelCommit) { Finish(); return 0; } break;
    case WM_GETDLGCODE: return DLGC_WANTARROWS;
    case WM_KEYDOWN:
        if (wParam==VK_ESCAPE && editing_) { Finish(true); return 0; }
        if (wParam==VK_LEFT || wParam==VK_DOWN || wParam==VK_RIGHT || wParam==VK_UP || wParam==VK_HOME || wParam==VK_END) {
            const double step=kind_==AnnotationSliderKind::Width ? kWheelWidthStep : kTimeStepMilliseconds;
            const double value=wParam==VK_HOME ? 0 : wParam==VK_END ? maximum_ :
                value_+(wParam==VK_LEFT || wParam==VK_DOWN ? -step : step);
            Preview(value); Finish(); return 0;
        }
        break;
    case WM_KILLFOCUS: Finish(); [[fallthrough]];
    case WM_SETFOCUS: InvalidateRect(window_,nullptr,FALSE); return 0;
    case WM_CAPTURECHANGED: if (dragging_) Finish(true); return 0;
    case WM_CANCELMODE: Finish(true); return 0;
    case WM_ENABLE: if (!wParam) Finish(); InvalidateRect(window_,nullptr,FALSE); return 0;
    case WM_NCDESTROY: KillTimer(window_,kWheelCommit); SetWindowLongPtrW(window_,GWLP_USERDATA,0); break;
    default: break;
    }
    return DefWindowProcW(window_,message,wParam,lParam);
}
}  // namespace qrec
