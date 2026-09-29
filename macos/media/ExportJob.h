#pragma once
// Immutable edit request and cancellable asynchronous export operation.
#import <AVFoundation/AVFoundation.h>
#include "annotations/Annotation.h"
@interface SRExportRequest : NSObject <NSCopying>
@property(strong) NSURL* videoURL;
@property(strong) NSURL* audioURL;
@property double start;
@property double end;
@property double speed;
@property NSInteger quality;
@property NSInteger fps;
@property BOOL audio;
@property BOOL gif;
@property(nonatomic) qrec::annotations::Snapshot annotations;
- (NSString*)cacheKey;
@end
@interface SRExportJob : NSObject
@property(atomic, readonly) BOOL cancelled;
- (void)run:(SRExportRequest*)request completion:(void (^)(NSURL*,NSError*))completion;
- (void)cancel;
@end
