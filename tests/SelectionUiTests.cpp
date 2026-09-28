// Real window hit-testing and selector gestures on the existing non-input test desktop.
#include "AnnotationTestSupport.h"
#include "selection/RegionSelector.h"
#include <array>
#include <iostream>

namespace annotation_tests {
namespace {
constexpr wchar_t kSelectorClass[]=L"SuperRecording.RegionSelector.Window";
constexpr wchar_t kInputClass[]=L"SuperRecording.SelectionInputSurface";
HWND FindOwnWindow(const wchar_t* name) {
    const HWND window=FindWindowW(name,nullptr);
    DWORD process=0; GetWindowThreadProcessId(window,&process);
    return process==GetCurrentProcessId() ? window : nullptr;
}
void Mouse(HWND window,UINT message,POINT screen) {
    DWORD process=0; GetWindowThreadProcessId(window,&process);
    if (process!=GetCurrentProcessId()) { Expect(false,L"Never send test input to another process"); return; }
    ScreenToClient(window,&screen);
    SendMessageW(window,message,message==WM_LBUTTONUP ? 0 : MK_LBUTTON,MAKELPARAM(screen.x,screen.y));
}
struct Probe final {
    bool adjust{};
    RECT expected{};
    bool ran{};
    void Run() {
        ran=true;
        const HWND selector=FindOwnWindow(kSelectorClass);
        Expect(selector!=nullptr,L"Modal selector belongs to the isolated test process");
        if (!selector) return;
        MONITORINFO monitor{sizeof(monitor)};
        GetMonitorInfoW(MonitorFromPoint({0,0},MONITOR_DEFAULTTOPRIMARY),&monitor);
        expected={monitor.rcMonitor.left+160,monitor.rcMonitor.top+160,monitor.rcMonitor.left+760,monitor.rcMonitor.top+520};
        Mouse(selector,WM_LBUTTONDOWN,{expected.left,expected.top});
        Mouse(selector,WM_MOUSEMOVE,{expected.right,expected.bottom});
        Mouse(selector,WM_LBUTTONUP,{expected.right,expected.bottom});
        if (!adjust) {
            Expect(!FindOwnWindow(kInputClass),L"Direct-record mode creates no adjustment input surface");
            return;
        }
        UpdateWindow(selector);
        const HWND input=FindOwnWindow(kInputClass);
        Expect(input && IsWindowVisible(input),L"Adjustment creates the transparent interior input surface");
        if (!input) { SendMessageW(selector,WM_CLOSE,0,0); return; }
        UpdateWindow(input);
        COLORREF key=0; BYTE alpha=0; DWORD flags=0;
        Expect(GetLayeredWindowAttributes(input,&key,&alpha,&flags) && alpha==1 && flags==LWA_ALPHA,
            L"Interior is 1/255 alpha, not a click-through color-key hole");
        const POINT center{(expected.left+expected.right)/2,(expected.top+expected.bottom)/2};
        ShowWindow(input,SW_HIDE);
        const bool oldHoleMissed=WindowFromPoint(center)!=selector;
        ShowWindow(input,SW_SHOWNOACTIVATE); UpdateWindow(input);
        unsigned hitCount=0;
        for (int row=1;row<=3;++row) for (int column=1;column<=3;++column) {
            RECT before{}; GetWindowRect(input,&before);
            POINT point{before.left+(before.right-before.left)*column/4,before.top+(before.bottom-before.top)*row/4};
            const HWND target=WindowFromPoint(point);
            if (target==input) ++hitCount;
            Expect(target==input,L"OS hit testing routes every interior sample to the selector, not the desktop behind it");
            if (target!=input) continue;
            Mouse(target,WM_LBUTTONDOWN,point);
            point.x+=16; point.y+=8;
            Mouse(selector,WM_MOUSEMOVE,point); Mouse(selector,WM_LBUTTONUP,point);
            UpdateWindow(selector);
            RECT after{}; GetWindowRect(input,&after);
            Expect(after.left==before.left+16 && after.top==before.top+8 &&
                after.right==before.right+16 && after.bottom==before.bottom+8,L"Interior drag moves the entire selected region without resizing");
        }
        std::wcout<<L"Selection input: oldColorKeyHoleMissed="<<oldHoleMissed<<L" interiorHits="<<hitCount<<L" / 9"<<std::endl;
        RECT before{}; GetWindowRect(input,&before);
        POINT edge{before.right-1,(before.top+before.bottom)/2};
        Mouse(input,WM_LBUTTONDOWN,edge); edge.x+=20;
        Mouse(selector,WM_MOUSEMOVE,edge); Mouse(selector,WM_LBUTTONUP,edge); UpdateWindow(selector);
        GetWindowRect(input,&expected);
        Expect(expected.right==before.right+20 && expected.left==before.left && expected.top==before.top && expected.bottom==before.bottom,
            L"Edge resize still has priority over moving the interior");
        RECT local=expected,client{},activeMonitor=monitor.rcMonitor;
        GetClientRect(selector,&client); MapWindowPoints(nullptr,selector,reinterpret_cast<POINT*>(&local),2);
        MapWindowPoints(nullptr,selector,reinterpret_cast<POINT*>(&activeMonitor),2);
        const auto controls=qrec::selection::view::CalculateControlLayout(client,local,activeMonitor,GetDpiForWindow(selector));
        POINT start{(controls.startButton.left+controls.startButton.right)/2,(controls.startButton.top+controls.startButton.bottom)/2};
        ClientToScreen(selector,&start);
        const HWND target=WindowFromPoint(start);
        Expect(target==selector || target==input,L"Start recording remains reachable with the interior input surface");
        if (target==selector || target==input) { Mouse(target,WM_LBUTTONDOWN,start); Mouse(selector,WM_LBUTTONUP,start); }
        else SendMessageW(selector,WM_CLOSE,0,0);
    }
};
thread_local Probe* activeProbe=nullptr;
void CALLBACK RunProbe(HWND,UINT,UINT_PTR timer,DWORD) {
    KillTimer(nullptr,timer);
    if (activeProbe) activeProbe->Run();
}
}
void SelectionUiTests() {
    for (const bool adjust : {true,false}) {
        Probe probe; probe.adjust=adjust; activeProbe=&probe;
        const UINT_PTR timer=SetTimer(nullptr,0,100,RunProbe);
        if (!timer) { Expect(false,L"Selector test timer created"); activeProbe=nullptr; return; }
        qrec::selection::RegionSelector selector;
        const auto result=selector.Select(nullptr,adjust);
        KillTimer(nullptr,timer); activeProbe=nullptr;
        Expect(probe.ran && result.has_value(),L"Selector completes through its real modal workflow");
        if (result) Expect(result->left==probe.expected.left && result->top==probe.expected.top &&
            result->right==probe.expected.right && result->bottom==probe.expected.bottom,L"Recording region matches final adjusted bounds");
        Expect(!FindOwnWindow(kInputClass) && !FindOwnWindow(kSelectorClass),L"Selection windows are destroyed before capture starts");
    }
}
}  // namespace annotation_tests
