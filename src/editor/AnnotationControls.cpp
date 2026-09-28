#include "editor/AnnotationControls.h"
#include "editor/EditorTheme.h"
#include <commctrl.h>
#include <algorithm>
#include <cmath>
#include <utility>

namespace qrec {
namespace {
constexpr wchar_t kClass[]=L"SuperRecording.AnnotationControls";
constexpr int kFirstTool=8100;
constexpr int kTimeline=8200,kColor=8202,kWidth=8203;
constexpr std::array<const wchar_t*,8> kNames{
    L"选择 / 移动标注 (V)",L"自由画笔 (P)",L"圆圈 · 按下点为圆心 (O)",
    L"箭头 · 从起点拖向终点 (A)",L"文字 · 点击输入，双击修改 (T)",
    L"撤销 (Ctrl+Z)",L"重做 (Ctrl+Y)",L"删除选中标注 (Delete)"};
int Dip(HWND window,int value) { return MulDiv(value,static_cast<int>(GetDpiForWindow(window)),96); }
void Position(HWND window,int x,int y,int w,int h) {
    SetWindowPos(window,nullptr,x,y,w,h,SWP_NOZORDER|SWP_NOACTIVATE);
}
}
AnnotationControls::~AnnotationControls() { Destroy(); }
void AnnotationControls::Destroy() noexcept {
    callbacks_={}; width_.Destroy(); timeline_.Destroy();
    for (HWND window : {tooltip_,rail_}) if (IsWindow(window)) DestroyWindow(window);
    rail_=tooltip_=color_=colorLabel_=nullptr; buttons_.fill(nullptr); chrome_=nullptr;
}
bool AnnotationControls::Create(HWND parent,HINSTANCE instance,EditorChrome* chrome,AnnotationControlCallbacks callbacks) {
    Destroy(); chrome_=chrome; callbacks_=std::move(callbacks);
    WNDCLASSEXW wc{sizeof(wc)}; wc.hInstance=instance; wc.lpfnWndProc=WindowProc;
    wc.lpszClassName=kClass; wc.hCursor=LoadCursorW(nullptr,IDC_ARROW);
    if (!RegisterClassExW(&wc) && GetLastError()!=ERROR_CLASS_ALREADY_EXISTS) return false;
    rail_=CreateWindowExW(WS_EX_CONTROLPARENT,kClass,L"标注工具",WS_CHILD|WS_VISIBLE|WS_CLIPCHILDREN|WS_CLIPSIBLINGS,
        0,0,1,1,parent,nullptr,instance,this);
    if (!rail_) return false;
    const auto create=[&](const wchar_t* cls,const wchar_t* title,DWORD style,int id) {
        return CreateWindowExW(0,cls,title,WS_CHILD|WS_VISIBLE|style,0,0,1,1,rail_,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),instance,nullptr);
    };
    tooltip_=CreateWindowExW(WS_EX_TOOLWINDOW,TOOLTIPS_CLASSW,nullptr,WS_POPUP|TTS_ALWAYSTIP|TTS_NOPREFIX,
        CW_USEDEFAULT,CW_USEDEFAULT,CW_USEDEFAULT,CW_USEDEFAULT,parent,nullptr,instance,nullptr);
    for (std::size_t i=0;i<buttons_.size();++i) {
        buttons_[i]=create(WC_BUTTONW,kNames[i],WS_TABSTOP|BS_OWNERDRAW,kFirstTool+static_cast<int>(i));
        if (!buttons_[i]) return false;
        chrome_->AttachButton(buttons_[i]);
        if (tooltip_) {
            TOOLINFOW info{sizeof(info)}; info.uFlags=TTF_IDISHWND|TTF_SUBCLASS;
            info.hwnd=rail_; info.uId=reinterpret_cast<UINT_PTR>(buttons_[i]);
            info.lpszText=const_cast<wchar_t*>(kNames[i]);
            SendMessageW(tooltip_,TTM_ADDTOOLW,0,reinterpret_cast<LPARAM>(&info));
        }
    }
    colorLabel_=create(WC_STATICW,L"颜色",SS_LEFT|SS_CENTERIMAGE,0);
    color_=create(WC_BUTTONW,L"标注颜色",WS_TABSTOP|BS_OWNERDRAW,kColor);
    if (!color_ || !colorLabel_) return false;
    chrome_->AttachButton(color_);
    if (!width_.Create(rail_,instance,kWidth,AnnotationSliderKind::Width,[this](double value,AnnotationEditPhase phase) {
        if (callbacks_.widthChanged) callbacks_.widthChanged(static_cast<float>(value),phase);
    })) return false;
    if (tooltip_) {
        TOOLINFOW info{sizeof(info)}; info.uFlags=TTF_IDISHWND|TTF_SUBCLASS;
        info.hwnd=rail_; info.uId=reinterpret_cast<UINT_PTR>(width_.Window());
        info.lpszText=const_cast<wchar_t*>(L"拖动调节粗细 · 悬停时滚轮微调");
        SendMessageW(tooltip_,TTM_ADDTOOLW,0,reinterpret_cast<LPARAM>(&info));
    }
    return timeline_.Create(parent,instance,kTimeline,{callbacks_.select,callbacks_.rangeChanged});
}
void AnnotationControls::Layout(const EditorChromeLayout& layout) {
    if (!rail_) return;
    const RECT tools=layout.annotationTools;
    const int w=tools.right-tools.left;
    Position(rail_,tools.left,tools.top,w,tools.bottom-tools.top);
    const int side=Dip(rail_,36),gap=Dip(rail_,4);
    const int x=(w-side)/2;
    for (int i=0;i<5;++i) Position(buttons_[i],x,i*(side+gap),side,side);
    int y=5*(side+gap)+Dip(rail_,8);
    const int small=Dip(rail_,26);
    for (int i=5;i<8;++i) Position(buttons_[i],(w-small*3)/2+(i-5)*small,y,small,small);
    y+=small+Dip(rail_,16);
    Position(colorLabel_,Dip(rail_,8),y,w-Dip(rail_,42),Dip(rail_,30));
    Position(color_,w-Dip(rail_,34),y,Dip(rail_,28),Dip(rail_,30));
    SendMessageW(colorLabel_,WM_SETFONT,reinterpret_cast<WPARAM>(chrome_->CaptionFont()),TRUE);
    y+=Dip(rail_,40);
    width_.Layout({0,y,w,y+Dip(rail_,40)},chrome_->CaptionFont());
    timeline_.Layout(layout.annotationTrack,chrome_->CaptionFont());
    InvalidateRect(rail_,nullptr,FALSE);
}
void AnnotationControls::Refresh(annotations::Tool tool,const annotations::Document& document,
    const annotations::Mark* selected,bool enabled,std::uint32_t color,float width) {
    if (!rail_) return;
    const bool toolChanged=tool_!=tool, colorChanged=colorArgb_!=color;
    tool_=tool; enabled_=enabled; colorArgb_=color;
    for (std::size_t i=0;i<buttons_.size();++i) {
        const bool available=i<5 || (i==5 && document.CanUndo()) || (i==6 && document.CanRedo()) || (i==7 && selected);
        const bool active=enabled && available;
        if ((IsWindowEnabled(buttons_[i])!=FALSE)!=active) EnableWindow(buttons_[i],active);
        if (toolChanged && i<5) InvalidateRect(buttons_[i],nullptr,FALSE);
    }
    EnableWindow(color_,enabled);
    if (colorChanged) InvalidateRect(color_,nullptr,FALSE);
    width_.SetState(width,std::max(32.0,static_cast<double>(width)),0,0,enabled);
    timeline_.SetState(document.Current(),selected,document.Duration(),enabled);
}
void AnnotationControls::SelectColor() {
    constexpr std::array<std::uint32_t,6> colors{0xFFFF595E,0xFFFFD166,0xFF70D6FF,0xFF80ED99,0xFFFFFFFF,0xFF17191D};
    constexpr const wchar_t* names[]{L"红色",L"黄色",L"蓝色",L"绿色",L"白色",L"黑色"};
    HMENU menu=CreatePopupMenu(); if (!menu) return;
    for (std::size_t i=0;i<colors.size();++i) AppendMenuW(menu,MF_STRING|(colors[i]==colorArgb_ ? MF_CHECKED : 0),i+1,names[i]);
    RECT r{}; GetWindowRect(color_,&r);
    const UINT choice=TrackPopupMenu(menu,TPM_RETURNCMD|TPM_NONOTIFY,r.right,r.top,0,rail_,nullptr);
    DestroyMenu(menu);
    if (choice>=1 && choice<=colors.size() && callbacks_.colorChanged) callbacks_.colorChanged(colors[choice-1]);
}
LRESULT CALLBACK AnnotationControls::WindowProc(HWND window,UINT message,WPARAM wParam,LPARAM lParam) {
    auto* self=reinterpret_cast<AnnotationControls*>(GetWindowLongPtrW(window,GWLP_USERDATA));
    if (message==WM_NCCREATE) {
        self=static_cast<AnnotationControls*>(reinterpret_cast<CREATESTRUCTW*>(lParam)->lpCreateParams);
        SetWindowLongPtrW(window,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(self));
    }
    return self ? self->Message(window,message,wParam,lParam) : DefWindowProcW(window,message,wParam,lParam);
}
LRESULT AnnotationControls::Message(HWND window,UINT message,WPARAM wParam,LPARAM lParam) {
    switch (message) {
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: { PAINTSTRUCT ps{}; HDC dc=BeginPaint(window,&ps); FillRect(dc,&ps.rcPaint,chrome_->PanelBrush()); EndPaint(window,&ps); return 0; }
    case WM_CTLCOLORSTATIC:
        SetTextColor(reinterpret_cast<HDC>(wParam),editor_theme::TextSecondary);
        SetBkColor(reinterpret_cast<HDC>(wParam),editor_theme::Panel);
        return reinterpret_cast<LRESULT>(chrome_->PanelBrush());
    case WM_DRAWITEM: {
        auto* item=reinterpret_cast<DRAWITEMSTRUCT*>(lParam); if (!item) return FALSE;
        EditorButtonPaintState state{};
        const int index=static_cast<int>(item->CtlID)-kFirstTool;
        if (index>=0 && index<8) {
            state.role=static_cast<EditorButtonRole>(static_cast<int>(EditorButtonRole::AnnotationSelect)+index);
            state.selected=index==static_cast<int>(tool_);
        } else if (item->CtlID==kColor) {
            state.role=EditorButtonRole::AnnotationColor;
            state.iconColor=RGB((colorArgb_>>16)&255,(colorArgb_>>8)&255,colorArgb_&255);
        }
        return chrome_->DrawButton(window,item,state) ? TRUE : FALSE;
    }
    case WM_COMMAND: {
        const int id=LOWORD(wParam);
        if (HIWORD(wParam)!=BN_CLICKED || !enabled_) break;
        if (id>=kFirstTool && id<kFirstTool+8 && callbacks_.action) callbacks_.action(static_cast<AnnotationAction>(id-kFirstTool));
        else if (id==kColor) SelectColor();
        return 0;
    }
    default: break;
    }
    return DefWindowProcW(window,message,wParam,lParam);
}
}  // namespace qrec
