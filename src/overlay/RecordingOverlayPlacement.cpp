#include "RecordingOverlayPlacement.h"

#include <algorithm>

namespace qrec::overlay::placement {
namespace {

constexpr int kBaseGap = 8;
constexpr int kBaseMargin = 8;

[[nodiscard]] int ScaleForDpi(int value, UINT dpi) noexcept {
    return MulDiv(value, static_cast<int>(dpi), USER_DEFAULT_SCREEN_DPI);
}

[[nodiscard]] MONITORINFO MonitorInformation(HMONITOR monitor) noexcept {
    MONITORINFO information{};
    information.cbSize = sizeof(information);
    if (!GetMonitorInfoW(monitor, &information)) {
        information.rcMonitor = RECT{
            0,
            0,
            GetSystemMetrics(SM_CXSCREEN),
            GetSystemMetrics(SM_CYSCREEN)};
        information.rcWork = information.rcMonitor;
    }
    return information;
}

[[nodiscard]] RECT ClampToBounds(RECT windowBounds, const RECT& workArea) noexcept {
    const LONG width = windowBounds.right - windowBounds.left;
    const LONG height = windowBounds.bottom - windowBounds.top;
    const LONG availableWidth = workArea.right - workArea.left;
    const LONG availableHeight = workArea.bottom - workArea.top;

    windowBounds.left = availableWidth >= width
        ? std::clamp(windowBounds.left, workArea.left, workArea.right - width)
        : workArea.left;
    windowBounds.top = availableHeight >= height
        ? std::clamp(windowBounds.top, workArea.top, workArea.bottom - height)
        : workArea.top;
    windowBounds.right = windowBounds.left + std::min(width, availableWidth);
    windowBounds.bottom = windowBounds.top + std::min(height, availableHeight);
    return windowBounds;
}

}  // namespace

UINT DpiAt(POINT screenPoint) noexcept {
    const HMONITOR monitor = MonitorFromPoint(screenPoint, MONITOR_DEFAULTTONEAREST);
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

RECT AutomaticBounds(const IntRect& recordingRegion, SIZE windowSize, UINT dpi) noexcept {
    const POINT anchor{recordingRegion.right - 1, recordingRegion.bottom - 1};
    const HMONITOR monitor = MonitorFromPoint(anchor, MONITOR_DEFAULTTONEAREST);
    const RECT work = MonitorInformation(monitor).rcWork;
    const int gap = ScaleForDpi(kBaseGap, dpi);
    const int margin = ScaleForDpi(kBaseMargin, dpi);

    RECT position{
        recordingRegion.right - windowSize.cx,
        recordingRegion.bottom + gap,
        recordingRegion.right,
        recordingRegion.bottom + gap + windowSize.cy};
    const bool belowFits = position.left >= work.left && position.right <= work.right &&
                           position.top >= work.top && position.bottom <= work.bottom;
    if (!belowFits) {
        const RECT inside{
            recordingRegion.right - windowSize.cx - margin,
            recordingRegion.bottom - windowSize.cy - margin,
            recordingRegion.right - margin,
            recordingRegion.bottom - margin};
        const bool regionCanContain = recordingRegion.Width() >= windowSize.cx + margin * 2 &&
                                      recordingRegion.Height() >= windowSize.cy + margin * 2;
        const bool insideFitsWork = inside.left >= work.left && inside.right <= work.right &&
                                    inside.top >= work.top && inside.bottom <= work.bottom;
        if (regionCanContain && insideFitsWork) {
            position = inside;
        } else {
            position = RECT{
                recordingRegion.right - windowSize.cx,
                recordingRegion.top - gap - windowSize.cy,
                recordingRegion.right,
                recordingRegion.top - gap};
        }
    }
    return ClampToBounds(position, work);
}

RECT ClampToWorkArea(RECT windowBounds) noexcept {
    const HMONITOR monitor = MonitorFromRect(&windowBounds, MONITOR_DEFAULTTONEAREST);
    return ClampToBounds(windowBounds, MonitorInformation(monitor).rcWork);
}

}  // namespace qrec::overlay::placement
