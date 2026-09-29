#import "media/ExportJob.h"
#import "media/ExportComposition.h"
#import "common/Support.h"
#import <CommonCrypto/CommonDigest.h>
@implementation SRExportRequest
- (instancetype)init { if ((self=[super init])) { _speed=1; _quality=100; _fps=60; } return self; }
- (id)copyWithZone:(NSZone*)zone {
    SRExportRequest* copy=[SRExportRequest new];
    copy.videoURL=self.videoURL; copy.audioURL=self.audioURL;
    copy.start=self.start; copy.end=self.end; copy.speed=self.speed;
    copy.quality=self.quality; copy.fps=self.fps; copy.audio=self.audio; copy.gif=self.gif; copy.annotations=self.annotations;
    return copy;
}
- (NSString*)cacheKey {
    NSString* identity=self.annotations?sr::String(self.annotations->Identity()):@"";
    NSString* value=[NSString stringWithFormat:@"mac-export-v2|%@|%@|%.6f|%.6f|%.3f|%ld|%ld|%d|%d|%@",
        self.videoURL.path,self.audioURL.path,self.start,self.end,self.speed,(long)self.quality,(long)self.fps,self.audio,self.gif,identity];
    NSData* data=[value dataUsingEncoding:NSUTF8StringEncoding]; unsigned char digest[CC_SHA256_DIGEST_LENGTH];
    CC_SHA256(data.bytes,(CC_LONG)data.length,digest);
    NSMutableString* key=[NSMutableString string]; for (unsigned char byte:digest) [key appendFormat:@"%02x",byte]; return key;
}
@end
@interface SRExportJob ()
@property(atomic, readwrite) BOOL cancelled;
@property(atomic,strong) AVAssetExportSession* session;
@end
@implementation SRExportJob
- (void)cancel { self.cancelled=YES; [self.session cancelExport]; }
- (void)run:(SRExportRequest*)request completion:(void (^)(NSURL*,NSError*))completion {
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED,0), ^{
        @autoreleasepool {
            if (self.cancelled) { completion(nil,nil); return; }
            AVURLAsset* asset=[AVURLAsset URLAssetWithURL:request.videoURL options:nil];
            double duration=CMTimeGetSeconds(asset.duration);
            BOOL marks=qrec::annotations::HasVisibleMarks(request.annotations,qrec::annotations::Time{(int64_t)(request.start*1000)},
                qrec::annotations::Time{(int64_t)ceil(request.end*1000)});
            BOOL sourceIsSilent=[asset tracksWithMediaType:AVMediaTypeAudio].count==0;
            BOOL untouched=sourceIsSilent && !request.gif && !marks && !request.audio && request.quality==100 && fabs(request.speed-1)<0.001 &&
                           request.start<0.0001 && fabs(request.end-duration)<0.03;
            if (untouched) { completion(request.videoURL,nil); return; }
            NSError* error=nil;
            AVMutableComposition* composition=SRComposition(request,&error);
            if (!composition) { completion(nil,error); return; }
            NSString* name=[[request cacheKey] stringByAppendingPathExtension:request.gif?@"gif":@"mp4"];
            NSURL* output=[request.videoURL.URLByDeletingLastPathComponent URLByAppendingPathComponent:name];
            if ([[NSFileManager defaultManager] fileExistsAtPath:output.path]) { completion(output,nil); return; }
            NSURL* partial=[output.URLByDeletingLastPathComponent URLByAppendingPathComponent:
                [NSString stringWithFormat:@"%@-%@.%@",request.cacheKey,NSUUID.UUID.UUIDString,request.gif?@"gif":@"mp4"]];
            void (^finished)(NSError*)=^(NSError* exportError) {
                if (self.cancelled || exportError) {
                    [[NSFileManager defaultManager] removeItemAtURL:partial error:nil]; completion(nil,exportError); return;
                }
                NSError* moveError=nil;
                if (![[NSFileManager defaultManager] moveItemAtURL:partial toURL:output error:&moveError]) completion(nil,moveError);
                else completion(output,nil);
            };
            if (request.gif) {
                if (!SRExportGIF(composition,request,partial,self,&error) && !self.cancelled && !error) error=sr::Error(@"GIF 生成失败。");
                finished(error); return;
            }
            BOOL render=marks || request.quality!=100 || fabs(request.speed-1)>0.001;
            if (render) {
                if (!SRTranscode(composition,request,partial,self,&error) && !self.cancelled && !error)
                    error=sr::Error(@"视频合成失败，原始文件已保留。");
                finished(error); return;
            }
            AVAssetExportSession* session=[[AVAssetExportSession alloc] initWithAsset:composition
                presetName:AVAssetExportPresetPassthrough];
            if (!session) { finished(sr::Error(@"此系统无法创建视频导出器。")); return; }
            session.outputURL=partial; session.outputFileType=AVFileTypeMPEG4;
            session.shouldOptimizeForNetworkUse=YES;
            session.audioTimePitchAlgorithm=AVAudioTimePitchAlgorithmVarispeed;
            self.session=session;
            if (self.cancelled) { finished(nil); return; }
            [session exportAsynchronouslyWithCompletionHandler:^{
                finished(session.status==AVAssetExportSessionStatusCompleted?nil:session.error ?: sr::Error(@"视频导出已取消或失败。"));
                self.session=nil;
            }];
        }
    });
}
@end
