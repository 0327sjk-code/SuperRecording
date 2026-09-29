#pragma once
// Native macOS editor entry point. Windows editor sources are not compiled into this target.
#import <AppKit/AppKit.h>
#import "capture/Recorder.h"
#import "app/Preferences.h"
@interface SREditorController : NSWindowController <NSWindowDelegate>
@property(copy) void (^closed)(void);
- (instancetype)initWithRecording:(SRRecording*)recording preferences:(SRPreferences*)preferences;
- (void)writeUISnapshot:(NSURL*)url;
@end
