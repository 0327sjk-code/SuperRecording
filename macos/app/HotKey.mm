#import "app/HotKey.h"
#import <Carbon/Carbon.h>
@interface SRHotKey ()
- (void)trigger;
@end
@implementation SRHotKey {
    EventHotKeyRef _hotKey;
    EventHandlerRef _handler;
    UInt32 _code;
    UInt32 _modifiers;
}
static OSStatus HotKeyEvent(EventHandlerCallRef next,EventRef event,void* context) {
    [(__bridge SRHotKey*)context trigger]; return noErr;
}
- (instancetype)init {
    if ((self=[super init])) {
        EventTypeSpec type={kEventClassKeyboard,kEventHotKeyPressed};
        InstallApplicationEventHandler(&HotKeyEvent,1,&type,(__bridge void*)self,&_handler);
    }
    return self;
}
- (BOOL)registerCode:(UInt32)code modifiers:(UInt32)modifiers {
    if (_hotKey && _code==code && _modifiers==modifiers) return YES;
    EventHotKeyRef replacement=nullptr;
    constexpr UInt32 signature=0x53525243; // SRRC
    EventHotKeyID identity={signature,1};
    OSStatus status=RegisterEventHotKey(code,modifiers,identity,GetApplicationEventTarget(),0,&replacement);
    if (status!=noErr) return NO;
    if (_hotKey) UnregisterEventHotKey(_hotKey);
    _hotKey=replacement; _code=code; _modifiers=modifiers; return YES;
}
- (void)trigger { if (self.pressed) self.pressed(); }
- (void)dealloc { if (_hotKey) UnregisterEventHotKey(_hotKey); if (_handler) RemoveEventHandler(_handler); }
@end
