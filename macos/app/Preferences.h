#pragma once
// User defaults are intentionally independent of the Windows settings.ini.
#import <Foundation/Foundation.h>
@interface SRPreferences : NSObject
@property NSInteger fps;
@property NSInteger quality;
@property BOOL adjustSelection;
@property BOOL keepEditor;
@property NSInteger hotKeyCode;
@property NSUInteger hotKeyModifiers;
@property(copy) NSString* hotKeyLabel;
@property(copy) NSURL* saveDirectory;
@end
