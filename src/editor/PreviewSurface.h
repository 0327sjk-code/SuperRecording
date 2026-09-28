#pragma once
// Video-owned paint surface: never let a static control erase a paused EVR frame.
#include <windows.h>

namespace qrec {
class MediaPreview;
class PreviewSurface final {
public:
    PreviewSurface() = default;
    ~PreviewSurface();
    PreviewSurface(const PreviewSurface&) = delete;
    PreviewSurface& operator=(const PreviewSurface&) = delete;
    HWND Create(HWND parent,HINSTANCE instance,int id,MediaPreview* preview);
private:
    static LRESULT CALLBACK WindowProc(HWND,UINT,WPARAM,LPARAM);
    HWND window_{};
    MediaPreview* preview_{};
};
}  // namespace qrec
