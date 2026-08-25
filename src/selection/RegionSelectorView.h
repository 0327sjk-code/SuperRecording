#pragma once

#include <windows.h>

namespace qrec::selection::view {

inline constexpr COLORREF TransparentColor = RGB(255, 0, 255);
inline constexpr BYTE MaskOpacity = 174;

// Persistent display-compatible back buffer used by the virtual-desktop
// selector. A selection update is composed completely off-screen and copied
// to the layered window in one operation, so the color-key hole and its
// decorations can never be observed in partially-painted states.
class FrameBuffer final {
public:
    FrameBuffer() = default;
    ~FrameBuffer();

    FrameBuffer(const FrameBuffer&) = delete;
    FrameBuffer& operator=(const FrameBuffer&) = delete;

    [[nodiscard]] bool Ensure(HDC referenceDevice, SIZE size) noexcept;
    void Reset() noexcept;

    [[nodiscard]] HDC Device() const noexcept { return device_; }
    [[nodiscard]] SIZE Size() const noexcept { return size_; }

private:
    HDC device_{};
    HBITMAP bitmap_{};
    HGDIOBJ previousBitmap_{};
    SIZE size_{};
};

[[nodiscard]] bool IsEmpty(const RECT& rectangle) noexcept;
void Paint(HWND window, const RECT& selection, UINT dpi, FrameBuffer& frameBuffer);

}  // namespace qrec::selection::view
