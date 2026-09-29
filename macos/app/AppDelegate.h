#pragma once
// Menu-bar application lifecycle, preferences, permission flow and recording/editor coordination.
#import <AppKit/AppKit.h>
@interface SRAppDelegate : NSObject <NSApplicationDelegate,NSMenuDelegate>
- (BOOL)isMenuBarReady;
@end
