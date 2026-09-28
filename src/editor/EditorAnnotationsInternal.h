#pragma once
// Private shared declarations for annotation view and input translation units.
#include "editor/EditorAnnotations.h"
#include <optional>

namespace qrec {
class EditorAnnotations::Impl final {
public:
    ~Impl();
    bool Create(HWND,HINSTANCE,EditorChrome*,annotations::Size,annotations::Time,EditorAnnotationCallbacks);
    void Destroy() noexcept;
    void Layout(const EditorChromeLayout&);
    void SetPosition(annotations::Time,bool);
    void SetEnabled(bool);
    void CommitText(bool cancel=false);
    void CommitProperty(bool cancel=false);
    bool HandleKey(const MSG&);
    annotations::Snapshot Snapshot() const noexcept { return document_.Current(); }

private:
    static LRESULT CALLBACK WindowProc(HWND,UINT,WPARAM,LPARAM);
    static LRESULT CALLBACK TextProc(HWND,UINT,WPARAM,LPARAM,UINT_PTR,DWORD_PTR);
    LRESULT Message(HWND,UINT,WPARAM,LPARAM);
    void RequestPaint();
    void Paint();
    bool EnsureBitmap(int,int);
    void ReleaseBitmap() noexcept;
    void RefreshControls();
    void Changed();
    void SelectTool(AnnotationAction);
    void SelectMark(std::uint64_t);
    void ChangeRange(std::uint64_t,annotations::Time,annotations::Time,AnnotationEditPhase);
    void ChangeColor(std::uint32_t);
    void ChangeWidth(float,AnnotationEditPhase);
    void BeginProperty();
    void PointerDown(POINT);
    void PointerMove(POINT);
    void PointerUp(POINT);
    void CancelGesture();
    void BeginText(annotations::Point,const annotations::Mark* existing=nullptr);
    void LayoutText();
    void UpdateTextDraft();
    void DrawTextInputGuide();
    annotations::Mark DisplayTextDraft() const;
    annotations::Point SourcePoint(POINT) const;
    const annotations::Mark* Pick(annotations::Point) const noexcept;

    HWND parent_{}, overlay_{}, text_{};
    HINSTANCE instance_{};
    EditorChrome* chrome_{};
    HFONT textFont_{};
    RECT textBounds_{};
    bool caretVisible_{true};
    std::wstring compositionText_,compositionBase_;
    DWORD compositionStart_{},compositionLength_{};
    HDC surfaceDc_{};
    HBITMAP bitmap_{};
    HGDIOBJ previousBitmap_{};
    std::uint8_t* pixels_{};
    int bitmapWidth_{},bitmapHeight_{};
    RECT previewBounds_{};
    AnnotationControls controls_;
    EditorAnnotationCallbacks callbacks_;
    annotations::Document document_;
    annotations::Tool tool_{annotations::Tool::Select};
    annotations::Time time_{};
    std::optional<annotations::Mark> draft_;
    std::optional<annotations::Mark> moveOriginal_;
    std::optional<annotations::Mark> propertyOriginal_;
    annotations::Point anchor_{};
    std::uint64_t selected_{};
    std::uint32_t color_{0xFFFF595E};
    float width_{5},propertyWidthBefore_{5};
    bool propertyEditing_{};
    bool enabled_{true},playing_{},gesture_{},paintPending_{},destroying_{},composing_{},committingText_{};
};
}  // namespace qrec
