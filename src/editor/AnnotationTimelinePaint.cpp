// Opaque atomic track rendering; annotation colors are kept as restrained tinted clips.
#include "editor/AnnotationTimeline.h"
#include "editor/EditorTheme.h"
#include "ui/AntiAliasedDrawing.h"
#include <algorithm>

namespace qrec {
namespace {
std::wstring Label(const annotations::Mark& mark) {
    switch (mark.tool) {
    case annotations::Tool::Circle: return L"圆圈";
    case annotations::Tool::Arrow: return L"箭头";
    case annotations::Tool::Text: {
        auto text=mark.text.substr(0,16); std::replace(text.begin(),text.end(),L'\n',L' '); return text;
    }
    default: return L"画笔";
    }
}
COLORREF ClipColor(std::uint32_t argb) {
    constexpr int strength=42;
    const auto channel=[=](unsigned value) { return 25+(static_cast<int>(value)-25)*strength/100; };
    return RGB(channel((argb>>16)&255),channel((argb>>8)&255),channel(argb&255));
}
}
void AnnotationTimeline::Paint(HDC target) {
    RECT r{}; GetClientRect(window_,&r);
    HDC buffer=CreateCompatibleDC(target); HBITMAP bitmap=CreateCompatibleBitmap(target,std::max(1L,r.right),std::max(1L,r.bottom));
    HGDIOBJ old=buffer && bitmap ? SelectObject(buffer,bitmap) : nullptr; HDC dc=old ? buffer : target;
    const HBRUSH background=CreateSolidBrush(editor_theme::Panel); FillRect(dc,&r,background); DeleteObject(background);
    const bool enabled=IsWindowEnabled(window_)!=FALSE;
    auto previous=SelectObject(dc,font_); SetBkMode(dc,TRANSPARENT);
    const auto* selected=Find(selected_);
    {
        ui::Canvas canvas(dc);
        if (selected) {
            const RECT bar=Bounds();
            const bool hovered=selected_==hoverId_;
            canvas.DrawRoundedRectangle(bar,static_cast<float>(Dip(3)),enabled ? ClipColor(selected->argb) : editor_theme::ControlDisabled,
                hovered ? editor_theme::TextPrimary : editor_theme::BorderHover);
            if (enabled) for (const int x : {static_cast<int>(bar.left)+Dip(3),static_cast<int>(bar.right)-Dip(4)}) {
                canvas.FillRoundedRectangle({x,bar.top+Dip(6),x+std::max(1,Dip(1)),bar.bottom-Dip(6)},1.0F,editor_theme::TextPrimary);
            }
        }
    }
    // GDI ClearType runs after GDI+ has flushed the opaque surfaces.
    if (selected) {
        RECT bar=Bounds();
        if (bar.right-bar.left>=Dip(34)) {
            bar.left+=Dip(8); bar.right-=Dip(8);
            SetTextColor(dc,enabled ? editor_theme::TextPrimary : editor_theme::TextDisabled);
            const auto text=Label(*selected);
            DrawTextW(dc,text.c_str(),-1,&bar,DT_SINGLELINE|DT_VCENTER|DT_END_ELLIPSIS|DT_NOPREFIX);
        }
    }
    if (snap_) {
        ui::Canvas canvas(dc);
        const int x=ToX(*snap_);
        canvas.FillRectangle({x,Dip(6),x+std::max(1,Dip(1)),Dip(32)},editor_theme::Focus,200);
    }
    SelectObject(dc,previous);
    if (old) { BitBlt(target,0,0,r.right,r.bottom,buffer,0,0,SRCCOPY); SelectObject(buffer,old); }
    if (bitmap) DeleteObject(bitmap); if (buffer) DeleteDC(buffer);
}
}  // namespace qrec
