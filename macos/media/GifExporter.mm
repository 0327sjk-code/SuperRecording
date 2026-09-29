#import "media/ExportComposition.h"
#import "common/Support.h"
#import <ImageIO/ImageIO.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>
#import <VideoToolbox/VideoToolbox.h>
BOOL SRExportGIF(AVAsset* asset,SRExportRequest* request,NSURL* output,SRExportJob* job,NSError** error) {
    AVAssetReader* reader=[[AVAssetReader alloc] initWithAsset:asset error:error];
    if (!reader) return NO;
    AVAssetReaderVideoCompositionOutput* frames=[AVAssetReaderVideoCompositionOutput
        assetReaderVideoCompositionOutputWithVideoTracks:[asset tracksWithMediaType:AVMediaTypeVideo]
        videoSettings:@{(__bridge NSString*)kCVPixelBufferPixelFormatTypeKey:@(kCVPixelFormatType_32BGRA)}];
    frames.videoComposition=SRVideoComposition(asset,request,YES); frames.alwaysCopiesSampleData=NO;
    if (![reader canAddOutput:frames]) { *error=sr::Error(@"无法读取 GIF 视频帧。"); return NO; }
    [reader addOutput:frames]; if (![reader startReading]) { *error=reader.error; return NO; }
    double duration=CMTimeGetSeconds(asset.duration), fps=MIN(20,request.fps);
    size_t count=(size_t)ceil(duration*fps-0.0001);
    if (!count) { *error=sr::Error(@"GIF 时间范围为空。"); return NO; }
    CGImageDestinationRef destination=CGImageDestinationCreateWithURL((__bridge CFURLRef)output,
        (__bridge CFStringRef)UTTypeGIF.identifier,count,nullptr);
    if (!destination) { *error=sr::Error(@"无法创建 GIF 文件。"); return NO; }
    CGImageDestinationSetProperties(destination,(__bridge CFDictionaryRef)@{
        (__bridge NSString*)kCGImagePropertyGIFDictionary:@{(__bridge NSString*)kCGImagePropertyGIFLoopCount:@0}});
    size_t written=0;
    while (!job.cancelled && written<count) {
        @autoreleasepool {
            CMSampleBufferRef sample=[frames copyNextSampleBuffer];
            if (!sample) break;
            CGImageRef image=nullptr;
            OSStatus status=VTCreateCGImageFromCVPixelBuffer(CMSampleBufferGetImageBuffer(sample),nullptr,&image);
            if (status==noErr && image) {
                double delay=MAX(0.02,MIN(1.0/fps,duration-written/fps));
                CGImageDestinationAddImage(destination,image,(__bridge CFDictionaryRef)@{
                    (__bridge NSString*)kCGImagePropertyGIFDictionary:@{
                        (__bridge NSString*)kCGImagePropertyGIFDelayTime:@(delay),
                        (__bridge NSString*)kCGImagePropertyGIFUnclampedDelayTime:@(delay)}});
                CGImageRelease(image); ++written;
            }
            CFRelease(sample);
        }
    }
    BOOL ok=!job.cancelled && written==count && CGImageDestinationFinalize(destination);
    CFRelease(destination); [reader cancelReading];
    if (!ok && !job.cancelled) *error=reader.error ?: sr::Error(@"GIF 帧数不完整，原始录制已保留。");
    return ok;
}
