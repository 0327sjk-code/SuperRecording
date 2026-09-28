#pragma once
// A single clip strip for the selected annotation: trim edges or move the entire range.
#include "annotations/Annotation.h"
#include "editor/AnnotationSlider.h"
#include <functional>
#include <optional>

namespace qrec {
struct AnnotationTimelineCallbacks final {
    std::function<void(std::uint64_t)> select;
    std::function<void(std::uint64_t,annotations::Time,annotations::Time,AnnotationEditPhase)> change;
};
class AnnotationTimeline final {
public:
    static constexpr int HeightDip=38;
    ~AnnotationTimeline();
    bool Create(HWND,HINSTANCE,int,AnnotationTimelineCallbacks);
    void Destroy() noexcept;
    void Layout(const RECT&,HFONT);
    void SetState(annotations::Snapshot,const annotations::Mark*,annotations::Time,bool);
    void SetPosition(annotations::Time);
private:
    enum class Part { None, Start, End, Body };
    struct Hit { std::uint64_t id{}; Part part{Part::None}; };
    static LRESULT CALLBACK WindowProc(HWND,UINT,WPARAM,LPARAM);
    LRESULT Message(UINT,WPARAM,LPARAM);
    int Dip(int) const;
    int TrackWidth() const;
    int ToX(annotations::Time) const;
    RECT Bounds() const;
    const annotations::Mark* Find(std::uint64_t) const;
    Hit HitTest(POINT) const;
    void Begin(POINT);
    void Move(int);
    void Finish(bool cancel=false);
    void Paint(HDC);
    void Nudge(WPARAM);
    HWND window_{};
    HFONT font_{};
    AnnotationTimelineCallbacks callbacks_;
    annotations::Snapshot scene_;
    std::optional<annotations::Mark> draft_;
    annotations::Time duration_{1},position_{},originalStart_{},originalEnd_{};
    std::optional<annotations::Time> snap_;
    std::uint64_t selected_{},dragId_{},hoverId_{};
    int downX_{};
    Part dragPart_{Part::None},hoverPart_{Part::None};
    bool dragMoved_{};
};
}  // namespace qrec
