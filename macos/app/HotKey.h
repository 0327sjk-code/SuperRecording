#pragma once
// Carbon hot keys avoid requesting broad Accessibility/input-monitoring privileges.
#import <Foundation/Foundation.h>
@interface SRHotKey : NSObject
@property(copy) void (^pressed)(void);
- (BOOL)registerCode:(UInt32)code modifiers:(UInt32)modifiers;
@end
