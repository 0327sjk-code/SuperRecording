#include "editor/PreviewSurface.h"
#include "editor/MediaPreview.h"
#include "editor/EditorTheme.h"

namespace qrec {
namespace { constexpr wchar_t kClass[]=L"SuperRecording.PreviewSurface"; }
PreviewSurface::~PreviewSurface() { if (IsWindow(window_)) DestroyWindow(window_); }
HWND PreviewSurface::Create(HWND parent,HINSTANCE instance,int id,MediaPreview* preview) {
    if (IsWindow(window_)) DestroyWindow(window_);
    preview_=preview;
    WNDCLASSEXW wc{sizeof(wc)}; wc.hInstance=instance; wc.lpfnWndProc=WindowProc;
    wc.lpszClassName=kClass; wc.hCursor=LoadCursorW(nullptr,IDC_ARROW);
    if (!RegisterClassExW(&wc) && GetLastError()!=ERROR_CLASS_ALREADY_EXISTS) return nullptr;
    window_=CreateWindowExW(0,kClass,L"",WS_CHILD|WS_VISIBLE|WS_CLIPSIBLINGS,
        0,0,1,1,parent,reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),instance,this);
    return window_;
}
LRESULT CALLBACK PreviewSurface::WindowProc(HWND window,UINT message,WPARAM wParam,LPARAM lParam) {
    auto* self=reinterpret_cast<PreviewSurface*>(GetWindowLongPtrW(window,GWLP_USERDATA));
    if (message==WM_NCCREATE) {
        self=static_cast<PreviewSurface*>(reinterpret_cast<CREATESTRUCTW*>(lParam)->lpCreateParams);
        SetWindowLongPtrW(window,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(self));
    }
    if (self) switch (message) {
    case WM_ERASEBKGND: return 1;
    case WM_SIZE: InvalidateRect(window,nullptr,FALSE); return 0;
    case WM_PAINT: {
        PAINTSTRUCT paint{}; HDC dc=BeginPaint(window,&paint);
        // MFPlay requires BeginPaint before repainting its retained video frame.
        // WM_PAINT coalesces layout bursts without an extra render timer or seek.
        if (!self->preview_ || !self->preview_->UpdateVideo()) {
            SetDCBrushColor(dc,editor_theme::VideoStage);
            FillRect(dc,&paint.rcPaint,static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
        }
        EndPaint(window,&paint); return 0;
    }
    case WM_NCDESTROY:
        self->window_=nullptr; SetWindowLongPtrW(window,GWLP_USERDATA,0); break;
    default: break;
    }
    return DefWindowProcW(window,message,wParam,lParam);
}
}  // namespace qrec
