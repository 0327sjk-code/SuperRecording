#include "RecordingOverlay.h"

#include <windowsx.h>

namespace qrec::overlay {
namespace {

[[nodiscard]] POINT PointFromMessage(LPARAM lParam) noexcept {
    return POINT{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
}

}  // namespace

LRESULT CALLBACK RecordingOverlay::WindowProcedure(
    HWND window,
    UINT message,
    WPARAM wParam,
    LPARAM lParam) {
    auto* overlay = reinterpret_cast<RecordingOverlay*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
        overlay = static_cast<RecordingOverlay*>(create->lpCreateParams);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(overlay));
    }
    if (overlay == nullptr) {
        return DefWindowProcW(window, message, wParam, lParam);
    }

    switch (message) {
    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;

    case WM_ERASEBKGND:
        return 1;

    case WM_PAINT:
        overlay->Paint();
        return 0;

    case WM_TIMER:
        if (wParam == RefreshTimerId) {
            overlay->InvalidateStatusRegion();
            return 0;
        }
        if (wParam == MotionTimerId) {
            const bool active = overlay->AdvanceMotion();
            overlay->InvalidateMotionRegions();
            if (!active) {
                KillTimer(window, MotionTimerId);
            }
            return 0;
        }
        break;

    case WM_MOUSEMOVE: {
        if (overlay->dragging_) {
            POINT screenPoint{};
            GetCursorPos(&screenPoint);
            overlay->ContinueDrag(screenPoint);
            return 0;
        }
        overlay->TrackMouseLeave();
        const HitTarget next = overlay->HitTest(PointFromMessage(lParam));
        if (overlay->hovered_ != next) {
            overlay->SetHoveredTarget(next);
        }
        return 0;
    }

    case WM_MOUSELEAVE:
        overlay->trackingMouse_ = false;
        overlay->SetHoveredTarget(HitTarget::None);
        return 0;

    case WM_LBUTTONDOWN: {
        const POINT point = PointFromMessage(lParam);
        const HitTarget target = overlay->HitTest(point);
        if (target == HitTarget::DragHandle) {
            overlay->BeginDrag(point);
            return 0;
        }
        if (!overlay->stopping_ && (target == HitTarget::Pause || target == HitTarget::Stop)) {
            overlay->SetPressedTarget(target);
            SetCapture(window);
            if (GetCapture() != window) {
                overlay->SetPressedTarget(HitTarget::None);
            }
        }
        return 0;
    }

    case WM_LBUTTONUP: {
        if (overlay->dragging_) {
            overlay->EndDrag();
            return 0;
        }
        const HitTarget pressed = overlay->pressed_;
        overlay->SetPressedTarget(HitTarget::None);
        if (GetCapture() == window) {
            ReleaseCapture();
        }
        const HitTarget released = overlay->HitTest(PointFromMessage(lParam));
        if (pressed == released) {
            overlay->Activate(pressed);
        }
        return 0;
    }

    case WM_CAPTURECHANGED:
        overlay->dragging_ = false;
        overlay->SetPressedTarget(HitTarget::None);
        return 0;

    case WM_SETCURSOR: {
        POINT screenPoint{};
        GetCursorPos(&screenPoint);
        POINT clientPoint = screenPoint;
        ScreenToClient(window, &clientPoint);
        const HitTarget target = overlay->HitTest(clientPoint);
        if (target == HitTarget::DragHandle) {
            SetCursor(LoadCursorW(nullptr, IDC_SIZEALL));
            return TRUE;
        }
        if (target == HitTarget::Pause || target == HitTarget::Stop) {
            SetCursor(LoadCursorW(nullptr, IDC_HAND));
            return TRUE;
        }
        break;
    }

    case WM_DPICHANGED: {
        const UINT nextDpi = HIWORD(wParam);
        overlay->UpdateDpi(nextDpi);
        if (overlay->manuallyPositioned_) {
            const auto* suggested = reinterpret_cast<const RECT*>(lParam);
            const SIZE desired = overlay->DesiredSize();
            SetWindowPos(
                window,
                HWND_TOPMOST,
                suggested->left,
                suggested->top,
                desired.cx,
                desired.cy,
                SWP_NOACTIVATE | SWP_SHOWWINDOW);
            overlay->UpdateRoundedRegion();
        } else {
            overlay->Reposition(false);
        }
        InvalidateRect(window, nullptr, FALSE);
        return 0;
    }

    case WM_DISPLAYCHANGE:
        overlay->Reposition(true);
        return 0;

    case WM_CLOSE:
        overlay->Activate(HitTarget::Stop);
        return 0;

    case WM_NCDESTROY:
        KillTimer(window, RefreshTimerId);
        KillTimer(window, MotionTimerId);
        SetWindowLongPtrW(window, GWLP_USERDATA, 0);
        if (overlay->window_ == window) {
            overlay->window_ = nullptr;
        }
        return DefWindowProcW(window, message, wParam, lParam);

    default:
        break;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

}  // namespace qrec::overlay
