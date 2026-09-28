#include "editor/EditorAnnotationsInternal.h"
#include <commctrl.h>
#include <algorithm>
#include <cmath>
#include <string>

namespace qrec {
namespace {
using namespace annotations;
bool IsEdit(HWND window) {
    wchar_t name[32]{};
    return window && GetClassNameW(window,name,32) && _wcsicmp(name,L"Edit")==0;
}
}

annotations::Point EditorAnnotations::Impl::SourcePoint(POINT point) const {
    const auto size=document_.Current()->Canvas();
    return {std::clamp(static_cast<float>(point.x)/static_cast<float>(std::max<LONG>(1,previewBounds_.right-previewBounds_.left)),0.0F,1.0F)*size.width,
        std::clamp(static_cast<float>(point.y)/static_cast<float>(std::max<LONG>(1,previewBounds_.bottom-previewBounds_.top)),0.0F,1.0F)*size.height};
}
const annotations::Mark* EditorAnnotations::Impl::Pick(annotations::Point point) const noexcept {
    const auto scene=document_.Current();
    if (!scene) return nullptr;
    const float tolerance=scene->Canvas().width/static_cast<float>(std::max<LONG>(1,previewBounds_.right-previewBounds_.left))*7.0F;
    for (auto it=scene->Marks().rbegin();it!=scene->Marks().rend();++it)
        if (Visible(*it,time_) && HitTest(*it,point,tolerance)) return &*it;
    return nullptr;
}
void EditorAnnotations::Impl::SelectTool(AnnotationAction action) {
    if (!enabled_) return;
    CommitProperty(); CommitText(); CancelGesture();
    if (action<=AnnotationAction::Text) {
        tool_=static_cast<Tool>(action); selected_=0;
        if (callbacks_.pauseAndPosition) time_=callbacks_.pauseAndPosition();
        playing_=false; RefreshControls(); RequestPaint(); return;
    }
    bool changed=false;
    if (action==AnnotationAction::Undo) changed=document_.Undo();
    else if (action==AnnotationAction::Redo) changed=document_.Redo();
    else if (action==AnnotationAction::Delete) changed=document_.Erase(selected_);
    if (changed) Changed();
}
void EditorAnnotations::Impl::BeginProperty() {
    if (propertyEditing_) return;
    CommitText(); CancelGesture();
    if (callbacks_.pauseAndPosition) time_=callbacks_.pauseAndPosition();
    playing_=false; propertyEditing_=true; propertyWidthBefore_=width_;
    if (const auto* existing=document_.Find(selected_)) { propertyOriginal_=*existing; draft_=*existing; }
}
void EditorAnnotations::Impl::CommitProperty(bool cancel) {
    if (!propertyEditing_) return;
    propertyEditing_=false;
    bool changed=false;
    if (!cancel && draft_) changed=document_.Put(*draft_);
    if (cancel) width_=propertyWidthBefore_;
    propertyOriginal_.reset(); draft_.reset();
    if (changed) Changed(); else { RefreshControls(); RequestPaint(); }
}
void EditorAnnotations::Impl::SelectMark(std::uint64_t id) {
    if (!enabled_) return;
    CommitProperty(); CommitText(); CancelGesture();
    if (callbacks_.pauseAndPosition) time_=callbacks_.pauseAndPosition();
    playing_=false; selected_=document_.Find(id) ? id : 0;
    RefreshControls(); RequestPaint();
}
void EditorAnnotations::Impl::ChangeRange(std::uint64_t id,annotations::Time start,annotations::Time end,AnnotationEditPhase phase) {
    if (phase!=AnnotationEditPhase::Preview) { CommitProperty(phase==AnnotationEditPhase::Cancel); return; }
    if (!enabled_ || !document_.Find(id)) return;
    if (selected_!=id) SelectMark(id);
    BeginProperty(); if (!draft_) return;
    draft_->start=std::clamp(start,Time{},document_.Duration()-Time{1});
    draft_->end=std::clamp(end,draft_->start+Time{1},document_.Duration());
    RefreshControls(); RequestPaint();
}
void EditorAnnotations::Impl::ChangeColor(std::uint32_t color) {
    CommitProperty(); CommitText(); color_=color;
    if (const auto* existing=document_.Find(selected_)) {
        auto mark=*existing; mark.argb=color;
        if (document_.Put(std::move(mark))) { Changed(); return; }
    }
    RefreshControls();
}
void EditorAnnotations::Impl::ChangeWidth(float width,AnnotationEditPhase phase) {
    if (phase!=AnnotationEditPhase::Preview) { CommitProperty(phase==AnnotationEditPhase::Cancel); return; }
    if (!enabled_) return;
    BeginProperty(); width_=std::clamp(width,0.5F,64.0F);
    if (draft_ && propertyOriginal_) {
        draft_->strokeWidth=width_;
        draft_->fontSize=std::clamp(propertyOriginal_->fontSize*width_/propertyOriginal_->strokeWidth,8.0F,512.0F);
    }
    RefreshControls(); RequestPaint();
}
void EditorAnnotations::Impl::PointerDown(POINT point) {
    if (!enabled_) return;
    const bool hadText=text_!=nullptr;
    CommitProperty(); CommitText(); CancelGesture(); SetFocus(overlay_);
    if (callbacks_.pauseAndPosition) time_=callbacks_.pauseAndPosition();
    playing_=false; anchor_=SourcePoint(point);
    const auto* picked=Pick(anchor_);
    if (picked) { selected_=picked->id; draft_=*picked; moveOriginal_=*picked; }
    else if (hadText) { RefreshControls(); RequestPaint(); return; }
    else if (tool_==Tool::Text) { BeginText(anchor_); return; }
    else if (tool_!=Tool::Select) {
        selected_=0; draft_=document_.NewMark(tool_,time_);
        draft_->argb=color_; draft_->strokeWidth=width_;
        draft_->points.push_back(anchor_);
        if (tool_==Tool::Circle || tool_==Tool::Arrow) draft_->points.push_back(anchor_);
    } else selected_=0;
    if (draft_) { gesture_=true; SetCapture(overlay_); }
    RefreshControls(); RequestPaint();
}
void EditorAnnotations::Impl::PointerMove(POINT point) {
    if (!gesture_ || !draft_) return;
    const auto cursor=SourcePoint(point);
    if (moveOriginal_) {
        draft_=*moveOriginal_;
        const auto size=document_.Current()->Canvas();
        const auto bounds=MeasureBounds(*moveOriginal_);
        const float dx=cursor.x-anchor_.x,dy=cursor.y-anchor_.y;
        // A click selects without nudging an edge-clipped mark into the frame.
        if (std::hypot(dx,dy)<0.01F) return;
        // Never snap a partially clipped or oversized mark when a drag starts.
        // Preserve its existing overflow while keeping a visible part within reach.
        const float visibleEdge=std::max(1.0F,moveOriginal_->strokeWidth);
        const float minX=std::min(0.0F,-bounds.right+visibleEdge),maxX=std::max(0.0F,size.width-bounds.left-visibleEdge);
        const float minY=std::min(0.0F,-bounds.bottom+visibleEdge),maxY=std::max(0.0F,size.height-bounds.top-visibleEdge);
        Translate(*draft_,{std::clamp(dx,minX,maxX),std::clamp(dy,minY,maxY)});
    } else if (draft_->tool==Tool::Pen) {
        const auto previous=draft_->points.back();
        const float threshold=document_.Current()->Canvas().width/
            static_cast<float>(std::max<LONG>(1,previewBounds_.right-previewBounds_.left))*0.6F;
        if (std::hypot(cursor.x-previous.x,cursor.y-previous.y)<threshold) return;
        if (draft_->points.size()>=MaximumPoints) {
            std::size_t write=1;
            for (std::size_t i=2;i<draft_->points.size();i+=2) draft_->points[write++]=draft_->points[i];
            draft_->points.resize(write);
        }
        draft_->points.push_back(cursor);
    } else draft_->points.back()=cursor;
    RequestPaint();
}
void EditorAnnotations::Impl::PointerUp(POINT point) {
    if (!gesture_ || !draft_) return;
    PointerMove(point); gesture_=false; ReleaseCapture();
    const std::uint64_t id=draft_->id;
    bool valid=true;
    if (draft_->tool==Tool::Circle || draft_->tool==Tool::Arrow) {
        const auto a=draft_->points.front(),b=draft_->points.back();
        valid=std::hypot(a.x-b.x,a.y-b.y)>draft_->strokeWidth;
    }
    const bool committed=valid && document_.Put(std::move(*draft_));
    draft_.reset(); moveOriginal_.reset();
    if (committed) { selected_=id!=0 ? id : document_.LastId(); Changed(); }
    else { RefreshControls(); RequestPaint(); }
}
void EditorAnnotations::Impl::CancelGesture() {
    if (!gesture_) return;
    gesture_=false; if (GetCapture()==overlay_) ReleaseCapture();
    draft_.reset(); moveOriginal_.reset(); RequestPaint();
}


bool EditorAnnotations::Impl::HandleKey(const MSG& message) {
    if (!enabled_ || !overlay_ || message.message!=WM_KEYDOWN ||
        (message.hwnd!=parent_ && !IsChild(parent_,message.hwnd))) return false;
    if (IsEdit(message.hwnd) || IsEdit(GetFocus())) return false;
    wchar_t focusedClass[64]{};
    if (GetClassNameW(GetFocus(),focusedClass,64) &&
        (wcscmp(focusedClass,L"SuperRecording.AnnotationSlider")==0 || wcscmp(focusedClass,L"SuperRecording.AnnotationTimeline")==0) &&
        (message.wParam==VK_ESCAPE || message.wParam==VK_LEFT || message.wParam==VK_RIGHT ||
         message.wParam==VK_UP || message.wParam==VK_DOWN || message.wParam==VK_HOME || message.wParam==VK_END)) {
        SendMessageW(GetFocus(),message.message,message.wParam,message.lParam); return true;
    }
    const bool control=(GetKeyState(VK_CONTROL)&0x8000)!=0;
    if (control && (message.wParam=='Z' || message.wParam=='Y')) {
        SelectTool(message.wParam=='Y' || (GetKeyState(VK_SHIFT)&0x8000) ? AnnotationAction::Redo : AnnotationAction::Undo); return true;
    }
    if (message.wParam==VK_DELETE && selected_!=0) { SelectTool(AnnotationAction::Delete); return true; }
    if (message.wParam==VK_ESCAPE && (gesture_ || tool_!=Tool::Select || selected_!=0)) {
        CancelGesture(); tool_=Tool::Select; selected_=0; RefreshControls(); RequestPaint(); return true;
    }
    if (control || (GetKeyState(VK_MENU)&0x8000)) return false;
    switch (message.wParam) {
    case 'V': SelectTool(AnnotationAction::Select); return true;
    case 'P': SelectTool(AnnotationAction::Pen); return true;
    case 'O': SelectTool(AnnotationAction::Circle); return true;
    case 'A': SelectTool(AnnotationAction::Arrow); return true;
    case 'T': SelectTool(AnnotationAction::Text); return true;
    default: return false;
    }
}
}  // namespace qrec
