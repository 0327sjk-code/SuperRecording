// Native entry point; self-tests never request capture permissions or modify the user's desktop.
#import "app/AppDelegate.h"
#import "tests/SelfTest.h"
#import <AppKit/AppKit.h>
#include <cstdio>
#include <cstdlib>
int main(int argc,const char* argv[]) {
    @autoreleasepool {
        [NSApplication sharedApplication];
        if (argc>1 && strcmp(argv[1],"--self-test")==0) return SRCoreTests();
        if (argc>2 && strcmp(argv[1],"--media-test")==0)
            return SRMediaTests([NSURL fileURLWithPath:[NSString stringWithUTF8String:argv[2]] isDirectory:YES]);
        BOOL appTest=argc>1 && strcmp(argv[1],"--app-test")==0;
        if (appTest) [NSUserDefaults.standardUserDefaults setVolatileDomain:@{@"firstLaunchDone":@YES} forName:NSArgumentDomain];
        for (NSRunningApplication* app in [NSRunningApplication runningApplicationsWithBundleIdentifier:NSBundle.mainBundle.bundleIdentifier])
            if (app.processIdentifier!=NSProcessInfo.processInfo.processIdentifier) { [app activateWithOptions:0]; return 0; }
        [NSApp setActivationPolicy:NSApplicationActivationPolicyAccessory];
        // NSApplication.delegate is non-owning. Keep it alive for the entire native event loop.
        __attribute__((objc_precise_lifetime)) SRAppDelegate* delegate=[SRAppDelegate new]; NSApp.delegate=delegate;
        if (appTest) [NSTimer scheduledTimerWithTimeInterval:1 repeats:NO block:^(NSTimer* timer) {
            BOOL ready=NSApp.delegate && [(SRAppDelegate*)NSApp.delegate isMenuBarReady];
            std::printf("%s native menu-bar app startup and delegate lifetime\n",ready?"PASS":"FAIL");
            if (!ready) std::exit(1);
            [NSApp terminate:nil];
        }];
        [NSApp run];
    }
    return 0;
}
