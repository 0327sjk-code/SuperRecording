#import "capture/RecordingHUD.h"
#import "common/Support.h"
@implementation SRRecordingHUD {
    NSTextField* _time;
    NSButton* _pause;
    NSButton* _stop;
}
- (instancetype)initWithSelection:(SRRegionSelection*)selection {
    NSRect screen=selection.screen.frame, rect=selection.rect;
    NSRect frame=NSMakeRect(NSMinX(screen)+NSMaxX(rect)-260,NSMaxY(screen)-NSMaxY(rect)-48,260,40);
    frame.origin.x=MAX(NSMinX(screen)+8,MIN(frame.origin.x,NSMaxX(screen)-268));
    frame.origin.y=MAX(NSMinY(screen)+8,MIN(frame.origin.y,NSMaxY(screen)-48));
    NSPanel* panel=[[NSPanel alloc] initWithContentRect:frame
        styleMask:NSWindowStyleMaskBorderless|NSWindowStyleMaskNonactivatingPanel backing:NSBackingStoreBuffered defer:NO];
    if ((self=[super initWithWindow:panel])) {
        panel.level=NSStatusWindowLevel+3; panel.opaque=NO; panel.backgroundColor=NSColor.clearColor;
        panel.movableByWindowBackground=YES; panel.hidesOnDeactivate=NO; panel.hasShadow=NO;
        panel.collectionBehavior=NSWindowCollectionBehaviorCanJoinAllSpaces|NSWindowCollectionBehaviorFullScreenAuxiliary;
        panel.appearance=[NSAppearance appearanceNamed:NSAppearanceNameDarkAqua];
        sr::Surface(panel.contentView,0x191B1E,10);
        NSTextField* dot=sr::Label(@"●",12); dot.textColor=sr::Color(0xFF595E); dot.frame=NSMakeRect(12,12,16,18);
        _time=sr::Label(@"00:00.00",12); _time.frame=NSMakeRect(32,12,76,18);
        _time.font=[NSFont monospacedDigitSystemFontOfSize:12 weight:NSFontWeightRegular];
        _pause=sr::Button(@"暂停",@"pause.fill",self,@selector(togglePause)); _pause.frame=NSMakeRect(113,6,66,28);
        _stop=sr::Button(@"结束",@"stop.fill",self,@selector(stop)); _stop.frame=NSMakeRect(184,6,66,28);
        for (NSView* view in @[dot,_time,_pause,_stop]) [panel.contentView addSubview:view];
    }
    return self;
}
- (void)togglePause { if (self.pauseAction) self.pauseAction(); }
- (void)stop { if (self.stopAction) self.stopAction(); }
- (void)updateTime:(double)seconds paused:(BOOL)paused {
    _time.stringValue=sr::TimeLabel(seconds); _pause.title=paused?@"继续":@"暂停";
    _pause.image=[NSImage imageWithSystemSymbolName:paused?@"play.fill":@"pause.fill" accessibilityDescription:_pause.title];
}
- (void)setFinishing { _pause.enabled=NO; _stop.enabled=NO; _time.stringValue=@"正在完成"; }
@end
