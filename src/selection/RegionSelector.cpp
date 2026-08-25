#include "RegionSelector.h"
#include "RegionSelectorView.h"

#include <windowsx.h>

#include <algorithm>
#include <cmath>
#include <utility>

namespace qrec::selection {
namespace {

constexpr wchar_t kWindowClassName[] = L"SuperRecording.RegionSelector.Window";
constexpr wchar_t kWindowTitle[] = L"SuperRecording - 选择录制区域";
constexpr int kMinimumRegionSize = 16;
constexpr int kBaseSnapDistance = 12;

[[nodiscard]] int ScaleForDpi(int value, UINT dpi) noexcept {
    return MulDiv(value, static_cast<int>(dpi), USER_DEFAULT_SCREEN_DPI);
}

[[nodiscard]] POINT PointFromMessage(LPARAM lParam) noexcept {
    return POINT{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
}

[[nodiscard]] UINT QueryMonitorDpi(HMONITOR monitor) noexcept {
    using GetDpiForMonitorFunction = HRESULT(WINAPI*)(HMONITOR, int, UINT*, UINT*);
    static const auto getDpiForMonitor = []() noexcept -> GetDpiForMonitorFunction {
        const HMODULE module = LoadLibraryW(L"shcore.dll");
        return module == nullptr
            ? nullptr
            : reinterpret_cast<GetDpiForMonitorFunction>(GetProcAddress(module, "GetDpiForMonitor"));
    }();

    if (getDpiForMonitor != nullptr) {
        UINT horizontalDpi = USER_DEFAULT_SCREEN_DPI;
        UINT verticalDpi = USER_DEFAULT_SCREEN_DPI;
        if (SUCCEEDED(getDpiForMonitor(monitor, 0, &horizontalDpi, &verticalDpi)) && horizontalDpi != 0) {
            return horizontalDpi;
        }
    }

    const HDC screen = GetDC(nullptr);
    if (screen == nullptr) {
        return USER_DEFAULT_SCREEN_DPI;
    }
    const int dpi = GetDeviceCaps(screen, LOGPIXELSX);
    ReleaseDC(nullptr, screen);
    return dpi > 0 ? static_cast<UINT>(dpi) : USER_DEFAULT_SCREEN_DPI;
}

class ScopedPerMonitorDpiAwareness final {
public:
    ScopedPerMonitorDpiAwareness() noexcept {
        using SetThreadDpiAwarenessContextFunction = DPI_AWARENESS_CONTEXT(WINAPI*)(DPI_AWARENESS_CONTEXT);
        const HMODULE user32 = GetModuleHandleW(L"user32.dll");
        setter_ = user32 == nullptr
            ? nullptr
            : reinterpret_cast<SetThreadDpiAwarenessContextFunction>(
                  GetProcAddress(user32, "SetThreadDpiAwarenessContext"));
        if (setter_ != nullptr) {
            previous_ = setter_(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
        }
    }

    ~ScopedPerMonitorDpiAwareness() {
        if (setter_ != nullptr && previous_ != nullptr) {
            setter_(previous_);
        }
    }

private:
    using Setter = DPI_AWARENESS_CONTEXT(WINAPI*)(DPI_AWARENESS_CONTEXT);
    Setter setter_{};
    DPI_AWARENESS_CONTEXT previous_{};
};

}  // namespace

RegionSelector::~RegionSelector() {
    if (window_ != nullptr) {
        DestroyWindow(window_);
        window_ = nullptr;
    }
}

std::optional<IntRect> RegionSelector::Select(HWND owner) {
    if (window_ != nullptr) {
        return std::nullopt;
    }

    ScopedPerMonitorDpiAwareness dpiAwareness;
    owner_ = owner;
    completed_ = false;
    result_.reset();
    mouseDown_ = false;
    hasDragged_ = false;
    selection_ = RECT{};

    if (!CreateSelectionWindow(owner) || window_ == nullptr) {
        return std::nullopt;
    }
    const HWND selectionWindow = window_;
    POINT initialPointer{};
    if (GetCursorPos(&initialPointer) && ScreenToClient(selectionWindow, &initialPointer)) {
        pointer_ = ClampToClient(initialPointer);
    }

    ShowWindow(selectionWindow, SW_SHOW);
    UpdateWindow(selectionWindow);
    SetForegroundWindow(selectionWindow);
    SetFocus(selectionWindow);

    bool receivedQuit = false;
    int quitCode = 0;
    MSG message{};
    while (!completed_) {
        const BOOL status = GetMessageW(&message, nullptr, 0, 0);
        if (status == -1) {
            Cancel();
            break;
        }
        if (status == 0) {
            receivedQuit = true;
            quitCode = static_cast<int>(message.wParam);
            Cancel();
            break;
        }

        TranslateMessage(&message);
        DispatchMessageW(&message);
    }

    if (window_ != nullptr) {
        DestroyWindow(window_);
        window_ = nullptr;
    }
    frameBuffer_.Reset();
    owner_ = nullptr;

    if (receivedQuit) {
        PostQuitMessage(quitCode);
    }
    return std::exchange(result_, std::nullopt);
}

bool RegionSelector::CreateSelectionWindow(HWND owner) {
    const HINSTANCE instance = GetModuleHandleW(nullptr);
    WNDCLASSEXW windowClass{};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.style = CS_HREDRAW | CS_VREDRAW;
    windowClass.lpfnWndProc = &RegionSelector::WindowProcedure;
    windowClass.hInstance = instance;
    windowClass.hCursor = LoadCursorW(nullptr, IDC_CROSS);
    windowClass.lpszClassName = kWindowClassName;

    if (RegisterClassExW(&windowClass) == 0 && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        return false;
    }

    RefreshMonitorAreas();
    const int width = virtualDesktop_.right - virtualDesktop_.left;
    const int height = virtualDesktop_.bottom - virtualDesktop_.top;
    if (width <= 0 || height <= 0) {
        return false;
    }

    window_ = CreateWindowExW(
        WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_LAYERED,
        kWindowClassName,
        kWindowTitle,
        WS_POPUP,
        virtualDesktop_.left,
        virtualDesktop_.top,
        width,
        height,
        owner,
        nullptr,
        instance,
        this);
    if (window_ == nullptr) {
        return false;
    }

    if (!SetLayeredWindowAttributes(
            window_, view::TransparentColor, view::MaskOpacity, LWA_COLORKEY | LWA_ALPHA)) {
        DestroyWindow(window_);
        window_ = nullptr;
        return false;
    }

#ifndef WDA_EXCLUDEFROMCAPTURE
#define WDA_EXCLUDEFROMCAPTURE 0x00000011
#endif
    SetWindowDisplayAffinity(window_, WDA_EXCLUDEFROMCAPTURE);
    return true;
}

void RegionSelector::Complete(const IntRect& region) {
    IntRect encodedRegion = region;
    if ((encodedRegion.Width() & 1) != 0) {
        --encodedRegion.right;
    }
    if ((encodedRegion.Height() & 1) != 0) {
        --encodedRegion.bottom;
    }
    if (!encodedRegion.IsValid(kMinimumRegionSize)) {
        return;
    }

    result_ = encodedRegion;
    completed_ = true;
}

void RegionSelector::Cancel() {
    result_.reset();
    completed_ = true;
}

void RegionSelector::RefreshMonitorAreas() {
    virtualDesktop_.left = GetSystemMetrics(SM_XVIRTUALSCREEN);
    virtualDesktop_.top = GetSystemMetrics(SM_YVIRTUALSCREEN);
    virtualDesktop_.right = virtualDesktop_.left + GetSystemMetrics(SM_CXVIRTUALSCREEN);
    virtualDesktop_.bottom = virtualDesktop_.top + GetSystemMetrics(SM_CYVIRTUALSCREEN);

    monitors_.clear();
    EnumDisplayMonitors(nullptr, nullptr, &RegionSelector::EnumerateMonitor, reinterpret_cast<LPARAM>(this));
    if (monitors_.empty()) {
        monitors_.push_back(MonitorArea{virtualDesktop_, USER_DEFAULT_SCREEN_DPI});
    }
}

BOOL CALLBACK RegionSelector::EnumerateMonitor(HMONITOR monitor, HDC, LPRECT, LPARAM context) {
    auto* selector = reinterpret_cast<RegionSelector*>(context);
    MONITORINFO information{};
    information.cbSize = sizeof(information);
    if (GetMonitorInfoW(monitor, &information)) {
        selector->monitors_.push_back(MonitorArea{information.rcMonitor, QueryMonitorDpi(monitor)});
    }
    return TRUE;
}

void RegionSelector::UpdatePointer(POINT clientPoint) {
    const POINT previousPointer = pointer_;
    pointer_ = ClampToClient(clientPoint);
    if (mouseDown_) {
        UpdateSelection(pointer_);
        return;
    }

    const POINT previousScreenPoint{
        previousPointer.x + virtualDesktop_.left,
        previousPointer.y + virtualDesktop_.top};
    const POINT currentScreenPoint{
        pointer_.x + virtualDesktop_.left,
        pointer_.y + virtualDesktop_.top};
    const RECT previousMonitor = MonitorBoundsAt(previousScreenPoint);
    const RECT currentMonitor = MonitorBoundsAt(currentScreenPoint);
    if (!EqualRect(&previousMonitor, &currentMonitor) ||
        DpiAt(previousScreenPoint) != DpiAt(currentScreenPoint)) {
        InvalidateFrame();
    }
}

void RegionSelector::UpdateSelection(POINT clientPoint) {
    const int dragWidth = std::abs(clientPoint.x - dragOrigin_.x);
    const int dragHeight = std::abs(clientPoint.y - dragOrigin_.y);
    const int dragThresholdX = std::max(2, GetSystemMetrics(SM_CXDRAG));
    const int dragThresholdY = std::max(2, GetSystemMetrics(SM_CYDRAG));
    if (dragWidth >= dragThresholdX || dragHeight >= dragThresholdY) {
        hasDragged_ = true;
    }

    selection_ = SnapSelection(NormalizeSelection(dragOrigin_, clientPoint), clientPoint);
    InvalidateFrame();
}

void RegionSelector::InvalidateFrame() {
    if (window_ != nullptr) {
        InvalidateRect(window_, nullptr, FALSE);
    }
}

POINT RegionSelector::ClampToClient(POINT point) const noexcept {
    const int width = virtualDesktop_.right - virtualDesktop_.left;
    const int height = virtualDesktop_.bottom - virtualDesktop_.top;
    point.x = std::clamp<LONG>(point.x, 0L, static_cast<LONG>(width));
    point.y = std::clamp<LONG>(point.y, 0L, static_cast<LONG>(height));
    return point;
}

RECT RegionSelector::NormalizeSelection(POINT first, POINT second) const noexcept {
    return RECT{
        std::min(first.x, second.x),
        std::min(first.y, second.y),
        std::max(first.x, second.x),
        std::max(first.y, second.y)};
}

RECT RegionSelector::SnapSelection(RECT selection, POINT pointer) const noexcept {
    const POINT screenPointer{pointer.x + virtualDesktop_.left, pointer.y + virtualDesktop_.top};
    const int snapDistance = std::max(1, ScaleForDpi(kBaseSnapDistance, DpiAt(screenPointer)));

    const auto snapCoordinate = [snapDistance](int value, const std::vector<int>& candidates) noexcept {
        int closest = value;
        int closestDistance = snapDistance + 1;
        for (const int candidate : candidates) {
            const int distance = std::abs(value - candidate);
            if (distance <= snapDistance && distance < closestDistance) {
                closest = candidate;
                closestDistance = distance;
            }
        }
        return closest;
    };

    std::vector<int> horizontalEdges;
    std::vector<int> verticalEdges;
    horizontalEdges.reserve(monitors_.size() * 2 + 2);
    verticalEdges.reserve(monitors_.size() * 2 + 2);
    horizontalEdges.push_back(0);
    horizontalEdges.push_back(virtualDesktop_.right - virtualDesktop_.left);
    verticalEdges.push_back(0);
    verticalEdges.push_back(virtualDesktop_.bottom - virtualDesktop_.top);

    for (const MonitorArea& monitor : monitors_) {
        horizontalEdges.push_back(monitor.bounds.left - virtualDesktop_.left);
        horizontalEdges.push_back(monitor.bounds.right - virtualDesktop_.left);
        verticalEdges.push_back(monitor.bounds.top - virtualDesktop_.top);
        verticalEdges.push_back(monitor.bounds.bottom - virtualDesktop_.top);
    }

    const RECT original = selection;
    selection.left = snapCoordinate(selection.left, horizontalEdges);
    selection.right = snapCoordinate(selection.right, horizontalEdges);
    selection.top = snapCoordinate(selection.top, verticalEdges);
    selection.bottom = snapCoordinate(selection.bottom, verticalEdges);
    if (selection.right <= selection.left) {
        selection.left = original.left;
        selection.right = original.right;
    }
    if (selection.bottom <= selection.top) {
        selection.top = original.top;
        selection.bottom = original.bottom;
    }
    return selection;
}

RECT RegionSelector::MonitorBoundsAt(POINT screenPoint) const noexcept {
    for (const MonitorArea& monitor : monitors_) {
        if (PtInRect(&monitor.bounds, screenPoint)) {
            return monitor.bounds;
        }
    }
    return virtualDesktop_;
}

UINT RegionSelector::DpiAt(POINT screenPoint) const noexcept {
    for (const MonitorArea& monitor : monitors_) {
        if (PtInRect(&monitor.bounds, screenPoint)) {
            return monitor.dpi;
        }
    }
    return USER_DEFAULT_SCREEN_DPI;
}

IntRect RegionSelector::ToScreenRect(const RECT& clientRect) const noexcept {
    return IntRect{
        clientRect.left + virtualDesktop_.left,
        clientRect.top + virtualDesktop_.top,
        clientRect.right + virtualDesktop_.left,
        clientRect.bottom + virtualDesktop_.top};
}

void RegionSelector::Paint() {
    const POINT activeScreenPoint{
        pointer_.x + virtualDesktop_.left,
        pointer_.y + virtualDesktop_.top};
    view::Paint(window_, selection_, DpiAt(activeScreenPoint), frameBuffer_);
}

LRESULT CALLBACK RegionSelector::WindowProcedure(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    auto* selector = reinterpret_cast<RegionSelector*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
        selector = static_cast<RegionSelector*>(create->lpCreateParams);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(selector));
    }
    if (selector == nullptr) {
        return DefWindowProcW(window, message, wParam, lParam);
    }

    switch (message) {
    case WM_ERASEBKGND:
        return 1;

    case WM_PAINT:
        selector->Paint();
        return 0;

    case WM_LBUTTONDOWN: {
        SetFocus(window);
        selector->dragOrigin_ = selector->ClampToClient(PointFromMessage(lParam));
        selector->pointer_ = selector->dragOrigin_;
        selector->selection_ = RECT{
            selector->dragOrigin_.x,
            selector->dragOrigin_.y,
            selector->dragOrigin_.x,
            selector->dragOrigin_.y};
        selector->mouseDown_ = true;
        selector->hasDragged_ = false;
        SetCapture(window);
        return 0;
    }

    case WM_MOUSEMOVE:
        selector->UpdatePointer(PointFromMessage(lParam));
        return 0;

    case WM_LBUTTONUP: {
        if (!selector->mouseDown_) {
            return 0;
        }
        selector->UpdatePointer(PointFromMessage(lParam));
        selector->mouseDown_ = false;
        if (GetCapture() == window) {
            ReleaseCapture();
        }

        if (!selector->hasDragged_) {
            const POINT screenPoint{
                selector->pointer_.x + selector->virtualDesktop_.left,
                selector->pointer_.y + selector->virtualDesktop_.top};
            const RECT monitor = selector->MonitorBoundsAt(screenPoint);
            selector->Complete(IntRect{monitor.left, monitor.top, monitor.right, monitor.bottom});
            return 0;
        }

        const IntRect selected = selector->ToScreenRect(selector->selection_);
        if (selected.IsValid(kMinimumRegionSize)) {
            selector->Complete(selected);
        } else {
            selector->selection_ = RECT{};
            selector->hasDragged_ = false;
            selector->InvalidateFrame();
        }
        return 0;
    }

    case WM_CAPTURECHANGED:
        if (selector->mouseDown_) {
            selector->mouseDown_ = false;
            selector->hasDragged_ = false;
            selector->selection_ = RECT{};
            selector->InvalidateFrame();
        }
        return 0;

    case WM_KEYDOWN:
        if (wParam == VK_ESCAPE) {
            selector->Cancel();
            return 0;
        }
        break;

    case WM_RBUTTONDOWN:
        selector->Cancel();
        return 0;

    case WM_DISPLAYCHANGE:
        selector->RefreshMonitorAreas();
        selector->frameBuffer_.Reset();
        SetWindowPos(
            window,
            HWND_TOPMOST,
            selector->virtualDesktop_.left,
            selector->virtualDesktop_.top,
            selector->virtualDesktop_.right - selector->virtualDesktop_.left,
            selector->virtualDesktop_.bottom - selector->virtualDesktop_.top,
            SWP_NOACTIVATE | SWP_SHOWWINDOW);
        selector->InvalidateFrame();
        return 0;

    case WM_SETCURSOR:
        SetCursor(LoadCursorW(nullptr, IDC_CROSS));
        return TRUE;

    case WM_CLOSE:
        selector->Cancel();
        return 0;

    case WM_NCDESTROY:
        SetWindowLongPtrW(window, GWLP_USERDATA, 0);
        if (selector->window_ == window) {
            selector->window_ = nullptr;
        }
        if (!selector->completed_) {
            selector->Cancel();
        }
        return DefWindowProcW(window, message, wParam, lParam);

    default:
        break;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

}  // namespace qrec::selection
