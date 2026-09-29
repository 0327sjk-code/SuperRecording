#import "media/ExportCoordinator.h"
#import "common/Support.h"
@implementation SRExportCoordinator {
    SRExportJob* _job;
    NSUInteger _generation;
    NSString* _key;
    NSMutableDictionary<NSString*,NSURL*>* _cache;
    NSURL* _readyURL;
}
- (instancetype)init { if ((self=[super init])) _cache=[NSMutableDictionary new]; return self; }
- (NSURL*)readyURL { return _readyURL; }
- (void)prepare:(SRExportRequest*)request immediately:(BOOL)immediate {
    NSAssert(NSThread.isMainThread,@"Editor export state belongs to the main thread");
    NSString* key=request.cacheKey;
    if ([key isEqualToString:_key]) return;
    [_job cancel]; _job=nil; NSUInteger generation=++_generation; _key=key; _readyURL=_cache[key];
    if (_readyURL) { if (self.changed) self.changed(_readyURL,nil,NO); return; }
    if (self.changed) self.changed(nil,nil,YES);
    SRExportRequest* snapshot=[request copy]; __weak SRExportCoordinator* weakSelf=self;
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW,(int64_t)((immediate?0:sr::ExportDebounce)*NSEC_PER_SEC)),dispatch_get_main_queue(), ^{
        SRExportCoordinator* owner=weakSelf; if (!owner || owner->_generation!=generation) return;
        SRExportJob* job=[SRExportJob new]; owner->_job=job;
        [job run:snapshot completion:^(NSURL* url,NSError* error) {
            dispatch_async(dispatch_get_main_queue(),^{
                SRExportCoordinator* current=weakSelf;
                if (!current || current->_generation!=generation) return;
                current->_job=nil;
                if (url) { current->_cache[key]=url; current->_readyURL=url; }
                else if (error) current->_key=nil;
                if (current.changed) current.changed(url,error,NO);
            });
        }];
    });
}
- (void)cancel { ++_generation; [_job cancel]; _job=nil; _key=nil; self.changed=nil; }
- (void)dealloc { [_job cancel]; }
@end
