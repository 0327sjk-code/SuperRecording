#import "app/Preferences.h"
#import <Carbon/Carbon.h>
@implementation SRPreferences
- (instancetype)init {
    if ((self=[super init])) {
        [NSUserDefaults.standardUserDefaults registerDefaults:@{
            @"fps":@60,@"quality":@100,@"adjustSelection":@NO,@"keepEditor":@YES,
            @"hotKeyCode":@(kVK_F3),@"hotKeyModifiers":@0,@"hotKeyLabel":@"F3"}];
    }
    return self;
}
#define SR_INTEGER(Name, Setter) \
- (NSInteger)Name { return [NSUserDefaults.standardUserDefaults integerForKey:@#Name]; } \
- (void)Setter:(NSInteger)value { [NSUserDefaults.standardUserDefaults setInteger:value forKey:@#Name]; }
SR_INTEGER(fps, setFps)
SR_INTEGER(quality, setQuality)
SR_INTEGER(hotKeyCode, setHotKeyCode)
#undef SR_INTEGER
- (NSUInteger)hotKeyModifiers { return [NSUserDefaults.standardUserDefaults integerForKey:@"hotKeyModifiers"]; }
- (void)setHotKeyModifiers:(NSUInteger)value { [NSUserDefaults.standardUserDefaults setInteger:value forKey:@"hotKeyModifiers"]; }
- (BOOL)adjustSelection { return [NSUserDefaults.standardUserDefaults boolForKey:@"adjustSelection"]; }
- (void)setAdjustSelection:(BOOL)value { [NSUserDefaults.standardUserDefaults setBool:value forKey:@"adjustSelection"]; }
- (BOOL)keepEditor { return [NSUserDefaults.standardUserDefaults boolForKey:@"keepEditor"]; }
- (void)setKeepEditor:(BOOL)value { [NSUserDefaults.standardUserDefaults setBool:value forKey:@"keepEditor"]; }
- (NSString*)hotKeyLabel { return [NSUserDefaults.standardUserDefaults stringForKey:@"hotKeyLabel"] ?: @"F3"; }
- (void)setHotKeyLabel:(NSString*)value { [NSUserDefaults.standardUserDefaults setObject:value forKey:@"hotKeyLabel"]; }
- (NSURL*)saveDirectory {
    NSString* path=[NSUserDefaults.standardUserDefaults stringForKey:@"saveDirectory"];
    return path.length ? [NSURL fileURLWithPath:path isDirectory:YES] :
        [[NSFileManager.defaultManager URLsForDirectory:NSMoviesDirectory inDomains:NSUserDomainMask].firstObject
            URLByAppendingPathComponent:@"SuperRecording" isDirectory:YES];
}
- (void)setSaveDirectory:(NSURL*)url { [NSUserDefaults.standardUserDefaults setObject:url.path forKey:@"saveDirectory"]; }
@end
