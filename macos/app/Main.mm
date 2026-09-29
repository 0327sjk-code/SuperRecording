// Native entry point; self-tests never request capture permissions or modify the user's desktop.
#import "app/AppDelegate.h"
#import "tests/SelfTest.h"
#import <AppKit/AppKit.h>
int main(int argc,const char* argv[]) {
    @autoreleasepool {
        [NSApplication sharedApplication];
        if (argc>1 && strcmp(argv[1],"--self-test")==0) return SRCoreTests();
        if (argc>2 && strcmp(argv[1],"--media-test")==0)
            return SRMediaTests([NSURL fileURLWithPath:[NSString stringWithUTF8String:argv[2]] isDirectory:YES]);
        for (NSRunningApplication* app in [NSRunningApplication runningApplicationsWithBundleIdentifier:NSBundle.mainBundle.bundleIdentifier])
            if (app.processIdentifier!=NSProcessInfo.processInfo.processIdentifier) { [app activateWithOptions:0]; return 0; }
        [NSApp setActivationPolicy:NSApplicationActivationPolicyAccessory];
        SRAppDelegate* delegate=[SRAppDelegate new]; NSApp.delegate=delegate; [NSApp run];
    }
    return 0;
}
