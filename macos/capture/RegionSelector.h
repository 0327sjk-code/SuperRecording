#pragma once
// Multi-display selection overlays. Rectangles use display-local top-left point coordinates.
#import <AppKit/AppKit.h>
@interface SRRegionSelection : NSObject
@property(strong) NSScreen* screen;
@property NSRect rect;
@property(readonly) CGDirectDisplayID displayID;
@end
@interface SRRegionSelector : NSObject
- (void)beginWithAdjustment:(BOOL)adjust completion:(void (^)(SRRegionSelection*))completion;
- (void)showRecordingMask;
- (void)close;
@end
