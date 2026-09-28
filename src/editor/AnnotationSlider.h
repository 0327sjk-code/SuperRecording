#pragma once
// Reusable double-buffered annotation sliders with transactional drag/wheel edits.
#include <windows.h>
#include <functional>

namespace qrec {
enum class AnnotationEditPhase { Preview, Commit, Cancel };
enum class AnnotationSliderKind { Width, Start, End };
class AnnotationSlider final {
public:
    ~AnnotationSlider();
    bool Create(HWND parent,HINSTANCE instance,int id,AnnotationSliderKind kind,
        std::function<void(double,AnnotationEditPhase)> changed);
    void Destroy() noexcept;
    void Layout(const RECT& rectangle,HFONT font);
    void SetState(double value,double maximum,double start,double end,bool enabled);
    void Finish(bool cancel=false);
    [[nodiscard]] HWND Window() const noexcept { return window_; }
private:
    static LRESULT CALLBACK WindowProc(HWND,UINT,WPARAM,LPARAM);
    LRESULT Message(UINT,WPARAM,LPARAM);
    void Paint(HDC);
    void Preview(double);
    double PositionValue(int x) const;
    int ValuePosition(double value) const;
    int Dip(int value) const;
    int Inset() const;
    HWND window_{};
    HFONT font_{};
    AnnotationSliderKind kind_{};
    std::function<void(double,AnnotationEditPhase)> changed_;
    double value_{}, maximum_{32}, start_{}, end_{}, original_{};
    bool editing_{}, dragging_{}, hovered_{};
};
}  // namespace qrec
