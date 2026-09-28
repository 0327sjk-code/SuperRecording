// Input-only caret, selection and corner guides. Never included in exported pixels.
#include "editor/EditorAnnotationsInternal.h"
#include "editor/EditorTheme.h"
#include "ui/AntiAliasedDrawing.h"
#include <windowsx.h>
#include <algorithm>

namespace qrec {
void EditorAnnotations::Impl::DrawTextInputGuide() {
    if (!text_ || !draft_ || !pixels_ || !textFont_) return;
    using namespace Gdiplus;
    Bitmap bitmap(bitmapWidth_,bitmapHeight_,bitmapWidth_*4,PixelFormat32bppPARGB,pixels_);
    Graphics graphics(&bitmap);
    graphics.SetSmoothingMode(SmoothingModeAntiAlias);
    const float dpi=static_cast<float>(GetDpiForWindow(text_))/96.0F;
    const float left=std::max(1.0F,static_cast<float>(textBounds_.left)-2*dpi);
    const float top=std::max(1.0F,static_cast<float>(textBounds_.top)-2*dpi);
    const float right=std::min(static_cast<float>(bitmapWidth_)-1,static_cast<float>(textBounds_.right)+2*dpi);
    const float bottom=std::min(static_cast<float>(bitmapHeight_)-1,static_cast<float>(textBounds_.bottom)+2*dpi);
    Pen guide(ui::ToGdiPlusColor(editor_theme::Focus),std::max(1.0F,dpi));
    const float corner=std::min({6*dpi,(right-left)/2,(bottom-top)/2});
    if (corner>0) for (const auto x : {left,right}) for (const auto y : {top,bottom}) {
        graphics.DrawLine(&guide,x,y,x+(x==left ? corner : -corner),y);
        graphics.DrawLine(&guide,x,y,x,y+(y==top ? corner : -corner));
    }
    HDC dc=GetDC(text_); const auto old=SelectObject(dc,textFont_);
    TEXTMETRICW metrics{}; GetTextMetricsW(dc,&metrics);
    const auto position=[&](DWORD index) {
        const LRESULT line=SendMessageW(text_,EM_LINEFROMCHAR,index,0);
        const LRESULT start=SendMessageW(text_,EM_LINEINDEX,static_cast<WPARAM>(line),0);
        const auto text=draft_->text;
        const std::size_t begin=std::min<std::size_t>(start<0 ? 0 : static_cast<std::size_t>(start),text.size());
        const std::size_t end=std::clamp<std::size_t>(index,begin,text.size());
        SIZE extent{};
        GetTextExtentPoint32W(dc,text.data()+begin,static_cast<int>(end-begin),&extent);
        return POINT{textBounds_.left+extent.cx,textBounds_.top+static_cast<LONG>(line)*metrics.tmHeight};
    };
    DWORD start=0,end=0;
    SendMessageW(text_,EM_GETSEL,reinterpret_cast<WPARAM>(&start),reinterpret_cast<LPARAM>(&end));
    if (end>start) {
        SolidBrush selection(Color(70,101,177,220));
        const auto from=position(start),to=position(end);
        const int lineHeight=std::max<LONG>(1,metrics.tmHeight);
        for (int y=from.y;y<=to.y;y+=lineHeight) {
            const int x1=y==from.y ? from.x : textBounds_.left;
            const int x2=y==to.y ? to.x : textBounds_.right;
            graphics.FillRectangle(&selection,static_cast<REAL>(x1),static_cast<REAL>(y),static_cast<REAL>(std::max(0,x2-x1)),static_cast<REAL>(lineHeight));
        }
    }
    if (caretVisible_ && GetFocus()==text_) {
        POINT caret{};
        if (GetCaretPos(&caret)) { caret.x+=textBounds_.left; caret.y+=textBounds_.top; }
        else caret=position(end);
        const float x=std::clamp(static_cast<float>(caret.x),left,right);
        const float y=std::clamp(static_cast<float>(caret.y),top,bottom);
        const float height=std::min(bottom-y,std::max(12*dpi,static_cast<float>(metrics.tmHeight)));
        Pen outline(Color(210,0,0,0),3*dpi); Pen ink(Color(255,245,245,242),std::max(1.0F,dpi));
        graphics.DrawLine(&outline,x,y,x,y+height); graphics.DrawLine(&ink,x,y,x,y+height);
    }
    SelectObject(dc,old); ReleaseDC(text_,dc);
    graphics.Flush(FlushIntentionSync);
}
}  // namespace qrec
