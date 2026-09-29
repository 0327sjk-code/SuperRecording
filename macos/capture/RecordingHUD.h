#pragma once
// Compact non-activating recording controls, excluded by ScreenCaptureKit's application filter.
#import <AppKit/AppKit.h>
#import "capture/RegionSelector.h"
@interface SRRecordingHUD : NSWindowController
@property(copy) void (^pauseAction)(void);
@property(copy) void (^stopAction)(void);
- (instancetype)initWithSelection:(SRRegionSelection*)selection;
- (void)updateTime:(double)seconds paused:(BOOL)paused;
- (void)setFinishing;
@end
