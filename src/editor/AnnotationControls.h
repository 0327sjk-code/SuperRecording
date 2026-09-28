#pragma once
// Left tool/style rail and a clip-based annotation timeline.
#include "annotations/Annotation.h"
#include "editor/AnnotationSlider.h"
#include "editor/AnnotationTimeline.h"
#include "editor/EditorChrome.h"
#include <array>
#include <functional>

namespace qrec {
enum class AnnotationAction { Select, Pen, Circle, Arrow, Text, Undo, Redo, Delete };
struct AnnotationControlCallbacks final {
    std::function<void(AnnotationAction)> action;
    std::function<void(std::uint64_t)> select;
    std::function<void(std::uint64_t,annotations::Time,annotations::Time,AnnotationEditPhase)> rangeChanged;
    std::function<void(std::uint32_t)> colorChanged;
    std::function<void(float,AnnotationEditPhase)> widthChanged;
};
class AnnotationControls final {
public:
    ~AnnotationControls();
    bool Create(HWND parent,HINSTANCE instance,EditorChrome* chrome,AnnotationControlCallbacks callbacks);
    void Layout(const EditorChromeLayout& layout);
    void SetPosition(annotations::Time time) { timeline_.SetPosition(time); }
    void Refresh(annotations::Tool tool,const annotations::Document& document,
        const annotations::Mark* selected,bool enabled,std::uint32_t color,float width);
    void Destroy() noexcept;
private:
    static LRESULT CALLBACK WindowProc(HWND,UINT,WPARAM,LPARAM);
    LRESULT Message(HWND,UINT,WPARAM,LPARAM);
    void SelectColor();
    HWND rail_{}, color_{}, colorLabel_{}, tooltip_{};
    std::array<HWND,8> buttons_{};
    AnnotationSlider width_;
    AnnotationTimeline timeline_;
    EditorChrome* chrome_{};
    AnnotationControlCallbacks callbacks_;
    annotations::Tool tool_{annotations::Tool::Select};
    std::uint32_t colorArgb_{0xFFFF595E};
    bool enabled_{true};
};
}  // namespace qrec
