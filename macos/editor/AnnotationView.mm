#import "editor/AnnotationView.h"
#import "editor/AnnotationDrawing.h"
#import "common/Support.h"
#include <algorithm>
#include <optional>
using namespace qrec::annotations;
@implementation SRAnnotationView {
    std::optional<Mark> _draft;
    Mark _original;
    Point _anchor;
    BOOL _moving;
    NSTextView* _text;
}
- (BOOL)isFlipped { return YES; }
- (BOOL)acceptsFirstResponder { return YES; }
- (instancetype)initWithFrame:(NSRect)frame {
    if ((self=[super initWithFrame:frame])) { _argb=0xFFFF595E; _strokeWidth=5; _tool=Tool::Select; }
    return self;
}
- (NSRect)videoRect {
    if (self.sourceSize.width<=0 || self.sourceSize.height<=0) return NSZeroRect;
    CGFloat scale=MIN(self.bounds.size.width/self.sourceSize.width,self.bounds.size.height/self.sourceSize.height);
    NSSize size=NSMakeSize(self.sourceSize.width*scale,self.sourceSize.height*scale);
    return NSMakeRect((self.bounds.size.width-size.width)/2,(self.bounds.size.height-size.height)/2,size.width,size.height);
}
- (Point)sourcePoint:(NSEvent*)event {
    NSPoint point=[self convertPoint:event.locationInWindow fromView:nil]; NSRect rect=self.videoRect;
    CGFloat scale=rect.size.width/self.sourceSize.width;
    return {(float)((point.x-rect.origin.x)/scale),(float)((point.y-rect.origin.y)/scale)};
}
- (void)setTime:(double)time { _time=time; self.needsDisplay=YES; }
- (void)drawRect:(NSRect)dirty {
    if (!self.document) return;
    NSRect rect=self.videoRect; if (NSIsEmptyRect(rect)) return;
    CGContextRef context=NSGraphicsContext.currentContext.CGContext;
    CGContextSaveGState(context); CGContextClipToRect(context,rect);
    CGContextTranslateCTM(context,rect.origin.x,rect.origin.y);
    CGFloat scale=rect.size.width/self.sourceSize.width; CGContextScaleCTM(context,scale,scale);
    sr::DrawScene(context,self.document->Current(),self.time,_draft ? _draft->id : 0);
    if (_draft && !_text) sr::DrawMark(context,*_draft);
    CGContextRestoreGState(context);
}
- (void)mouseDown:(NSEvent*)event {
    if (self.locked) return;
    [self finishText:YES];
    if (self.willInteract) self.willInteract();
    [self.window makeFirstResponder:self];
    if (!self.document || !NSPointInRect([self convertPoint:event.locationInWindow fromView:nil],self.videoRect)) return;
    Point point=[self sourcePoint:event]; _anchor=point; _moving=NO;
    float tolerance=(float)(7*self.sourceSize.width/self.videoRect.size.width);
    const auto& marks=self.document->Current()->Marks();
    for (auto it=marks.rbegin();it!=marks.rend();++it) {
        if (Visible(*it,Time{(int64_t)llround(self.time*1000)}) && HitTest(*it,point,tolerance)) {
            self.selectedID=it->id; _draft=*it; _original=*it; _moving=YES;
            if (event.clickCount==2 && it->tool==Tool::Text) [self beginText];
            if (self.selectionChanged) self.selectionChanged(); self.needsDisplay=YES; return;
        }
    }
    self.selectedID=0; if (self.selectionChanged) self.selectionChanged();
    if (self.tool==Tool::Select) { self.needsDisplay=YES; return; }
    _draft=self.document->NewMark(self.tool,Time{(int64_t)llround(self.time*1000)});
    _draft->strokeWidth=self.strokeWidth; _draft->argb=self.argb; _draft->points={point};
    if (self.tool==Tool::Circle || self.tool==Tool::Arrow) _draft->points.push_back(point);
    if (self.tool==Tool::Text) { _draft->fontSize=MAX(18,self.sourceSize.height*0.05); [self beginText]; }
    self.needsDisplay=YES;
}
- (void)mouseDragged:(NSEvent*)event {
    if (!_draft || _text) return;
    Point point=[self sourcePoint:event];
    if (_moving) { *_draft=_original; Translate(*_draft,{point.x-_anchor.x,point.y-_anchor.y}); }
    else if (_draft->tool==Tool::Pen) {
        Point last=_draft->points.back();
        if (std::hypot(point.x-last.x,point.y-last.y)>=0.6 && _draft->points.size()<MaximumPoints) _draft->points.push_back(point);
    } else if (_draft->points.size()==2) _draft->points.back()=point;
    self.needsDisplay=YES;
}
- (void)mouseUp:(NSEvent*)event { if (_draft && !_text) [self commitDraft]; }
- (void)commitDraft {
    if (!_draft || !self.document) return;
    uint64_t existing=_draft->id;
    if (self.document->Put(*_draft)) {
        self.selectedID=existing?existing:self.document->LastId(); if (self.edited) self.edited();
    }
    _draft.reset(); self.needsDisplay=YES; if (self.selectionChanged) self.selectionChanged();
}
- (void)beginText {
    if (!_draft) return;
    NSRect rect=self.videoRect; CGFloat scale=rect.size.width/self.sourceSize.width;
    NSPoint anchor=NSMakePoint(rect.origin.x+_draft->points[0].x*scale,rect.origin.y+_draft->points[0].y*scale);
    _text=[[NSTextView alloc] initWithFrame:NSMakeRect(anchor.x,anchor.y,MAX(120,MIN(480,NSMaxX(rect)-anchor.x)),100)];
    _text.drawsBackground=NO; _text.richText=NO; _text.importsGraphics=NO;
    _text.font=[NSFont systemFontOfSize:_draft->fontSize*scale];
    _text.textColor=sr::Color(_draft->argb&0xFFFFFF); _text.insertionPointColor=_text.textColor;
    _text.textContainerInset=NSZeroSize; _text.textContainer.lineFragmentPadding=0;
    _text.horizontallyResizable=YES; _text.textContainer.widthTracksTextView=NO;
    _text.textContainer.containerSize=NSMakeSize(CGFLOAT_MAX,CGFLOAT_MAX);
    _text.string=sr::String(_draft->text); _text.delegate=self;
    _text.wantsLayer=YES; _text.layer.borderWidth=1; _text.layer.borderColor=sr::Color(0x65B1DC,0.75).CGColor;
    _text.layer.cornerRadius=3; _text.toolTip=@"直接输入文字；回车完成，Shift+回车换行，Esc 取消";
    [self addSubview:_text]; [self.window makeFirstResponder:_text]; [_text selectAll:nil]; self.needsDisplay=YES;
}
- (BOOL)textView:(NSTextView*)view doCommandBySelector:(SEL)command {
    if (command==@selector(cancelOperation:)) { [self finishText:NO]; return YES; }
    if (command==@selector(insertNewline:) && !(NSApp.currentEvent.modifierFlags&NSEventModifierFlagShift)) {
        [self finishText:YES]; return YES;
    }
    return NO;
}
- (void)finishText:(BOOL)commit {
    if (!_text) return;
    NSString* text=_text.string; NSTextView* field=_text; _text=nil;
    field.delegate=nil; [field removeFromSuperview];
    if (commit && text.length && _draft) { _draft->text=sr::Wide(text); [self commitDraft]; }
    else { _draft.reset(); self.needsDisplay=YES; }
    [self.window makeFirstResponder:self];
}
- (void)deleteSelection {
    [self finishText:YES];
    if (self.document && self.document->Erase(self.selectedID)) {
        self.selectedID=0; self.needsDisplay=YES;
        if (self.edited) self.edited(); if (self.selectionChanged) self.selectionChanged();
    }
}
- (void)keyDown:(NSEvent*)event {
    if (event.keyCode==51 || event.keyCode==117) [self deleteSelection]; else [super keyDown:event];
}
@end
