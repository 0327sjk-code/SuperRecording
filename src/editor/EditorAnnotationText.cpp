// Transparent inline input: the native EDIT owns text/IME, the shared overlay owns pixels.
#include "editor/EditorAnnotationsInternal.h"
#include <commctrl.h>
#include <imm.h>
#include <algorithm>
#include <cmath>
#include <string>
#pragma comment(lib,"imm32.lib")

namespace qrec {
namespace {
constexpr UINT_PTR kTextSubclass=0xAC02,kCaretTimer=0xAC03;
constexpr UINT kCommitTextMessage=WM_APP+0x351;
constexpr int kTextEdit=8300;
int Dip(HWND window,int value) { return MulDiv(value,static_cast<int>(GetDpiForWindow(window)),96); }
}
void EditorAnnotations::Impl::BeginText(annotations::Point point,const annotations::Mark* existing) {
    const std::optional<annotations::Mark> copy=existing ? std::optional{*existing} : std::nullopt;
    CommitProperty(); CommitText();
    if (callbacks_.pauseAndPosition) time_=callbacks_.pauseAndPosition();
    playing_=false; composing_=false; compositionText_.clear(); caretVisible_=true;
    if (copy) { draft_=*copy; selected_=copy->id; }
    else {
        selected_=0; draft_=document_.NewMark(annotations::Tool::Text,time_); draft_->points.push_back(point);
        draft_->fontSize=std::clamp(draft_->fontSize*width_/draft_->strokeWidth,8.0F,512.0F);
        draft_->argb=color_; draft_->strokeWidth=width_;
    }
    // Native input lives inside the per-pixel-alpha canvas, not in an opaque floating panel.
    // Its default paint is suppressed; the draft and exported text use identical source geometry.
    text_=CreateWindowExW(0,WC_EDITW,draft_->text.c_str(),
        WS_CHILD|WS_VISIBLE|WS_TABSTOP|ES_MULTILINE|ES_AUTOHSCROLL|ES_AUTOVSCROLL|ES_WANTRETURN,
        0,0,1,1,overlay_,reinterpret_cast<HMENU>(static_cast<INT_PTR>(kTextEdit)),instance_,nullptr);
    if (!text_) { draft_.reset(); return; }
    SetWindowSubclass(text_,TextProc,kTextSubclass,reinterpret_cast<DWORD_PTR>(this));
    SendMessageW(text_,EM_SETLIMITTEXT,annotations::MaximumTextLength,0);
    SendMessageW(text_,EM_SETMARGINS,EC_LEFTMARGIN|EC_RIGHTMARGIN,0);
    LayoutText(); SetFocus(text_); SendMessageW(text_,EM_SETSEL,0,-1);
    RefreshControls(); Paint();
}
void EditorAnnotations::Impl::LayoutText() {
    if (!text_ || !draft_) return;
    const auto size=document_.Current()->Canvas();
    const int availableWidth=previewBounds_.right-previewBounds_.left,availableHeight=previewBounds_.bottom-previewBounds_.top;
    const float scale=static_cast<float>(availableWidth)/size.width;
    const int fontHeight=std::max(1,static_cast<int>(std::lround(draft_->fontSize*scale)));
    LOGFONTW previous{};
    if (!textFont_ || GetObjectW(textFont_,sizeof(previous),&previous)==0 || previous.lfHeight!=-fontHeight) {
        HFONT font=CreateFontW(-fontHeight,0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,
            OUT_TT_PRECIS,CLIP_DEFAULT_PRECIS,ANTIALIASED_QUALITY,DEFAULT_PITCH,L"Microsoft YaHei UI");
        if (font) {
            SendMessageW(text_,WM_SETFONT,reinterpret_cast<WPARAM>(font),FALSE);
            if (textFont_) DeleteObject(textFont_); textFont_=font;
        }
    }
    const auto mark=DisplayTextDraft();
    const auto bounds=annotations::MeasureBounds(mark);
    const auto anchor=mark.points.front();
    const int x=std::clamp(static_cast<int>(std::lround(anchor.x*scale)),0,std::max(0,availableWidth-1));
    const int y=std::clamp(static_cast<int>(std::lround(anchor.y*scale)),0,std::max(0,availableHeight-1));
    const int width=std::min(availableWidth-x,std::max(Dip(text_,120),static_cast<int>(std::ceil((bounds.right-anchor.x)*scale))+Dip(text_,10)));
    const int height=std::min(availableHeight-y,std::max(Dip(text_,22),static_cast<int>(std::ceil((bounds.bottom-anchor.y)*scale))+Dip(text_,4)));
    const RECT rectangle{x,y,x+width,y+height};
    if (!EqualRect(&rectangle,&textBounds_)) {
        textBounds_=rectangle;
        SetWindowPos(text_,HWND_TOP,x,y,width,height,SWP_NOACTIVATE);
        RECT formatting{0,0,width,height}; SendMessageW(text_,EM_SETRECTNP,0,reinterpret_cast<LPARAM>(&formatting));
    }
}
annotations::Mark EditorAnnotations::Impl::DisplayTextDraft() const {
    auto result=draft_.value_or(annotations::Mark{});
    if (composing_ && !compositionText_.empty()) {
        result.text=compositionBase_;
        const auto index=std::min<std::size_t>(compositionStart_,result.text.size());
        result.text.replace(index,std::min<std::size_t>(compositionLength_,result.text.size()-index),compositionText_);
    }
    return result;
}
void EditorAnnotations::Impl::UpdateTextDraft() {
    if (!text_ || !draft_ || committingText_) return;
    std::wstring value(static_cast<std::size_t>(GetWindowTextLengthW(text_))+1,L'\0');
    const int copied=GetWindowTextW(text_,value.data(),static_cast<int>(value.size()));
    value.resize(static_cast<std::size_t>(std::max(0,copied)));
    draft_->text=std::move(value); caretVisible_=true; LayoutText(); RequestPaint();
}
void EditorAnnotations::Impl::CommitText(bool cancel) {
    if (!text_ || committingText_) return;
    UpdateTextDraft(); committingText_=true;
    const HWND window=text_; text_=nullptr;
    KillTimer(window,kCaretTimer); RemoveWindowSubclass(window,TextProc,kTextSubclass); DestroyWindow(window);
    if (textFont_) DeleteObject(textFont_); textFont_=nullptr;
    bool changed=false;
    if (draft_ && !cancel) {
        const auto id=draft_->id;
        if (draft_->text.find_first_not_of(L" \r\n\t")!=std::wstring::npos) {
            changed=document_.Put(std::move(*draft_));
            if (changed) selected_=id!=0 ? id : document_.LastId();
        } else if (id!=0) changed=document_.Erase(id);
    }
    draft_.reset(); composing_=false; committingText_=false; textBounds_={};
    compositionText_.clear(); compositionBase_.clear();
    if (changed) Changed(); else RefreshControls();
    Paint();
}
LRESULT CALLBACK EditorAnnotations::Impl::TextProc(HWND window,UINT message,WPARAM wParam,LPARAM lParam,UINT_PTR,DWORD_PTR data) {
    auto* self=reinterpret_cast<Impl*>(data);
    if (message==WM_ERASEBKGND) return 1;
    if (message==WM_PAINT) { PAINTSTRUCT paint{}; BeginPaint(window,&paint); EndPaint(window,&paint); return 0; }
    if (message==WM_PRINT || message==WM_PRINTCLIENT) return 0;
    if (message==WM_GETDLGCODE) return DefSubclassProc(window,message,wParam,lParam)|DLGC_WANTALLKEYS;
    if (message==WM_TIMER && wParam==kCaretTimer) { self->caretVisible_=!self->caretVisible_; self->RequestPaint(); return 0; }
    if (message==WM_IME_STARTCOMPOSITION) {
        self->composing_=true; self->compositionText_.clear();
        self->compositionBase_=self->draft_ ? self->draft_->text : L"";
        DWORD start=0,end=0; SendMessageW(window,EM_GETSEL,reinterpret_cast<WPARAM>(&start),reinterpret_cast<LPARAM>(&end));
        self->compositionStart_=start; self->compositionLength_=end-start;
    }
    if (message==WM_KEYDOWN && !self->composing_ &&
        (wParam==VK_ESCAPE || (wParam==VK_RETURN && !(GetKeyState(VK_SHIFT)&0x8000)))) {
        self->CommitText(wParam==VK_ESCAPE); SetFocus(self->overlay_); return 0;
    }
    if (message==WM_KILLFOCUS) {
        KillTimer(window,kCaretTimer);
        if (!self->destroying_ && !self->committingText_)
            PostMessageW(self->overlay_,kCommitTextMessage,reinterpret_cast<WPARAM>(window),0);
    }
    if (message==WM_NCDESTROY) { KillTimer(window,kCaretTimer); RemoveWindowSubclass(window,TextProc,kTextSubclass); }
    const LRESULT result=DefSubclassProc(window,message,wParam,lParam);
    if (message==WM_SETFOCUS) {
        HideCaret(window);
        const UINT interval=GetCaretBlinkTime();
        if (interval!=INFINITE && interval!=0) SetTimer(window,kCaretTimer,std::max(100U,interval),nullptr);
        self->caretVisible_=true; self->RequestPaint();
    }
    if (message==WM_IME_COMPOSITION) {
        if (const HIMC context=ImmGetContext(window)) {
            if (lParam&GCS_COMPSTR) {
                const LONG bytes=ImmGetCompositionStringW(context,GCS_COMPSTR,nullptr,0);
                if (bytes>=0 && bytes<=static_cast<LONG>(annotations::MaximumTextLength*sizeof(wchar_t))) {
                    self->compositionText_.resize(static_cast<std::size_t>(bytes)/sizeof(wchar_t));
                    if (bytes>0) ImmGetCompositionStringW(context,GCS_COMPSTR,self->compositionText_.data(),static_cast<DWORD>(bytes));
                }
            }
            if (lParam&GCS_RESULTSTR) self->compositionText_.clear();
            ImmReleaseContext(window,context);
        }
        self->UpdateTextDraft();
    }
    if (message==WM_IME_ENDCOMPOSITION) {
        self->composing_=false; self->compositionText_.clear(); self->UpdateTextDraft();
    }
    // A multiline EDIT does not emit EN_CHANGE for every programmatic/paste path.
    if (message==WM_SETTEXT || message==WM_PASTE || message==WM_CUT || message==WM_CLEAR ||
        message==WM_UNDO || message==EM_REPLACESEL) self->UpdateTextDraft();
    if (message==WM_CHAR || message==WM_KEYDOWN || message==WM_LBUTTONDOWN || message==WM_LBUTTONUP ||
        message==EM_SETSEL || (message==WM_MOUSEMOVE && (wParam&MK_LBUTTON))) {
        self->caretVisible_=true; self->RequestPaint();
    }
    return result;
}
}  // namespace qrec
