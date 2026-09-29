#pragma once
// Preview-channel discovery is separate from Windows' stable release and executable assets.
#import <Foundation/Foundation.h>
@interface SRUpdateChecker : NSObject
- (void)check;
@end
