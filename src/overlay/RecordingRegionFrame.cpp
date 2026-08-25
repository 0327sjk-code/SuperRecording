#include "RecordingRegionFrame.h"

#include "RecordingOverlayPlacement.h"
#include "RecordingRegionGeometry.h"

#include <dwmapi.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>

#include "../ui/Theme.h"

namespace qrec::overlay {
namespace {

constexpr wchar_t kWindowClassName[] = L"SuperRecording.RecordingRegionFrame.Layer";
constexpr wchar_t kEdgeWindowTitle[] = L"SuperRecording - 录制区域边界";
constexpr wchar_t kDimmerWindowTitle[] = L"SuperRecording - 录制区域遮罩";
constexpr BYTE kDimmerOpacity = 174;
constexpr UINT kRefreshLayoutMessage = WM_APP + 0x52;

#ifndef WDA_EXCLUDEFROMCAPTURE
#define WDA_EXCLUDEFROMCAPTURE 0x00000011
#endif

[[nodiscard]] bool IsValidRegion(const IntRect& region) noexcept {
    return region.right > region.left && region.bottom > region.top;
}

[[nodiscard]] bool IsNonEmpty(const RECT& rectangle) noexcept {
    return region_geometry::IsNonEmpty(rectangle);
}

class ScopedFlagReset final {
public:
    explicit ScopedFlagReset(bool& flag) noexcept : flag_(flag) {}
    ~ScopedFlagReset() { flag_ = false; }

    ScopedFlagReset(const ScopedFlagReset&) = delete;
    ScopedFlagReset& operator=(const ScopedFlagReset&) = delete;

private:
    bool& flag_;
};

// A temporary 32-bit surface lets UpdateLayeredWindow hand DWM one complete
// ARGB frame. No partially painted state is ever presented to the user.
class DimmingSurface final {
public:
    DimmingSurface() = default;
    ~DimmingSurface() { Reset(); }

    DimmingSurface(const DimmingSurface&) = delete;
    DimmingSurface& operator=(const DimmingSurface&) = delete;

    [[nodiscard]] bool Create(const HDC referenceDevice, const SIZE size) noexcept {
        Reset();
        if (referenceDevice == nullptr || size.cx <= 0 || size.cy <= 0) {
            return false;
        }
        const std::size_t width = static_cast<std::size_t>(size.cx);
        const std::size_t height = static_cast<std::size_t>(size.cy);
        if (height > std::numeric_limits<std::size_t>::max() / width) {
            return false;
        }

        BITMAPINFO information{};
        information.bmiHeader.biSize = sizeof(information.bmiHeader);
        information.bmiHeader.biWidth = size.cx;
        information.bmiHeader.biHeight = -size.cy;
        information.bmiHeader.biPlanes = 1;
        information.bmiHeader.biBitCount = 32;
        information.bmiHeader.biCompression = BI_RGB;

        void* pixels = nullptr;
        bitmap_ = CreateDIBSection(
            referenceDevice,
            &information,
            DIB_RGB_COLORS,
            &pixels,
            nullptr,
            0);
        device_ = CreateCompatibleDC(referenceDevice);
        if (bitmap_ == nullptr || device_ == nullptr || pixels == nullptr) {
            Reset();
            return false;
        }
        previousBitmap_ = SelectObject(device_, bitmap_);
        if (previousBitmap_ == nullptr || previousBitmap_ == HGDI_ERROR) {
            previousBitmap_ = nullptr;
            Reset();
            return false;
        }

        constexpr DWORD maskColor = static_cast<DWORD>(theme::OverlayMask);
        const DWORD red = maskColor & 0xFFU;
        const DWORD green = (maskColor >> 8U) & 0xFFU;
        const DWORD blue = (maskColor >> 16U) & 0xFFU;
        const DWORD pixel =
            0xFF000000U |
            (red << 16U) |
            (green << 8U) |
            blue;
        std::fill_n(static_cast<DWORD*>(pixels), width * height, pixel);
        return true;
    }

    [[nodiscard]] HDC Device() const noexcept { return device_; }

private:
    void Reset() noexcept {
        if (device_ != nullptr && previousBitmap_ != nullptr) {
            static_cast<void>(SelectObject(device_, previousBitmap_));
        }
        previousBitmap_ = nullptr;
        if (bitmap_ != nullptr) {
            static_cast<void>(DeleteObject(bitmap_));
            bitmap_ = nullptr;
        }
        if (device_ != nullptr) {
            static_cast<void>(DeleteDC(device_));
            device_ = nullptr;
        }
    }

    HDC device_{};
    HBITMAP bitmap_{};
    HGDIOBJ previousBitmap_{};
};

}  // namespace

RecordingRegionFrame::~RecordingRegionFrame() {
    Destroy();
}

bool RecordingRegionFrame::Create(
    const HWND owner,
    const IntRect& recordingRegion) {
    Destroy();
    if (!IsValidRegion(recordingRegion)) {
        return false;
    }

    instance_ = GetModuleHandleW(nullptr);
    owner_ = owner;
    recordingRegion_ = recordingRegion;
    brush_ = CreateSolidBrush(theme::Danger);
    if (instance_ == nullptr || brush_ == nullptr || !RegisterWindowClass() ||
        !CreateLayerWindows(owner_) || !ApplyLayout()) {
        Destroy();
        return false;
    }
    return true;
}

void RecordingRegionFrame::Destroy() noexcept {
    const auto edgeWindows = windows_;
    windows_.fill(nullptr);
    for (const HWND window : edgeWindows) {
        if (window != nullptr && IsWindow(window)) {
            static_cast<void>(DestroyWindow(window));
        }
    }

    const auto dimmerWindows = dimmerWindows_;
    dimmerWindows_.fill(nullptr);
    for (const HWND window : dimmerWindows) {
        if (window != nullptr && IsWindow(window)) {
            static_cast<void>(DestroyWindow(window));
        }
    }

    if (brush_ != nullptr) {
        static_cast<void>(DeleteObject(brush_));
        brush_ = nullptr;
    }
    recordingRegion_ = IntRect{};
    captureExcluded_ = false;
    applyingLayout_ = false;
    layoutRefreshPending_ = false;
    owner_ = nullptr;
    instance_ = nullptr;
}

bool RecordingRegionFrame::SetRegion(const IntRect& recordingRegion) {
    if (!IsCreated() || !IsValidRegion(recordingRegion)) {
        return false;
    }
    const IntRect previousRegion = recordingRegion_;
    recordingRegion_ = recordingRegion;
    if (ApplyLayout()) {
        return true;
    }
    recordingRegion_ = previousRegion;
    static_cast<void>(ApplyLayout());
    return false;
}

bool RecordingRegionFrame::IsCreated() const noexcept {
    const auto isValidWindow = [](const HWND window) noexcept {
        return window != nullptr && IsWindow(window) != FALSE;
    };
    return std::ranges::all_of(windows_, isValidWindow) &&
           std::ranges::all_of(dimmerWindows_, isValidWindow);
}

bool RecordingRegionFrame::RegisterWindowClass() {
    WNDCLASSEXW windowClass{};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.lpfnWndProc = &RecordingRegionFrame::WindowProcedure;
    windowClass.hInstance = instance_;
    windowClass.lpszClassName = kWindowClassName;
    if (RegisterClassExW(&windowClass) != 0) {
        return true;
    }
    return GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
}

bool RecordingRegionFrame::ConfigureCaptureExclusion(const HWND window) {
    if (!SetWindowDisplayAffinity(window, WDA_EXCLUDEFROMCAPTURE)) {
        return false;
    }
    DWORD affinity = WDA_NONE;
    return GetWindowDisplayAffinity(window, &affinity) != FALSE &&
           affinity == WDA_EXCLUDEFROMCAPTURE;
}

bool RecordingRegionFrame::CreateLayerWindows(const HWND owner) {
    constexpr DWORD extendedStyle =
        WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE |
        WS_EX_LAYERED | WS_EX_TRANSPARENT;

    const auto createWindow = [&](HWND& window, const wchar_t* title) {
        window = CreateWindowExW(
            extendedStyle,
            kWindowClassName,
            title,
            WS_POPUP,
            0,
            0,
            1,
            1,
            owner,
            nullptr,
            instance_,
            this);
        if (window == nullptr || !ConfigureCaptureExclusion(window)) {
            return false;
        }
        const BOOL disableTransitions = TRUE;
        static_cast<void>(DwmSetWindowAttribute(
            window,
            DWMWA_TRANSITIONS_FORCEDISABLED,
            &disableTransitions,
            sizeof(disableTransitions)));
        return true;
    };

    for (HWND& window : windows_) {
        if (!createWindow(window, kEdgeWindowTitle) ||
            !SetLayeredWindowAttributes(window, 0, 255, LWA_ALPHA)) {
            return false;
        }
    }
    for (HWND& window : dimmerWindows_) {
        if (!createWindow(window, kDimmerWindowTitle)) {
            return false;
        }
    }
    captureExcluded_ = true;
    return true;
}

bool RecordingRegionFrame::ComposeDimmer(
    const HWND window,
    const RECT& bounds) const {
    if (!IsNonEmpty(bounds)) {
        return true;
    }
    SIZE size{bounds.right - bounds.left, bounds.bottom - bounds.top};
    POINT destination{bounds.left, bounds.top};
    POINT source{};
    BLENDFUNCTION blend{};
    blend.BlendOp = AC_SRC_OVER;
    blend.SourceConstantAlpha = kDimmerOpacity;
    blend.AlphaFormat = AC_SRC_ALPHA;

    const HDC screenDevice = GetDC(nullptr);
    if (screenDevice == nullptr) {
        return false;
    }
    DimmingSurface surface;
    const bool surfaceReady = surface.Create(screenDevice, size);
    const bool updated = surfaceReady &&
        UpdateLayeredWindow(
            window,
            screenDevice,
            &destination,
            &size,
            surface.Device(),
            &source,
            0,
            &blend,
            ULW_ALPHA) != FALSE;
    static_cast<void>(ReleaseDC(nullptr, screenDevice));
    return updated;
}

bool RecordingRegionFrame::CommitLayerPositions(
    const std::array<RECT, LayerCount>& dimmerBounds,
    const std::array<RECT, LayerCount>& edgeBounds) {
    const auto deferPosition = [](
        const HDWP transaction,
        const HWND window,
        const RECT& bounds,
        const bool show) -> HDWP {
        const RECT effectiveBounds = IsNonEmpty(bounds)
            ? bounds
            : RECT{bounds.left, bounds.top, bounds.left + 1, bounds.top + 1};
        return DeferWindowPos(
            transaction,
            window,
            HWND_TOPMOST,
            effectiveBounds.left,
            effectiveBounds.top,
            effectiveBounds.right - effectiveBounds.left,
            effectiveBounds.bottom - effectiveBounds.top,
            SWP_NOACTIVATE | SWP_NOOWNERZORDER | SWP_NOCOPYBITS |
                SWP_NOSENDCHANGING | (show ? SWP_SHOWWINDOW : SWP_HIDEWINDOW));
    };

    HDWP transaction = BeginDeferWindowPos(static_cast<int>(LayerCount * 2));
    if (transaction != nullptr) {
        for (std::size_t index = 0;
             index < LayerCount && transaction != nullptr;
             ++index) {
            transaction = deferPosition(
                transaction,
                dimmerWindows_[index],
                dimmerBounds[index],
                IsNonEmpty(dimmerBounds[index]));
        }
        for (std::size_t index = 0;
             index < LayerCount && transaction != nullptr;
             ++index) {
            transaction = deferPosition(
                transaction,
                windows_[index],
                edgeBounds[index],
                IsNonEmpty(edgeBounds[index]));
        }
        if (transaction != nullptr && EndDeferWindowPos(transaction)) {
            return true;
        }
    }

    bool succeeded = true;
    const auto setPosition = [&succeeded](
        const HWND window,
        const RECT& bounds,
        const bool show) {
        const RECT effectiveBounds = IsNonEmpty(bounds)
            ? bounds
            : RECT{bounds.left, bounds.top, bounds.left + 1, bounds.top + 1};
        succeeded = SetWindowPos(
            window,
            HWND_TOPMOST,
            effectiveBounds.left,
            effectiveBounds.top,
            effectiveBounds.right - effectiveBounds.left,
            effectiveBounds.bottom - effectiveBounds.top,
            SWP_NOACTIVATE | SWP_NOOWNERZORDER | SWP_NOCOPYBITS |
                SWP_NOSENDCHANGING | (show ? SWP_SHOWWINDOW : SWP_HIDEWINDOW)) != FALSE &&
            succeeded;
    };
    for (std::size_t index = 0; index < LayerCount; ++index) {
        setPosition(
            dimmerWindows_[index],
            dimmerBounds[index],
            IsNonEmpty(dimmerBounds[index]));
    }
    for (std::size_t index = 0; index < LayerCount; ++index) {
        setPosition(windows_[index], edgeBounds[index], IsNonEmpty(edgeBounds[index]));
    }
    return succeeded;
}

bool RecordingRegionFrame::IsDimmerWindow(const HWND window) const noexcept {
    return std::ranges::find(dimmerWindows_, window) != dimmerWindows_.end();
}

bool RecordingRegionFrame::ApplyLayout() {
    if (!IsCreated() || applyingLayout_) {
        return IsCreated();
    }

    applyingLayout_ = true;
    const ScopedFlagReset applyingGuard(applyingLayout_);

    std::array<RECT, LayerCount> dimmerBounds{};
    const RECT virtualDesktop = region_geometry::VirtualDesktopBounds();
    if (!region_geometry::BuildDimmingBounds(
            virtualDesktop,
            recordingRegion_,
            dimmerBounds)) {
        return false;
    }

    const int width = recordingRegion_.Width();
    const int height = recordingRegion_.Height();
    const POINT center{
        recordingRegion_.left + std::max(0, width - 1) / 2,
        recordingRegion_.top + std::max(0, height - 1) / 2};
    const UINT dpi = placement::DpiAt(center);
    const auto edgeBounds = region_geometry::BuildEdgeBounds(recordingRegion_, dpi);

    for (std::size_t index = 0; index < LayerCount; ++index) {
        if (!ComposeDimmer(dimmerWindows_[index], dimmerBounds[index])) {
            return false;
        }
    }
    if (!CommitLayerPositions(dimmerBounds, edgeBounds)) {
        return false;
    }
    for (const HWND window : windows_) {
        RedrawWindow(
            window,
            nullptr,
            nullptr,
            RDW_INVALIDATE | RDW_UPDATENOW | RDW_NOERASE);
    }
    return true;
}

void RecordingRegionFrame::Paint(const HWND window) const {
    PAINTSTRUCT paint{};
    const HDC device = BeginPaint(window, &paint);
    if (device != nullptr && !IsDimmerWindow(window)) {
        RECT client{};
        GetClientRect(window, &client);
        if (brush_ != nullptr) {
            FillRect(device, &client, brush_);
        }
    }
    EndPaint(window, &paint);
}

void RecordingRegionFrame::RefreshLayoutForDisplayChange() {
    if (!layoutRefreshPending_ && IsCreated()) {
        layoutRefreshPending_ = true;
        if (!PostMessageW(windows_.front(), kRefreshLayoutMessage, 0, 0)) {
            layoutRefreshPending_ = false;
            static_cast<void>(ApplyLayout());
        }
    }
}

LRESULT CALLBACK RecordingRegionFrame::WindowProcedure(
    const HWND window,
    const UINT message,
    const WPARAM wParam,
    const LPARAM lParam) {
    auto* frame = reinterpret_cast<RecordingRegionFrame*>(
        GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
        frame = static_cast<RecordingRegionFrame*>(create->lpCreateParams);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(frame));
    }
    if (frame == nullptr) {
        return DefWindowProcW(window, message, wParam, lParam);
    }

    switch (message) {
    case WM_ERASEBKGND:
        return 1;

    case WM_PAINT:
        frame->Paint(window);
        return 0;

    case WM_NCHITTEST:
        return HTTRANSPARENT;

    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;

    case WM_DPICHANGED:
    case WM_DISPLAYCHANGE:
        frame->RefreshLayoutForDisplayChange();
        return 0;

    case kRefreshLayoutMessage:
        frame->layoutRefreshPending_ = false;
        static_cast<void>(frame->ApplyLayout());
        return 0;

    case WM_NCDESTROY:
        SetWindowLongPtrW(window, GWLP_USERDATA, 0);
        for (HWND& edgeWindow : frame->windows_) {
            if (edgeWindow == window) {
                edgeWindow = nullptr;
                break;
            }
        }
        for (HWND& dimmerWindow : frame->dimmerWindows_) {
            if (dimmerWindow == window) {
                dimmerWindow = nullptr;
                break;
            }
        }
        return DefWindowProcW(window, message, wParam, lParam);

    default:
        break;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

}  // namespace qrec::overlay
