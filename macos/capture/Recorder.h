#pragma once
// ScreenCaptureKit -> separate real-time H.264/AAC files, using a shared pause-safe clock.
#import <Foundation/Foundation.h>
#import "capture/RegionSelector.h"
@interface SRRecording : NSObject
@property(strong) NSURL* videoURL;
@property(strong) NSURL* audioURL;
@property CGSize size;
@property NSInteger fps;
@end
@interface SRRecorder : NSObject
@property(atomic, readonly) double recordedSeconds;
@property(atomic, readonly) BOOL paused;
@property(copy) void (^failure)(NSError*);
- (void)start:(SRRegionSelection*)selection fps:(NSInteger)fps completion:(void (^)(NSError*))completion;
- (void)togglePause;
- (void)stop:(void (^)(SRRecording*,NSError*))completion;
@end
