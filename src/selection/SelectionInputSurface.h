#pragma once
// Almost-transparent input surface over the color-key hole of the selector.
#include <windows.h>

namespace qrec::selection {
class SelectionInputSurface final {
public:
    SelectionInputSurface() = default;
    ~SelectionInputSurface();
    SelectionInputSurface(const SelectionInputSurface&) = delete;
    SelectionInputSurface& operator=(const SelectionInputSurface&) = delete;
    bool Create(HWND owner);
    void Reset() noexcept;
    void Update(const RECT& selection,bool visible) noexcept;
private:
    static LRESULT CALLBACK WindowProc(HWND,UINT,WPARAM,LPARAM);
    HWND window_{};
    HWND owner_{};
};
}  // namespace qrec::selection
