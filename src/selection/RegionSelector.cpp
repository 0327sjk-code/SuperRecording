#include "RegionSelector.h"
#include "RegionSelectorView.h"

#include <windowsx.h>

#include <algorithm>
#include <array>
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

std::optional<IntRect> RegionSelector::Select(
    HWND owner,
    const bool adjustSelectionBeforeRecording) {
    if (window_ != nullptr) {
        return std::nullopt;
    }

    ScopedPerMonitorDpiAwareness dpiAwareness;
    owner_ = owner;
    completed_ = false;
    result_.reset();
    mouseDown_ = false;
    hasDragged_ = false;
    adjustSelectionBeforeRecording_ = adjustSelectionBeforeRecording;
    phase_ = Phase::Selecting;
    activeAdjustment_ = AdjustmentHit::None;
    hoveredControl_ = view::ControlButton::None;
    pressedControl_ = view::ControlButton::None;
    selection_ = RECT{};
    adjustmentOriginSelection_ = RECT{};
    adjustmentOrigin_ = POINT{};

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

    // The modal selection surface is destroyed before Select() returns to the
    // capture pipeline. Hiding it here also makes that ordering explicit to
    // the compositor on machines where window destruction is deferred.
    if (adjustSelectionBeforeRecording_ && window_ != nullptr) {
        ShowWindow(window_, SW_HIDE);
    }
    result_ = encodedRegion;
    completed_ = true;
}

void RegionSelector::Cancel() {
    result_.reset();
    completed_ = true;
}

void RegionSelector::FinishInitialSelection(const IntRect& region) {
    RECT clientRegion = ToClientRect(RECT{
        region.left,
        region.top,
        region.right,
        region.bottom});
    const POINT bottomRight{clientRegion.right, clientRegion.bottom};
    clientRegion = MakeInitialSelectionEven(clientRegion, bottomRight);
    selection_ = clientRegion;
    if (!ToScreenRect(selection_).IsValid(kMinimumRegionSize)) {
        selection_ = RECT{};
        hasDragged_ = false;
        InvalidateFrame();
        return;
    }

    if (!adjustSelectionBeforeRecording_) {
        Complete(ToScreenRect(selection_));
        return;
    }
    EnterAdjustment();
}

void RegionSelector::EnterAdjustment() {
    phase_ = Phase::Adjusting;
    mouseDown_ = false;
    activeAdjustment_ = AdjustmentHit::None;
    pressedControl_ = view::ControlButton::None;
    hoveredControl_ = HitTestControl(pointer_);
    InvalidateFrame();
    if (window_ != nullptr) {
        SetCursor(CursorForPoint(pointer_));
    }
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
        if (phase_ == Phase::Adjusting) {
            if (activeAdjustment_ != AdjustmentHit::None) {
                UpdateAdjustedSelection(pointer_);
            }
            const view::ControlButton hovered = HitTestControl(pointer_);
            if (hovered != hoveredControl_) {
                hoveredControl_ = hovered;
                InvalidateFrame();
            }
        } else {
            UpdateSelection(pointer_);
        }
        return;
    }

    if (phase_ == Phase::Adjusting) {
        const view::ControlButton hovered = HitTestControl(pointer_);
        if (hovered != hoveredControl_) {
            hoveredControl_ = hovered;
            InvalidateFrame();
        }
        if (window_ != nullptr) {
            SetCursor(CursorForPoint(pointer_));
        }
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

    selection_ = MakeInitialSelectionEven(
        SnapSelection(NormalizeSelection(dragOrigin_, clientPoint), clientPoint),
        clientPoint);
    InvalidateFrame();
}

void RegionSelector::BeginAdjustment(POINT clientPoint) {
    pointer_ = ClampToClient(clientPoint);
    hoveredControl_ = HitTestControl(pointer_);
    if (hoveredControl_ != view::ControlButton::None) {
        if (hoveredControl_ == view::ControlButton::Start && !IsSelectionValid()) {
            return;
        }
        pressedControl_ = hoveredControl_;
        activeAdjustment_ = AdjustmentHit::None;
        mouseDown_ = true;
        SetCapture(window_);
        InvalidateFrame();
        return;
    }

    activeAdjustment_ = HitTestAdjustment(pointer_);
    if (activeAdjustment_ == AdjustmentHit::None) {
        return;
    }
    adjustmentOrigin_ = pointer_;
    adjustmentOriginSelection_ = selection_;
    pressedControl_ = view::ControlButton::None;
    mouseDown_ = true;
    SetCapture(window_);
}

void RegionSelector::EndPointerInteraction(POINT clientPoint) {
    if (phase_ != Phase::Adjusting || !mouseDown_) {
        return;
    }

    UpdatePointer(clientPoint);
    const view::ControlButton releasedControl = HitTestControl(pointer_);
    const view::ControlButton pressedControl = pressedControl_;
    mouseDown_ = false;
    activeAdjustment_ = AdjustmentHit::None;
    pressedControl_ = view::ControlButton::None;
    if (GetCapture() == window_) {
        ReleaseCapture();
    }

    if (pressedControl != view::ControlButton::None &&
        releasedControl == pressedControl) {
        if (pressedControl == view::ControlButton::Cancel) {
            Cancel();
            return;
        }
        if (pressedControl == view::ControlButton::Start && IsSelectionValid()) {
            Complete(ToScreenRect(selection_));
            return;
        }
    }
    hoveredControl_ = HitTestControl(pointer_);
    InvalidateFrame();
    if (window_ != nullptr) {
        SetCursor(CursorForPoint(pointer_));
    }
}

void RegionSelector::UpdateAdjustedSelection(POINT clientPoint) {
    RECT adjusted = adjustmentOriginSelection_;
    const LONG deltaX = clientPoint.x - adjustmentOrigin_.x;
    const LONG deltaY = clientPoint.y - adjustmentOrigin_.y;
    const int clientWidth = virtualDesktop_.right - virtualDesktop_.left;
    const int clientHeight = virtualDesktop_.bottom - virtualDesktop_.top;

    if (activeAdjustment_ == AdjustmentHit::Move) {
        const LONG width = adjusted.right - adjusted.left;
        const LONG height = adjusted.bottom - adjusted.top;
        adjusted.left += deltaX;
        adjusted.right = adjusted.left + width;
        adjusted.top += deltaY;
        adjusted.bottom = adjusted.top + height;
        if (adjusted.left < 0) {
            adjusted.right -= adjusted.left;
            adjusted.left = 0;
        }
        if (adjusted.right > clientWidth) {
            const LONG overflow = adjusted.right - clientWidth;
            adjusted.left -= overflow;
            adjusted.right = clientWidth;
        }
        if (adjusted.top < 0) {
            adjusted.bottom -= adjusted.top;
            adjusted.top = 0;
        }
        if (adjusted.bottom > clientHeight) {
            const LONG overflow = adjusted.bottom - clientHeight;
            adjusted.top -= overflow;
            adjusted.bottom = clientHeight;
        }
        selection_ = SnapMovedSelection(adjusted, clientPoint);
        InvalidateFrame();
        return;
    }

    const bool changesLeft = activeAdjustment_ == AdjustmentHit::Left ||
        activeAdjustment_ == AdjustmentHit::TopLeft ||
        activeAdjustment_ == AdjustmentHit::BottomLeft;
    const bool changesRight = activeAdjustment_ == AdjustmentHit::Right ||
        activeAdjustment_ == AdjustmentHit::TopRight ||
        activeAdjustment_ == AdjustmentHit::BottomRight;
    const bool changesTop = activeAdjustment_ == AdjustmentHit::Top ||
        activeAdjustment_ == AdjustmentHit::TopLeft ||
        activeAdjustment_ == AdjustmentHit::TopRight;
    const bool changesBottom = activeAdjustment_ == AdjustmentHit::Bottom ||
        activeAdjustment_ == AdjustmentHit::BottomLeft ||
        activeAdjustment_ == AdjustmentHit::BottomRight;

    if (changesLeft) {
        adjusted.left = std::clamp<LONG>(
            adjustmentOriginSelection_.left + deltaX,
            0,
            adjusted.right - kMinimumRegionSize);
    }
    if (changesRight) {
        adjusted.right = std::clamp<LONG>(
            adjustmentOriginSelection_.right + deltaX,
            adjusted.left + kMinimumRegionSize,
            clientWidth);
    }
    if (changesTop) {
        adjusted.top = std::clamp<LONG>(
            adjustmentOriginSelection_.top + deltaY,
            0,
            adjusted.bottom - kMinimumRegionSize);
    }
    if (changesBottom) {
        adjusted.bottom = std::clamp<LONG>(
            adjustmentOriginSelection_.bottom + deltaY,
            adjusted.top + kMinimumRegionSize,
            clientHeight);
    }

    adjusted = SnapAdjustedSelection(adjusted, clientPoint);
    if (changesLeft) {
        adjusted.left = std::min<LONG>(
            adjusted.left,
            adjusted.right - kMinimumRegionSize);
    }
    if (changesRight) {
        adjusted.right = std::max<LONG>(
            adjusted.right,
            adjusted.left + kMinimumRegionSize);
    }
    if (changesTop) {
        adjusted.top = std::min<LONG>(
            adjusted.top,
            adjusted.bottom - kMinimumRegionSize);
    }
    if (changesBottom) {
        adjusted.bottom = std::max<LONG>(
            adjusted.bottom,
            adjusted.top + kMinimumRegionSize);
    }
    selection_ = MakeAdjustedSelectionEven(adjusted);
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

RECT RegionSelector::SnapMovedSelection(RECT selection, POINT pointer) const noexcept {
    const POINT screenPointer{pointer.x + virtualDesktop_.left, pointer.y + virtualDesktop_.top};
    const int snapDistance = std::max(1, ScaleForDpi(kBaseSnapDistance, DpiAt(screenPointer)));
    const int clientWidth = virtualDesktop_.right - virtualDesktop_.left;
    const int clientHeight = virtualDesktop_.bottom - virtualDesktop_.top;

    int offsetX = 0;
    int offsetY = 0;
    int bestHorizontalDistance = snapDistance + 1;
    int bestVerticalDistance = snapDistance + 1;
    const auto considerHorizontal = [&](const int candidate) noexcept {
        for (const int edge : {
                 static_cast<int>(selection.left),
                 static_cast<int>(selection.right)}) {
            const int distance = std::abs(candidate - edge);
            if (distance <= snapDistance && distance < bestHorizontalDistance) {
                offsetX = candidate - edge;
                bestHorizontalDistance = distance;
            }
        }
    };
    const auto considerVertical = [&](const int candidate) noexcept {
        for (const int edge : {
                 static_cast<int>(selection.top),
                 static_cast<int>(selection.bottom)}) {
            const int distance = std::abs(candidate - edge);
            if (distance <= snapDistance && distance < bestVerticalDistance) {
                offsetY = candidate - edge;
                bestVerticalDistance = distance;
            }
        }
    };
    considerHorizontal(0);
    considerHorizontal(clientWidth);
    considerVertical(0);
    considerVertical(clientHeight);
    for (const MonitorArea& monitor : monitors_) {
        considerHorizontal(monitor.bounds.left - virtualDesktop_.left);
        considerHorizontal(monitor.bounds.right - virtualDesktop_.left);
        considerVertical(monitor.bounds.top - virtualDesktop_.top);
        considerVertical(monitor.bounds.bottom - virtualDesktop_.top);
    }
    selection.left += offsetX;
    selection.right += offsetX;
    selection.top += offsetY;
    selection.bottom += offsetY;

    if (selection.left < 0) {
        selection.right -= selection.left;
        selection.left = 0;
    }
    if (selection.right > clientWidth) {
        const LONG overflow = selection.right - clientWidth;
        selection.left -= overflow;
        selection.right = clientWidth;
    }
    if (selection.top < 0) {
        selection.bottom -= selection.top;
        selection.top = 0;
    }
    if (selection.bottom > clientHeight) {
        const LONG overflow = selection.bottom - clientHeight;
        selection.top -= overflow;
        selection.bottom = clientHeight;
    }
    return selection;
}

RECT RegionSelector::SnapAdjustedSelection(RECT selection, POINT pointer) const noexcept {
    const POINT screenPointer{pointer.x + virtualDesktop_.left, pointer.y + virtualDesktop_.top};
    const int snapDistance = std::max(1, ScaleForDpi(kBaseSnapDistance, DpiAt(screenPointer)));
    const int clientWidth = virtualDesktop_.right - virtualDesktop_.left;
    const int clientHeight = virtualDesktop_.bottom - virtualDesktop_.top;

    const auto snapHorizontal = [&](const int value) noexcept {
        int closest = value;
        int closestDistance = snapDistance + 1;
        const auto consider = [&](const int candidate) noexcept {
            const int distance = std::abs(candidate - value);
            if (distance <= snapDistance && distance < closestDistance) {
                closest = candidate;
                closestDistance = distance;
            }
        };
        consider(0);
        consider(clientWidth);
        for (const MonitorArea& monitor : monitors_) {
            consider(monitor.bounds.left - virtualDesktop_.left);
            consider(monitor.bounds.right - virtualDesktop_.left);
        }
        return closest;
    };
    const auto snapVertical = [&](const int value) noexcept {
        int closest = value;
        int closestDistance = snapDistance + 1;
        const auto consider = [&](const int candidate) noexcept {
            const int distance = std::abs(candidate - value);
            if (distance <= snapDistance && distance < closestDistance) {
                closest = candidate;
                closestDistance = distance;
            }
        };
        consider(0);
        consider(clientHeight);
        for (const MonitorArea& monitor : monitors_) {
            consider(monitor.bounds.top - virtualDesktop_.top);
            consider(monitor.bounds.bottom - virtualDesktop_.top);
        }
        return closest;
    };

    switch (activeAdjustment_) {
    case AdjustmentHit::Left:
    case AdjustmentHit::TopLeft:
    case AdjustmentHit::BottomLeft:
        selection.left = snapHorizontal(selection.left);
        break;
    case AdjustmentHit::Right:
    case AdjustmentHit::TopRight:
    case AdjustmentHit::BottomRight:
        selection.right = snapHorizontal(selection.right);
        break;
    default:
        break;
    }
    switch (activeAdjustment_) {
    case AdjustmentHit::Top:
    case AdjustmentHit::TopLeft:
    case AdjustmentHit::TopRight:
        selection.top = snapVertical(selection.top);
        break;
    case AdjustmentHit::Bottom:
    case AdjustmentHit::BottomLeft:
    case AdjustmentHit::BottomRight:
        selection.bottom = snapVertical(selection.bottom);
        break;
    default:
        break;
    }
    return selection;
}

RECT RegionSelector::MakeInitialSelectionEven(RECT selection, POINT pointer) const noexcept {
    if (((selection.right - selection.left) & 1) != 0) {
        if (pointer.x >= dragOrigin_.x) {
            --selection.right;
        } else {
            ++selection.left;
        }
    }
    if (((selection.bottom - selection.top) & 1) != 0) {
        if (pointer.y >= dragOrigin_.y) {
            --selection.bottom;
        } else {
            ++selection.top;
        }
    }
    return selection;
}

RECT RegionSelector::MakeAdjustedSelectionEven(RECT selection) const noexcept {
    const bool changesLeft = activeAdjustment_ == AdjustmentHit::Left ||
        activeAdjustment_ == AdjustmentHit::TopLeft ||
        activeAdjustment_ == AdjustmentHit::BottomLeft;
    const bool changesTop = activeAdjustment_ == AdjustmentHit::Top ||
        activeAdjustment_ == AdjustmentHit::TopLeft ||
        activeAdjustment_ == AdjustmentHit::TopRight;
    if (((selection.right - selection.left) & 1) != 0) {
        if (changesLeft) {
            ++selection.left;
        } else {
            --selection.right;
        }
    }
    if (((selection.bottom - selection.top) & 1) != 0) {
        if (changesTop) {
            ++selection.top;
        } else {
            --selection.bottom;
        }
    }
    return selection;
}

bool RegionSelector::IsSelectionValid() const noexcept {
    return ToScreenRect(selection_).IsValid(kMinimumRegionSize) &&
        IsContainedInSingleMonitor(selection_);
}

bool RegionSelector::IsContainedInSingleMonitor(const RECT& clientRect) const noexcept {
    const RECT screenRect{
        clientRect.left + virtualDesktop_.left,
        clientRect.top + virtualDesktop_.top,
        clientRect.right + virtualDesktop_.left,
        clientRect.bottom + virtualDesktop_.top};
    for (const MonitorArea& monitor : monitors_) {
        if (screenRect.left >= monitor.bounds.left &&
            screenRect.top >= monitor.bounds.top &&
            screenRect.right <= monitor.bounds.right &&
            screenRect.bottom <= monitor.bounds.bottom) {
            return true;
        }
    }
    return false;
}

RegionSelector::AdjustmentHit RegionSelector::HitTestAdjustment(
    POINT clientPoint) const noexcept {
    if (phase_ != Phase::Adjusting || view::IsEmpty(selection_)) {
        return AdjustmentHit::None;
    }

    const POINT screenPoint{
        clientPoint.x + virtualDesktop_.left,
        clientPoint.y + virtualDesktop_.top};
    const int handleRadius = std::max(7, ScaleForDpi(9, DpiAt(screenPoint)));
    const int edgeRadius = std::max(5, ScaleForDpi(7, DpiAt(screenPoint)));
    const std::array<std::pair<POINT, AdjustmentHit>, 4> corners{{
        {POINT{selection_.left, selection_.top}, AdjustmentHit::TopLeft},
        {POINT{selection_.right - 1, selection_.top}, AdjustmentHit::TopRight},
        {POINT{selection_.left, selection_.bottom - 1}, AdjustmentHit::BottomLeft},
        {POINT{selection_.right - 1, selection_.bottom - 1}, AdjustmentHit::BottomRight},
    }};
    for (const auto& [center, hit] : corners) {
        if (std::abs(clientPoint.x - center.x) <= handleRadius &&
            std::abs(clientPoint.y - center.y) <= handleRadius) {
            return hit;
        }
    }

    if (std::abs(clientPoint.y - selection_.top) <= edgeRadius &&
        clientPoint.x >= selection_.left - edgeRadius &&
        clientPoint.x <= selection_.right + edgeRadius) {
        return AdjustmentHit::Top;
    }
    if (std::abs(clientPoint.y - (selection_.bottom - 1)) <= edgeRadius &&
        clientPoint.x >= selection_.left - edgeRadius &&
        clientPoint.x <= selection_.right + edgeRadius) {
        return AdjustmentHit::Bottom;
    }
    if (std::abs(clientPoint.x - selection_.left) <= edgeRadius &&
        clientPoint.y >= selection_.top - edgeRadius &&
        clientPoint.y <= selection_.bottom + edgeRadius) {
        return AdjustmentHit::Left;
    }
    if (std::abs(clientPoint.x - (selection_.right - 1)) <= edgeRadius &&
        clientPoint.y >= selection_.top - edgeRadius &&
        clientPoint.y <= selection_.bottom + edgeRadius) {
        return AdjustmentHit::Right;
    }

    const RECT interior{
        selection_.left + edgeRadius,
        selection_.top + edgeRadius,
        selection_.right - edgeRadius,
        selection_.bottom - edgeRadius};
    if (PtInRect(&interior, clientPoint)) {
        return AdjustmentHit::Move;
    }
    return AdjustmentHit::None;
}

view::ControlButton RegionSelector::HitTestControl(POINT clientPoint) const noexcept {
    if (phase_ != Phase::Adjusting || view::IsEmpty(selection_)) {
        return view::ControlButton::None;
    }
    const RECT client{
        0,
        0,
        virtualDesktop_.right - virtualDesktop_.left,
        virtualDesktop_.bottom - virtualDesktop_.top};
    const POINT selectionCenter{
        (selection_.left + selection_.right) / 2 + virtualDesktop_.left,
        (selection_.top + selection_.bottom) / 2 + virtualDesktop_.top};
    const view::ControlLayout layout = view::CalculateControlLayout(
        client,
        selection_,
        ActiveMonitorClientBounds(),
        DpiAt(selectionCenter));
    if (PtInRect(&layout.startButton, clientPoint)) {
        return view::ControlButton::Start;
    }
    if (PtInRect(&layout.cancelButton, clientPoint)) {
        return view::ControlButton::Cancel;
    }
    return view::ControlButton::None;
}

HCURSOR RegionSelector::CursorForPoint(POINT clientPoint) const noexcept {
    const view::ControlButton control = HitTestControl(clientPoint);
    if (control == view::ControlButton::Cancel ||
        (control == view::ControlButton::Start && IsSelectionValid())) {
        return LoadCursorW(nullptr, IDC_HAND);
    }
    switch (HitTestAdjustment(clientPoint)) {
    case AdjustmentHit::Move:
        return LoadCursorW(nullptr, IDC_SIZEALL);
    case AdjustmentHit::Left:
    case AdjustmentHit::Right:
        return LoadCursorW(nullptr, IDC_SIZEWE);
    case AdjustmentHit::Top:
    case AdjustmentHit::Bottom:
        return LoadCursorW(nullptr, IDC_SIZENS);
    case AdjustmentHit::TopLeft:
    case AdjustmentHit::BottomRight:
        return LoadCursorW(nullptr, IDC_SIZENWSE);
    case AdjustmentHit::TopRight:
    case AdjustmentHit::BottomLeft:
        return LoadCursorW(nullptr, IDC_SIZENESW);
    case AdjustmentHit::None:
    default:
        return LoadCursorW(nullptr, IDC_ARROW);
    }
}

RECT RegionSelector::MonitorBoundsAt(POINT screenPoint) const noexcept {
    for (const MonitorArea& monitor : monitors_) {
        if (PtInRect(&monitor.bounds, screenPoint)) {
            return monitor.bounds;
        }
    }
    const HMONITOR nearest = MonitorFromPoint(screenPoint, MONITOR_DEFAULTTONEAREST);
    MONITORINFO information{};
    information.cbSize = sizeof(information);
    if (nearest != nullptr && GetMonitorInfoW(nearest, &information)) {
        return information.rcMonitor;
    }
    return virtualDesktop_;
}

RECT RegionSelector::ActiveMonitorClientBounds() const noexcept {
    RECT screenSelection{
        selection_.left + virtualDesktop_.left,
        selection_.top + virtualDesktop_.top,
        selection_.right + virtualDesktop_.left,
        selection_.bottom + virtualDesktop_.top};
    const HMONITOR nearest = MonitorFromRect(&screenSelection, MONITOR_DEFAULTTONEAREST);
    MONITORINFO information{};
    information.cbSize = sizeof(information);
    if (nearest != nullptr && GetMonitorInfoW(nearest, &information)) {
        return ToClientRect(information.rcMonitor);
    }
    return RECT{
        0,
        0,
        virtualDesktop_.right - virtualDesktop_.left,
        virtualDesktop_.bottom - virtualDesktop_.top};
}

UINT RegionSelector::DpiAt(POINT screenPoint) const noexcept {
    for (const MonitorArea& monitor : monitors_) {
        if (PtInRect(&monitor.bounds, screenPoint)) {
            return monitor.dpi;
        }
    }
    const HMONITOR nearest = MonitorFromPoint(screenPoint, MONITOR_DEFAULTTONEAREST);
    return nearest != nullptr ? QueryMonitorDpi(nearest) : USER_DEFAULT_SCREEN_DPI;
}

IntRect RegionSelector::ToScreenRect(const RECT& clientRect) const noexcept {
    return IntRect{
        clientRect.left + virtualDesktop_.left,
        clientRect.top + virtualDesktop_.top,
        clientRect.right + virtualDesktop_.left,
        clientRect.bottom + virtualDesktop_.top};
}

RECT RegionSelector::ToClientRect(const RECT& screenRect) const noexcept {
    return RECT{
        screenRect.left - virtualDesktop_.left,
        screenRect.top - virtualDesktop_.top,
        screenRect.right - virtualDesktop_.left,
        screenRect.bottom - virtualDesktop_.top};
}

void RegionSelector::Paint() {
    const POINT activeScreenPoint = phase_ == Phase::Adjusting && !view::IsEmpty(selection_)
        ? POINT{
              (selection_.left + selection_.right) / 2 + virtualDesktop_.left,
              (selection_.top + selection_.bottom) / 2 + virtualDesktop_.top}
        : POINT{
              pointer_.x + virtualDesktop_.left,
              pointer_.y + virtualDesktop_.top};
    view::SelectionUiState state;
    state.adjusting = phase_ == Phase::Adjusting;
    state.selectionValid = !state.adjusting || IsSelectionValid();
    state.hoveredButton = hoveredControl_;
    state.pressedButton = pressedControl_;
    state.activeMonitorBounds = ActiveMonitorClientBounds();
    view::Paint(
        window_,
        selection_,
        DpiAt(activeScreenPoint),
        state,
        frameBuffer_);
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
        if (selector->phase_ == Phase::Adjusting) {
            selector->BeginAdjustment(PointFromMessage(lParam));
            return 0;
        }
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
        if (selector->phase_ == Phase::Adjusting) {
            selector->EndPointerInteraction(PointFromMessage(lParam));
            return 0;
        }
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
            selector->FinishInitialSelection(
                IntRect{monitor.left, monitor.top, monitor.right, monitor.bottom});
            return 0;
        }

        const IntRect selected = selector->ToScreenRect(selector->selection_);
        if (selected.IsValid(kMinimumRegionSize)) {
            selector->FinishInitialSelection(selected);
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
            selector->activeAdjustment_ = AdjustmentHit::None;
            selector->pressedControl_ = view::ControlButton::None;
            if (selector->phase_ == Phase::Selecting) {
                selector->hasDragged_ = false;
                selector->selection_ = RECT{};
            }
            selector->InvalidateFrame();
        }
        return 0;

    case WM_KEYDOWN:
        if (wParam == VK_ESCAPE) {
            selector->Cancel();
            return 0;
        }
        if (wParam == VK_RETURN && selector->phase_ == Phase::Adjusting &&
            selector->IsSelectionValid()) {
            selector->Complete(selector->ToScreenRect(selector->selection_));
            return 0;
        }
        break;

    case WM_RBUTTONDOWN:
        selector->Cancel();
        return 0;

    case WM_DISPLAYCHANGE:
        {
        selector->mouseDown_ = false;
        if (GetCapture() == window) {
            ReleaseCapture();
        }
        selector->activeAdjustment_ = AdjustmentHit::None;
        selector->pressedControl_ = view::ControlButton::None;
        const IntRect previousScreenSelection = selector->ToScreenRect(selector->selection_);
        selector->RefreshMonitorAreas();
        selector->selection_ = selector->ToClientRect(RECT{
            previousScreenSelection.left,
            previousScreenSelection.top,
            previousScreenSelection.right,
            previousScreenSelection.bottom});
        selector->selection_.left = std::clamp<LONG>(
            selector->selection_.left,
            0,
            selector->virtualDesktop_.right - selector->virtualDesktop_.left);
        selector->selection_.right = std::clamp<LONG>(
            selector->selection_.right,
            0,
            selector->virtualDesktop_.right - selector->virtualDesktop_.left);
        selector->selection_.top = std::clamp<LONG>(
            selector->selection_.top,
            0,
            selector->virtualDesktop_.bottom - selector->virtualDesktop_.top);
        selector->selection_.bottom = std::clamp<LONG>(
            selector->selection_.bottom,
            0,
            selector->virtualDesktop_.bottom - selector->virtualDesktop_.top);
        selector->selection_ = selector->MakeAdjustedSelectionEven(selector->selection_);
        if (!selector->ToScreenRect(selector->selection_).IsValid(kMinimumRegionSize)) {
            selector->selection_ = RECT{};
            selector->phase_ = Phase::Selecting;
        }
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
        }

    case WM_SETCURSOR:
        SetCursor(
            selector->phase_ == Phase::Adjusting
                ? selector->CursorForPoint(selector->pointer_)
                : LoadCursorW(nullptr, IDC_CROSS));
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
