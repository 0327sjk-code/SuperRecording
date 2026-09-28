#include "selection/SelectionInputSurface.h"
#include <windowsx.h>

namespace qrec::selection {
namespace {
constexpr wchar_t kClass[]=L"SuperRecording.SelectionInputSurface";
// Zero alpha is click-through. 1/255 keeps the live desktop visible but owns input.
constexpr BYTE kInputOpacity=1;
}
SelectionInputSurface::~SelectionInputSurface() { Reset(); }
void SelectionInputSurface::Reset() noexcept {
    if (IsWindow(window_)) DestroyWindow(window_);
    window_=owner_=nullptr;
}
bool SelectionInputSurface::Create(HWND owner) {
    Reset(); owner_=owner;
    const HINSTANCE instance=GetModuleHandleW(nullptr);
    WNDCLASSEXW wc{sizeof(wc)}; wc.hInstance=instance; wc.lpfnWndProc=WindowProc;
    wc.lpszClassName=kClass; wc.hCursor=LoadCursorW(nullptr,IDC_SIZEALL);
    if (!RegisterClassExW(&wc) && GetLastError()!=ERROR_CLASS_ALREADY_EXISTS) return false;
    window_=CreateWindowExW(WS_EX_TOPMOST|WS_EX_TOOLWINDOW|WS_EX_LAYERED|WS_EX_NOACTIVATE,
        kClass,L"选区拖动表面",WS_POPUP,0,0,1,1,owner,nullptr,instance,this);
    if (!window_ || !SetLayeredWindowAttributes(window_,0,kInputOpacity,LWA_ALPHA)) { Reset(); return false; }
    SetWindowDisplayAffinity(window_,WDA_EXCLUDEFROMCAPTURE);
    return true;
}
void SelectionInputSurface::Update(const RECT& selection,bool visible) noexcept {
    if (!window_) return;
    if (!visible || IsRectEmpty(&selection)) { ShowWindow(window_,SW_HIDE); return; }
    RECT screen=selection;
    MapWindowPoints(owner_,nullptr,reinterpret_cast<POINT*>(&screen),2);
    SetWindowPos(window_,HWND_TOP,screen.left,screen.top,screen.right-screen.left,screen.bottom-screen.top,
        SWP_NOACTIVATE|SWP_SHOWWINDOW);
}
LRESULT CALLBACK SelectionInputSurface::WindowProc(HWND window,UINT message,WPARAM wParam,LPARAM lParam) {
    auto* self=reinterpret_cast<SelectionInputSurface*>(GetWindowLongPtrW(window,GWLP_USERDATA));
    if (message==WM_NCCREATE) {
        self=static_cast<SelectionInputSurface*>(reinterpret_cast<CREATESTRUCTW*>(lParam)->lpCreateParams);
        SetWindowLongPtrW(window,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(self));
    }
    if (self) switch (message) {
    case WM_MOUSEACTIVATE: return MA_NOACTIVATE;
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: {
        PAINTSTRUCT paint{}; HDC dc=BeginPaint(window,&paint);
        FillRect(dc,&paint.rcPaint,static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
        EndPaint(window,&paint); return 0;
    }
    case WM_LBUTTONDOWN:
    case WM_LBUTTONUP:
    case WM_MOUSEMOVE:
    case WM_RBUTTONDOWN: {
        POINT point{GET_X_LPARAM(lParam),GET_Y_LPARAM(lParam)};
        MapWindowPoints(window,self->owner_,&point,1);
        return SendMessageW(self->owner_,message,wParam,MAKELPARAM(point.x,point.y));
    }
    case WM_SETCURSOR: return SendMessageW(self->owner_,message,reinterpret_cast<WPARAM>(self->owner_),lParam);
    case WM_NCDESTROY: self->window_=nullptr; SetWindowLongPtrW(window,GWLP_USERDATA,0); break;
    default: break;
    }
    return DefWindowProcW(window,message,wParam,lParam);
}
}  // namespace qrec::selection
