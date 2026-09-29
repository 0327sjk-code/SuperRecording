#import "capture/RegionSelector.h"
#import "common/Support.h"
#include <algorithm>
@implementation SRRegionSelection
- (CGDirectDisplayID)displayID { return [self.screen.deviceDescription[@"NSScreenNumber"] unsignedIntValue]; }
@end
@interface SRSelectionWindow : NSWindow @end
@implementation SRSelectionWindow
- (BOOL)canBecomeKeyWindow { return YES; }
@end
@class SRSelectionView;
@interface SRRegionSelector ()
@property(strong) NSMutableArray<NSWindow*>* windows;
@property(strong) SRRegionSelection* selected;
@property(copy) void (^completion)(SRRegionSelection*);
@property BOOL adjust;
@property BOOL recording;
- (void)refresh;
- (void)accept;
- (void)cancel;
@end
@interface SRSelectionView : NSView
@property(weak) SRRegionSelector* owner;
@property(strong) NSScreen* display;
@property(strong) NSButton* startButton;
@property NSPoint anchor;
@property NSRect initial;
@property NSInteger dragMode;
@end
@implementation SRSelectionView
- (BOOL)isFlipped { return YES; }
- (BOOL)acceptsFirstResponder { return YES; }
- (void)drawRect:(NSRect)dirty {
    BOOL active=self.owner.selected.screen==self.display;
    NSRect rect=active ? self.owner.selected.rect : NSZeroRect;
    NSBezierPath* shade=[NSBezierPath bezierPathWithRect:self.bounds];
    if (active) [shade appendBezierPathWithRect:rect];
    shade.windingRule=NSEvenOddWindingRule;
    [sr::Color(0x000000,0.42) setFill]; [shade fill];
    if (!active) return;
    [sr::Color(self.owner.recording ? 0xFF595E : 0x65B1DC) setStroke];
    NSBezierPath* outline=[NSBezierPath bezierPathWithRect:NSInsetRect(rect,1,1)];
    outline.lineWidth=2; [outline stroke];
    if (!self.owner.recording && self.owner.adjust) {
        [sr::Color(0xF4F4F2) setFill];
        for (int y=0;y<3;++y) for (int x=0;x<3;++x) {
            if (x==1 && y==1) continue;
            NSPoint p=NSMakePoint(NSMinX(rect)+rect.size.width*x/2,NSMinY(rect)+rect.size.height*y/2);
            [[NSBezierPath bezierPathWithRoundedRect:NSMakeRect(p.x-4,p.y-4,8,8) xRadius:2 yRadius:2] fill];
        }
    }
    if (!self.owner.recording) {
        NSString* dimensions=[NSString stringWithFormat:@"%.0f × %.0f   Esc 取消",rect.size.width*self.display.backingScaleFactor,
                                                        rect.size.height*self.display.backingScaleFactor];
        [dimensions drawAtPoint:NSMakePoint(rect.origin.x+6,MAX(8,rect.origin.y-24)) withAttributes:@{
            NSFontAttributeName:[NSFont monospacedDigitSystemFontOfSize:12 weight:NSFontWeightMedium],
            NSForegroundColorAttributeName:NSColor.whiteColor}];
    }
}
- (void)mouseDown:(NSEvent*)event {
    if (self.owner.recording) return;
    self.anchor=[self convertPoint:event.locationInWindow fromView:nil];
    self.dragMode=0;
    if (self.owner.adjust && self.owner.selected.screen==self.display) {
        NSRect rect=self.owner.selected.rect;
        // Eight handles have priority; every other interior point moves the region.
        for (int y=0;y<3;++y) for (int x=0;x<3;++x) {
            if (x==1 && y==1) continue;
            NSPoint p=NSMakePoint(NSMinX(rect)+rect.size.width*x/2,NSMinY(rect)+rect.size.height*y/2);
            if (hypot(p.x-self.anchor.x,p.y-self.anchor.y)<=10) self.dragMode=2+y*3+x;
        }
        if (!self.dragMode && NSPointInRect(self.anchor,rect)) self.dragMode=1;
    }
    if (!self.dragMode) {
        self.owner.selected=[SRRegionSelection new]; self.owner.selected.screen=self.display;
        self.owner.selected.rect=NSMakeRect(self.anchor.x,self.anchor.y,0,0);
    }
    self.initial=self.owner.selected.rect;
    [self.owner refresh];
}
- (void)mouseDragged:(NSEvent*)event {
    NSPoint point=[self convertPoint:event.locationInWindow fromView:nil];
    point.x=std::clamp(point.x,0.0,self.bounds.size.width);
    point.y=std::clamp(point.y,0.0,self.bounds.size.height);
    if (point.x<sr::SnapDistance) point.x=0;
    if (point.y<sr::SnapDistance) point.y=0;
    if (self.bounds.size.width-point.x<sr::SnapDistance) point.x=self.bounds.size.width;
    if (self.bounds.size.height-point.y<sr::SnapDistance) point.y=self.bounds.size.height;
    NSRect rect=self.initial;
    if (self.dragMode==1) {
        rect.origin.x=std::clamp(rect.origin.x+point.x-self.anchor.x,0.0,self.bounds.size.width-rect.size.width);
        rect.origin.y=std::clamp(rect.origin.y+point.y-self.anchor.y,0.0,self.bounds.size.height-rect.size.height);
    } else if (self.dragMode>=2) {
        int handle=(int)self.dragMode-2,x=handle%3,y=handle/3;
        double left=NSMinX(rect),right=NSMaxX(rect),top=NSMinY(rect),bottom=NSMaxY(rect);
        if (x==0) left=MIN(point.x,right-sr::MinimumSelection);
        if (x==2) right=MAX(point.x,left+sr::MinimumSelection);
        if (y==0) top=MIN(point.y,bottom-sr::MinimumSelection);
        if (y==2) bottom=MAX(point.y,top+sr::MinimumSelection);
        rect=NSMakeRect(left,top,right-left,bottom-top);
    } else {
        NSPoint start=self.anchor;
        if (start.x<sr::SnapDistance) start.x=0;
        if (start.y<sr::SnapDistance) start.y=0;
        if (self.bounds.size.width-start.x<sr::SnapDistance) start.x=self.bounds.size.width;
        if (self.bounds.size.height-start.y<sr::SnapDistance) start.y=self.bounds.size.height;
        rect=NSMakeRect(MIN(start.x,point.x),MIN(start.y,point.y),fabs(point.x-start.x),fabs(point.y-start.y));
    }
    self.owner.selected.rect=rect; [self.owner refresh];
}
- (void)mouseUp:(NSEvent*)event {
    if (self.owner.selected.rect.size.width<sr::MinimumSelection || self.owner.selected.rect.size.height<sr::MinimumSelection) return;
    if (!self.owner.adjust) [self.owner accept];
    else [self.owner refresh];
}
- (void)keyDown:(NSEvent*)event {
    if (event.keyCode==53) [self.owner cancel];
    else if (event.keyCode==36) [self.owner accept];
    else [super keyDown:event];
}
@end
@implementation SRRegionSelector
- (void)beginWithAdjustment:(BOOL)adjust completion:(void (^)(SRRegionSelection*))completion {
    self.adjust=adjust; self.completion=completion; self.windows=[NSMutableArray new];
    for (NSScreen* screen in NSScreen.screens) {
        NSWindow* window=[[SRSelectionWindow alloc] initWithContentRect:screen.frame styleMask:NSWindowStyleMaskBorderless
                                                               backing:NSBackingStoreBuffered defer:NO];
        window.opaque=NO; window.backgroundColor=NSColor.clearColor; window.hasShadow=NO;
        window.level=NSStatusWindowLevel+2; window.releasedWhenClosed=NO;
        window.collectionBehavior=NSWindowCollectionBehaviorCanJoinAllSpaces|NSWindowCollectionBehaviorFullScreenAuxiliary;
        SRSelectionView* view=[[SRSelectionView alloc] initWithFrame:NSMakeRect(0,0,screen.frame.size.width,screen.frame.size.height)];
        view.owner=self; view.display=screen;
        view.startButton=sr::Button(@"开始录制",@"record.circle",self,@selector(accept));
        view.startButton.hidden=YES; [view addSubview:view.startButton];
        window.contentView=view; [window makeFirstResponder:view]; [window orderFrontRegardless];
        [self.windows addObject:window];
        if (NSPointInRect(NSEvent.mouseLocation,screen.frame)) [window makeKeyWindow];
    }
}
- (void)refresh {
    for (NSWindow* window in self.windows) {
        SRSelectionView* view=(SRSelectionView*)window.contentView;
        BOOL active=self.selected.screen==view.display;
        view.startButton.hidden=!active || !self.adjust || self.recording || self.selected.rect.size.width<sr::MinimumSelection;
        NSRect rect=self.selected.rect;
        view.startButton.frame=NSMakeRect(std::clamp(NSMaxX(rect)-116,8.0,view.bounds.size.width-124),
             std::clamp(NSMaxY(rect)+8,8.0,view.bounds.size.height-42),116,32);
        view.needsDisplay=YES;
    }
}
- (void)accept {
    if (self.recording || self.selected.rect.size.width<sr::MinimumSelection || self.selected.rect.size.height<sr::MinimumSelection) return;
    void (^callback)(SRRegionSelection*)=self.completion; self.completion=nil;
    [self showRecordingMask]; if (callback) callback(self.selected);
}
- (void)showRecordingMask {
    self.recording=YES;
    for (NSWindow* window in self.windows) { window.ignoresMouseEvents=YES; [window resignKeyWindow]; }
    [self refresh];
}
- (void)cancel { void (^callback)(SRRegionSelection*)=self.completion; self.completion=nil; [self close]; if (callback) callback(nil); }
- (void)close { for (NSWindow* window in self.windows) [window close]; [self.windows removeAllObjects]; }
@end
