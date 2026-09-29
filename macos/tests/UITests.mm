// Direct events target offscreen test views only; no global pointer/keyboard injection is used.
#import "tests/SelfTest.h"
#import "editor/EditorInternal.h"
#include <iostream>
#include <stdexcept>
#include <cmath>
namespace {
void Require(bool value,const char* label) {
    if (!value) throw std::runtime_error(label);
    std::cout<<"PASS "<<label<<'\n';
}
NSEvent* Mouse(NSView* view,NSPoint local,NSEventType type) {
    return [NSEvent mouseEventWithType:type location:[view convertPoint:local toView:nil] modifierFlags:0 timestamp:0
        windowNumber:view.window.windowNumber context:nil eventNumber:1 clickCount:1 pressure:1];
}
NSPoint Pixel(SRAnnotationView* canvas,double x,double y) {
    NSRect rect=canvas.videoRect; double scale=rect.size.width/canvas.sourceSize.width;
    return NSMakePoint(rect.origin.x+x*scale,rect.origin.y+y*scale);
}
void Stroke(SRAnnotationView* canvas,double x,double y,double endX,double endY) {
    [canvas mouseDown:Mouse(canvas,Pixel(canvas,x,y),NSEventTypeLeftMouseDown)];
    [canvas mouseDragged:Mouse(canvas,Pixel(canvas,endX,endY),NSEventTypeLeftMouseDragged)];
    [canvas mouseUp:Mouse(canvas,Pixel(canvas,endX,endY),NSEventTypeLeftMouseUp)];
}
}
void SRUITests(SREditorController* editor) {
    using qrec::annotations::Tool;
    auto* canvas=editor->_canvas;
    [editor selectTool:editor->_tools[1]]; Stroke(canvas,40,140,95,135);
    Require(editor->_document.Current()->Marks().size()==1,"free pen commits pointer path");
    [editor selectTool:editor->_tools[2]]; Stroke(canvas,80,80,112,80);
    uint64_t circleID=canvas.selectedID;
    auto circle=*editor->_document.Find(circleID);
    Require(circle.tool==Tool::Circle && fabs(circle.points[0].x-80)<0.1,"circle uses mouse-down as center");
    [editor selectTool:editor->_tools[3]]; Stroke(canvas,170,120,260,30);
    Require(editor->_document.Current()->Marks().back().tool==Tool::Arrow,"arrow commits from drag");
    [editor selectTool:editor->_tools[4]];
    [canvas mouseDown:Mouse(canvas,Pixel(canvas,40,10),NSEventTypeLeftMouseDown)];
    NSTextView* input=nil;
    for (NSView* view in canvas.subviews) if ([view isKindOfClass:NSTextView.class]) input=(NSTextView*)view;
    Require(input && !input.drawsBackground,"text entry is transparent and embedded in canvas");
    input.string=@"Mac 输入"; [canvas finishText:YES];
    Require(editor->_document.Current()->Marks().back().text==L"Mac 输入","text commits without changing tool");
    [editor selectTool:editor->_tools[1]];
    Stroke(canvas,112,80,130,92);
    circle=*editor->_document.Find(circleID);
    Require(fabs(circle.points[0].x-98)<0.1 && fabs(circle.points[0].y-92)<0.1 && canvas.tool==Tool::Pen,
            "existing circle drags directly while pen tool remains selected");
    SRTimelineView* timeline=editor->_timeline;
    Require(timeline.hasAnnotation && canvas.selectedID==circleID,"only selected annotation interval is active");
    auto timePoint=[&](double value) { return NSMakePoint(12+(timeline.bounds.size.width-24)*value/timeline.duration,15); };
    [timeline mouseDown:Mouse(timeline,timePoint(0),NSEventTypeLeftMouseDown)];
    [timeline mouseDragged:Mouse(timeline,timePoint(0.2),NSEventTypeLeftMouseDragged)];
    [timeline mouseUp:Mouse(timeline,timePoint(0.2),NSEventTypeLeftMouseUp)];
    Require(std::abs(editor->_document.Find(circleID)->start.count()-200)<=1,"annotation start handle updates time");
    [timeline mouseDown:Mouse(timeline,timePoint(0.35),NSEventTypeLeftMouseDown)];
    [timeline mouseDragged:Mouse(timeline,timePoint(0.65),NSEventTypeLeftMouseDragged)];
    [timeline mouseUp:Mouse(timeline,timePoint(0.65),NSEventTypeLeftMouseUp)];
    circle=*editor->_document.Find(circleID);
    Require(std::abs(circle.start.count()-500)<=1 && std::abs(circle.end.count()-800)<=1,"annotation strip moves without changing duration");
    size_t before=editor->_document.Current()->Marks().size();
    [editor removeMark:nil]; Require(editor->_document.Current()->Marks().size()==before-1,"delete selected annotation");
    [editor undo:nil]; Require(editor->_document.Current()->Marks().size()==before,"undo restores annotation");
    for (int i=0;i<12;++i) {
        [editor.window setContentSize:NSMakeSize(1000+(i%3)*100,680+(i%2)*80)];
        [editor layoutInterface];
        Require(NSEqualRects(editor->_canvas.frame,editor->_stage.bounds),"resize keeps canvas aligned to player");
    }
    canvas.time=0.2;
}
