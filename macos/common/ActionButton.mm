#import "common/ActionButton.h"
#import "common/Support.h"
@implementation SRActionButton {
    NSTrackingArea* _tracking;
    NSTimer* _hoverTimer;
    double _hover;
}
- (BOOL)isFlipped { return YES; }
- (void)updateTrackingAreas {
    [super updateTrackingAreas];
    if (_tracking) [self removeTrackingArea:_tracking];
    _tracking=[[NSTrackingArea alloc] initWithRect:NSZeroRect
        options:NSTrackingMouseEnteredAndExited|NSTrackingActiveAlways|NSTrackingInVisibleRect owner:self userInfo:nil];
    [self addTrackingArea:_tracking];
}
- (void)mouseEntered:(NSEvent*)event { [self hoverTo:1]; }
- (void)mouseExited:(NSEvent*)event { [self hoverTo:0]; }
- (void)hoverTo:(double)target {
    [_hoverTimer invalidate]; _hoverTimer=nil;
    if (NSWorkspace.sharedWorkspace.accessibilityDisplayShouldReduceMotion) { _hover=target; self.needsDisplay=YES; return; }
    double start=_hover,time=NSProcessInfo.processInfo.systemUptime; __weak SRActionButton* weakSelf=self;
    _hoverTimer=[NSTimer scheduledTimerWithTimeInterval:1.0/60 repeats:YES block:^(NSTimer* timer) {
        SRActionButton* button=weakSelf;
        if (!button) { [timer invalidate]; return; }
        double fraction=MIN(1,(NSProcessInfo.processInfo.systemUptime-time)/0.14);
        double ease=1-pow(1-fraction,3); button->_hover=start+(target-start)*ease; button.needsDisplay=YES;
        if (fraction>=1) { [timer invalidate]; button->_hoverTimer=nil; }
    }];
}
- (void)drawRect:(NSRect)dirty {
    BOOL selected=self.state==NSControlStateValueOn,pressed=self.cell.highlighted;
    NSColor* base=self.bezelColor ?: sr::Color(selected?0x303A46:0x24272B);
    NSColor* hover=self.bezelColor ? [base blendedColorWithFraction:0.12 ofColor:NSColor.whiteColor] : sr::Color(0x353B42);
    NSColor* fill=[base blendedColorWithFraction:_hover ofColor:hover];
    if (pressed) fill=[base blendedColorWithFraction:0.2 ofColor:NSColor.blackColor];
    if (!self.enabled) fill=sr::Color(0x1E2023);
    NSColor* ink=sr::Color(self.enabled?0xF4F4F2:0x74787E);
    NSRect rect=NSInsetRect(self.bounds,0.5,0.5);
    NSBezierPath* path=[NSBezierPath bezierPathWithRoundedRect:rect xRadius:6 yRadius:6];
    [fill setFill]; [path fill];
    [sr::Color(selected?0x788697:0x373B40) setStroke]; path.lineWidth=1; [path stroke];
    CGFloat iconSize=self.bounds.size.width<30?12:15;
    NSDictionary* attributes=@{NSFontAttributeName:self.font ?: [NSFont systemFontOfSize:12],NSForegroundColorAttributeName:ink};
    NSSize textSize=[self.title sizeWithAttributes:attributes];
    BOOL hasText=self.title.length>0,hasIcon=self.image!=nil;
    CGFloat total=(hasText?textSize.width:0)+(hasIcon?iconSize:0)+(hasText&&hasIcon?6:0);
    CGFloat x=MAX(3,(self.bounds.size.width-total)/2);
    if (hasIcon) {
        NSImage* image=[self.image imageWithSymbolConfiguration:[NSImageSymbolConfiguration configurationWithPaletteColors:@[ink]]] ?: self.image;
        [image drawInRect:NSMakeRect(x,(self.bounds.size.height-iconSize)/2,iconSize,iconSize) fromRect:NSZeroRect
              operation:NSCompositingOperationSourceOver fraction:1 respectFlipped:YES hints:nil];
        x+=iconSize+6;
    }
    if (hasText) [self.title drawAtPoint:NSMakePoint(x,(self.bounds.size.height-textSize.height)/2) withAttributes:attributes];
    if (self.window.firstResponder==self && self.enabled) {
        [sr::Color(0x65B1DC) setStroke];
        NSBezierPath* focus=[NSBezierPath bezierPathWithRoundedRect:NSInsetRect(self.bounds,2,2) xRadius:4 yRadius:4]; [focus stroke];
    }
}
- (void)dealloc { [_hoverTimer invalidate]; }
@end
