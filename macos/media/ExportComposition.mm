#import "media/ExportComposition.h"
#import "editor/AnnotationDrawing.h"
#import "common/Support.h"
#import <CoreImage/CoreImage.h>
#include "media/ExportQuality.h"
#include <mutex>
AVMutableComposition* SRComposition(SRExportRequest* request,NSError** error) {
    AVURLAsset* source=[AVURLAsset URLAssetWithURL:request.videoURL options:nil];
    AVAssetTrack* video=[source tracksWithMediaType:AVMediaTypeVideo].firstObject;
    double duration=CMTimeGetSeconds(source.duration);
    if (!video || !std::isfinite(duration) || request.start<0 || request.end<=request.start ||
        request.end>duration+0.05 || request.speed<sr::MinimumSpeed || request.speed>sr::MaximumSpeed) {
        *error=sr::Error(@"录制文件或裁剪范围无效，原始文件已保留。"); return nil;
    }
    CMTimeRange range=CMTimeRangeFromTimeToTime(sr::Time(request.start),sr::Time(MIN(duration,request.end)));
    AVMutableComposition* composition=[AVMutableComposition composition];
    AVMutableCompositionTrack* videoTrack=[composition addMutableTrackWithMediaType:AVMediaTypeVideo
                                                                  preferredTrackID:kCMPersistentTrackID_Invalid];
    if (![videoTrack insertTimeRange:range ofTrack:video atTime:kCMTimeZero error:error]) return nil;
    videoTrack.preferredTransform=video.preferredTransform;
    if (request.audio && request.audioURL) {
        AVURLAsset* audio=[AVURLAsset URLAssetWithURL:request.audioURL options:nil];
        AVAssetTrack* audioSource=[audio tracksWithMediaType:AVMediaTypeAudio].firstObject;
        if (audioSource) {
            CMTimeRange intersection=CMTimeRangeGetIntersection(audioSource.timeRange,range);
            if (CMTIMERANGE_IS_VALID(intersection) && CMTimeCompare(intersection.duration,kCMTimeZero)>0) {
                AVMutableCompositionTrack* audioTrack=[composition addMutableTrackWithMediaType:AVMediaTypeAudio
                                                                           preferredTrackID:kCMPersistentTrackID_Invalid];
                CMTime offset=CMTimeSubtract(intersection.start,range.start);
                if (![audioTrack insertTimeRange:intersection ofTrack:audioSource atTime:offset error:error]) return nil;
            }
        }
    }
    if (fabs(request.speed-1)>0.001)
        [composition scaleTimeRange:CMTimeRangeMake(kCMTimeZero,range.duration)
                         toDuration:CMTimeMultiplyByFloat64(range.duration,1.0/request.speed)];
    return composition;
}
@interface SRFrameRenderer : NSObject
- (instancetype)initWithRequest:(SRExportRequest*)request outputSize:(CGSize)size;
- (void)render:(AVAsynchronousCIImageFilteringRequest*)frame;
@end
@implementation SRFrameRenderer {
    SRExportRequest* _request;
    CGSize _size;
    CIContext* _context;
    CIImage* _overlay;
    std::vector<std::uint64_t> _active;
    std::mutex _mutex;
}
- (instancetype)initWithRequest:(SRExportRequest*)request outputSize:(CGSize)size {
    if ((self=[super init])) {
        _request=[request copy]; _size=size;
        _context=[CIContext contextWithOptions:@{kCIContextUseSoftwareRenderer:@NO,kCIContextCacheIntermediates:@NO}];
    }
    return self;
}
- (void)render:(AVAsynchronousCIImageFilteringRequest*)frame {
    @autoreleasepool {
        double sourceTime=_request.start+CMTimeGetSeconds(frame.compositionTime)*_request.speed;
        CIImage* overlay=nil;
        {
            std::scoped_lock lock(_mutex);
            std::vector<std::uint64_t> active;
            if (_request.annotations) for (const auto& mark:_request.annotations->Marks())
                if (qrec::annotations::Visible(mark,qrec::annotations::Time{(int64_t)llround(sourceTime*1000)})) active.push_back(mark.id);
            if (active!=_active) {
                _active=active; _overlay=nil;
                if (!active.empty()) {
                    CGImageRef image=sr::AnnotationImage(_request.annotations,sourceTime);
                    if (image) { _overlay=[CIImage imageWithCGImage:image]; CGImageRelease(image); }
                }
            }
            overlay=_overlay;
        }
        CIImage* image=frame.sourceImage;
        CGRect extent=image.extent;
        image=[image imageByApplyingTransform:CGAffineTransformMakeTranslation(-extent.origin.x,-extent.origin.y)];
        if (overlay) {
            overlay=[overlay imageByApplyingTransform:CGAffineTransformMakeScale(extent.size.width/overlay.extent.size.width,
                                                                                 extent.size.height/overlay.extent.size.height)];
            image=[overlay imageByCompositingOverImage:image];
        }
        image=[image imageByApplyingTransform:CGAffineTransformMakeScale(_size.width/extent.size.width,_size.height/extent.size.height)];
        [frame finishWithImage:[image imageByCroppingToRect:CGRectMake(0,0,_size.width,_size.height)] context:_context];
    }
}
@end
AVMutableVideoComposition* SRVideoComposition(AVAsset* asset,SRExportRequest* request,BOOL gif) {
    AVAssetTrack* video=[asset tracksWithMediaType:AVMediaTypeVideo].firstObject;
    CGSize source=video.naturalSize;
    auto size=gif ? qrec::media::ExportQuality::ComputeGifSize(source.width,source.height,(int)request.quality) :
                    qrec::media::ExportQuality::ComputeMp4Size(source.width,source.height,(int)request.quality);
    SRFrameRenderer* renderer=[[SRFrameRenderer alloc] initWithRequest:request outputSize:CGSizeMake(size.width,size.height)];
    AVMutableVideoComposition* composition=[AVMutableVideoComposition videoCompositionWithAsset:asset
        applyingCIFiltersWithHandler:^(AVAsynchronousCIImageFilteringRequest* frame) { [renderer render:frame]; }];
    composition.renderSize=CGSizeMake(size.width,size.height);
    // The CI factory inherits the source track clock. Disable it so speed edits do not
    // silently multiply FPS, and GIF consumes the entire interval rather than its prefix.
    composition.sourceTrackIDForFrameTiming=kCMPersistentTrackID_Invalid;
    composition.frameDuration=CMTimeMake(1,(int32_t)(gif?MIN(20,request.fps):request.fps));
    return composition;
}
