#import "editor/TimelineView.h"
#import "common/Support.h"
#include <algorithm>
@implementation SRTimelineView {
    NSInteger _drag;
    double _anchor;
    double _oldStart;
    double _oldEnd;
}
- (BOOL)isFlipped { return YES; }
- (BOOL)acceptsFirstResponder { return YES; }
- (CGFloat)x:(double)time { return 12+(self.bounds.size.width-24)*time/MAX(0.001,self.duration); }
- (double)time:(CGFloat)x { return std::clamp((x-12)/(self.bounds.size.width-24)*self.duration,0.0,self.duration); }
- (void)drawRect:(NSRect)dirty {
    NSDictionary* labels=@{NSFontAttributeName:[NSFont monospacedDigitSystemFontOfSize:11 weight:NSFontWeightRegular],
                           NSForegroundColorAttributeName:sr::Color(0xB8BBC0)};
    NSString* left=[@"起点 " stringByAppendingString:sr::TimeLabel(self.start)];
    NSString* right=[@"终点 " stringByAppendingString:sr::TimeLabel(self.end)];
    [left drawAtPoint:NSMakePoint(12,35) withAttributes:labels];
    [right drawAtPoint:NSMakePoint(self.bounds.size.width-12-[right sizeWithAttributes:labels].width,35) withAttributes:labels];
    NSRect rail=NSMakeRect(12,56,self.bounds.size.width-24,16);
    [sr::Color(0x24272B) setFill]; [[NSBezierPath bezierPathWithRoundedRect:rail xRadius:4 yRadius:4] fill];
    [sr::Color(0x303C49) setFill];
    NSRect selected=NSMakeRect([self x:self.start],56,[self x:self.end]-[self x:self.start],16);
    [[NSBezierPath bezierPathWithRoundedRect:selected xRadius:3 yRadius:3] fill];
    [sr::Color(0x526272) setStroke];
    for (int i=1;i<10;++i) {
        CGFloat x=12+rail.size.width*i/10;
        NSBezierPath* tick=[NSBezierPath bezierPath]; [tick moveToPoint:NSMakePoint(x,61)]; [tick lineToPoint:NSMakePoint(x,67)]; [tick stroke];
    }
    [self handleAt:[self x:self.start] y:52 color:0x788697];
    [self handleAt:[self x:self.end] y:52 color:0x788697];
    [sr::Color(0xF4F4F2) setFill]; NSRectFill(NSMakeRect([self x:self.position]-1,52,2,24));
    if (self.hasAnnotation) {
        CGFloat x=[self x:self.annotationStart],end=[self x:self.annotationEnd];
        [sr::Color(0x415F70) setFill];
        [[NSBezierPath bezierPathWithRoundedRect:NSMakeRect(x,7,MAX(3,end-x),20) xRadius:4 yRadius:4] fill];
        [self handleAt:x y:5 color:0x65B1DC]; [self handleAt:end y:5 color:0x65B1DC];
    }
}
- (void)handleAt:(CGFloat)x y:(CGFloat)y color:(unsigned)color {
    [sr::Color(color) setFill];
    [[NSBezierPath bezierPathWithRoundedRect:NSMakeRect(x-5,y,10,24) xRadius:3 yRadius:3] fill];
    [sr::Color(0x121315) setFill]; NSRectFill(NSMakeRect(x-0.5,y+6,1,12));
}
- (void)mouseDown:(NSEvent*)event {
    if (self.locked) return;
    [self.window makeFirstResponder:self];
    NSPoint p=[self convertPoint:event.locationInWindow fromView:nil]; _anchor=[self time:p.x];
    _drag=0;
    if (self.hasAnnotation && p.y<31) {
        CGFloat left=[self x:self.annotationStart],right=[self x:self.annotationEnd];
        BOOL onLeft=fabs(p.x-left)<=9,onRight=fabs(p.x-right)<=9;
        if (onLeft || onRight) _drag=(onLeft && (!onRight || fabs(p.x-left)<fabs(p.x-right)))?3:4;
        else if (p.x>left && p.x<right) _drag=5;
        _oldStart=self.annotationStart; _oldEnd=self.annotationEnd;
    } else if (p.y>=31) {
        if (fabs(p.x-[self x:self.start])<=10) _drag=1;
        else if (fabs(p.x-[self x:self.end])<=10) _drag=2;
        else _drag=6;
    }
    [self mouseDragged:event];
}
- (void)mouseDragged:(NSEvent*)event {
    if (!_drag) return;
    double value=[self time:[self convertPoint:event.locationInWindow fromView:nil].x];
    if (_drag>=3 && _drag<=5 && fabs(value-self.position)<self.duration*7/MAX(1,self.bounds.size.width)) value=self.position;
    double span=MAX(0.001,self.minimumSpan);
    switch (_drag) {
        case 1: self.start=std::clamp(value,0.0,MAX(0.0,self.end-span)); break;
        case 2: self.end=std::clamp(value,MIN(self.duration,self.start+span),self.duration); break;
        case 3: self.annotationStart=std::clamp(value,0.0,MAX(0.0,self.annotationEnd-0.001)); break;
        case 4: self.annotationEnd=std::clamp(value,MIN(self.duration,self.annotationStart+0.001),self.duration); break;
        case 5: {
            double offset=std::clamp(value-_anchor,-_oldStart,self.duration-_oldEnd);
            self.annotationStart=_oldStart+offset; self.annotationEnd=_oldEnd+offset; break;
        }
        case 6: self.position=std::clamp(value,self.start,self.end); break;
    }
    self.needsDisplay=YES;
    if (self.changed) self.changed(_drag<=2?SRTimelineTrim:(_drag<=5?SRTimelineAnnotation:SRTimelineSeek),NO);
}
- (void)mouseUp:(NSEvent*)event {
    if (_drag && self.changed) self.changed(_drag<=2?SRTimelineTrim:(_drag<=5?SRTimelineAnnotation:SRTimelineSeek),YES);
    _drag=0;
}
- (void)keyDown:(NSEvent*)event {
    if (self.locked) return;
    if (event.keyCode==123 || event.keyCode==124) {
        self.position=std::clamp(self.position+(event.keyCode==123?-1:1)*MAX(0.001,self.minimumSpan),self.start,self.end);
        self.needsDisplay=YES; if (self.changed) self.changed(SRTimelineSeek,YES);
    } else [super keyDown:event];
}
- (BOOL)isAccessibilityElement { return YES; }
- (NSString*)accessibilityLabel { return @"视频裁剪时间轴，左右方向键逐帧定位"; }
- (id)accessibilityValue { return [NSString stringWithFormat:@"%@ 至 %@，播放位置 %@",sr::TimeLabel(self.start),sr::TimeLabel(self.end),sr::TimeLabel(self.position)]; }
@end
