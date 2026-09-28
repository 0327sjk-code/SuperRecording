#pragma once
// Coordinates annotation interaction, transparent preview overlay and export snapshots.
#include "annotations/Annotation.h"
#include "editor/AnnotationControls.h"
#include <functional>
#include <memory>

namespace qrec {
struct EditorAnnotationCallbacks final {
    std::function<annotations::Time()> pauseAndPosition;
    std::function<void()> changed;
    std::function<void(const std::wstring&)> error;
};
class EditorAnnotations final {
public:
    EditorAnnotations();
    ~EditorAnnotations();
    bool Create(HWND parent,HINSTANCE instance,EditorChrome* chrome,
        annotations::Size canvas,annotations::Time duration,EditorAnnotationCallbacks callbacks);
    void Destroy() noexcept;
    void Layout(const EditorChromeLayout& layout);
    void SetPosition(annotations::Time time,bool playing=false);
    void SetEnabled(bool enabled);
    void CommitText();
    [[nodiscard]] bool HandleKey(const MSG& message);
    [[nodiscard]] annotations::Snapshot Snapshot() const noexcept;
private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};
}  // namespace qrec
