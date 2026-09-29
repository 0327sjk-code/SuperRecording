#pragma once
// Debounces edits, cancels stale generations and publishes only the current cached artifact.
#import "media/ExportJob.h"
@interface SRExportCoordinator : NSObject
@property(copy) void (^changed)(NSURL*,NSError*,BOOL);
@property(readonly,strong) NSURL* readyURL;
- (void)prepare:(SRExportRequest*)request immediately:(BOOL)immediate;
- (void)cancel;
@end
