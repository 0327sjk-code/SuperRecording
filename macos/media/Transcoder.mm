// Bounded reader/writer pump: explicit bitrate policy, hardware encoding, and cancellable rendering.
#import "media/ExportComposition.h"
#import "common/Support.h"
#import <VideoToolbox/VideoToolbox.h>
#import <AudioToolbox/AudioToolbox.h>
#include "media/ExportQuality.h"
#include <atomic>
#include <thread>
namespace {
BOOL PumpTrack(AVAssetReaderOutput* output,AVAssetWriterInput* input,BOOL& finished,NSError** error) {
    if (finished || !input.readyForMoreMediaData) return NO;
    CMSampleBufferRef sample=[output copyNextSampleBuffer];
    if (!sample) { [input markAsFinished]; finished=YES; return YES; }
    BOOL appended=[input appendSampleBuffer:sample]; CFRelease(sample);
    if (!appended) *error=sr::Error(@"视频编码器无法写入数据，原始录制已保留。");
    return YES;
}
}
BOOL SRTranscode(AVAsset* asset,SRExportRequest* request,NSURL* output,SRExportJob* job,NSError** error) {
    AVAssetReader* reader=[[AVAssetReader alloc] initWithAsset:asset error:error]; if (!reader) return NO;
    AVMutableVideoComposition* composition=SRVideoComposition(asset,request,NO);
    CGSize size=composition.renderSize;
    AVAssetReaderVideoCompositionOutput* frames=[AVAssetReaderVideoCompositionOutput
        assetReaderVideoCompositionOutputWithVideoTracks:[asset tracksWithMediaType:AVMediaTypeVideo]
        videoSettings:@{(__bridge NSString*)kCVPixelBufferPixelFormatTypeKey:@(kCVPixelFormatType_32BGRA)}];
    frames.videoComposition=composition; frames.alwaysCopiesSampleData=NO;
    if (![reader canAddOutput:frames]) { *error=sr::Error(@"无法读取待合成的画面。"); return NO; } [reader addOutput:frames];
    AVAssetWriter* writer=[[AVAssetWriter alloc] initWithURL:output fileType:AVFileTypeMPEG4 error:error]; if (!writer) return NO;
    writer.shouldOptimizeForNetworkUse=YES;
    uint32_t bitrate=qrec::media::ExportQuality::ComputeVideoBitrate(size.width,size.height,(int)request.fps,(int)request.quality);
    AVAssetWriterInput* video=[AVAssetWriterInput assetWriterInputWithMediaType:AVMediaTypeVideo outputSettings:@{
        AVVideoCodecKey:AVVideoCodecTypeH264,AVVideoWidthKey:@(size.width),AVVideoHeightKey:@(size.height),
        AVVideoCompressionPropertiesKey:@{AVVideoAverageBitRateKey:@(bitrate),
            AVVideoProfileLevelKey:AVVideoProfileLevelH264HighAutoLevel,AVVideoExpectedSourceFrameRateKey:@(request.fps),
            AVVideoMaxKeyFrameIntervalKey:@(request.fps),AVVideoAllowFrameReorderingKey:@YES},
        AVVideoEncoderSpecificationKey:@{(__bridge NSString*)kVTVideoEncoderSpecification_EnableHardwareAcceleratedVideoEncoder:@YES}}];
    if (![writer canAddInput:video]) { *error=sr::Error(@"当前 Mac 不支持此输出分辨率。"); return NO; } [writer addInput:video];
    NSArray* audioTracks=[asset tracksWithMediaType:AVMediaTypeAudio];
    AVAssetReaderAudioMixOutput* audioFrames=nil; AVAssetWriterInput* audio=nil;
    if (request.audio && audioTracks.count) {
        audioFrames=[AVAssetReaderAudioMixOutput assetReaderAudioMixOutputWithAudioTracks:audioTracks audioSettings:@{
            AVFormatIDKey:@(kAudioFormatLinearPCM),AVSampleRateKey:@48000,AVNumberOfChannelsKey:@2,
            AVLinearPCMBitDepthKey:@32,AVLinearPCMIsFloatKey:@YES,AVLinearPCMIsNonInterleaved:@NO}];
        audioFrames.audioTimePitchAlgorithm=AVAudioTimePitchAlgorithmVarispeed; audioFrames.alwaysCopiesSampleData=NO;
        if (![reader canAddOutput:audioFrames]) { *error=sr::Error(@"无法读取待合成的声音。"); return NO; } [reader addOutput:audioFrames];
        audio=[AVAssetWriterInput assetWriterInputWithMediaType:AVMediaTypeAudio outputSettings:@{
            AVFormatIDKey:@(kAudioFormatMPEG4AAC),AVSampleRateKey:@48000,AVNumberOfChannelsKey:@2,AVEncoderBitRateKey:@192000}];
        if (![writer canAddInput:audio]) { *error=sr::Error(@"声音输出编码器不可用。"); return NO; } [writer addInput:audio];
    }
    if (![writer startWriting]) { *error=writer.error; return NO; }
    [writer startSessionAtSourceTime:kCMTimeZero];
    if (![reader startReading]) { *error=reader.error; [writer cancelWriting]; return NO; }
    BOOL videoFinished=NO,audioFinished=audio==nil;
    double lastProgress=NSProcessInfo.processInfo.systemUptime;
    constexpr double stallTimeoutSeconds=30;
    while ((!videoFinished || !audioFinished) && !job.cancelled && !*error) {
        @autoreleasepool {
            BOOL progress=PumpTrack(frames,video,videoFinished,error);
            if (audio) progress=PumpTrack(audioFrames,audio,audioFinished,error)||progress;
            if (writer.status==AVAssetWriterStatusFailed || reader.status==AVAssetReaderStatusFailed) {
                *error=writer.error ?: reader.error; break;
            }
            if (progress) lastProgress=NSProcessInfo.processInfo.systemUptime;
            else if (NSProcessInfo.processInfo.systemUptime-lastProgress>stallTimeoutSeconds) {
                *error=sr::Error(@"编码器长时间没有响应，原始文件已保留。"); break;
            } else std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }
    if (job.cancelled || *error) { [reader cancelReading]; [writer cancelWriting]; return NO; }
    [writer endSessionAtSourceTime:asset.duration];
    auto done=std::make_shared<std::atomic_bool>(false);
    [writer finishWritingWithCompletionHandler:^{ done->store(true); }];
    double deadline=NSProcessInfo.processInfo.systemUptime+stallTimeoutSeconds;
    while (!done->load() && !job.cancelled && NSProcessInfo.processInfo.systemUptime<deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    if (!done->load() || job.cancelled) {
        [writer cancelWriting]; if (!job.cancelled) *error=sr::Error(@"视频封装超时，请重试。"); return NO;
    }
    if (writer.status!=AVAssetWriterStatusCompleted) { *error=writer.error ?: sr::Error(@"视频封装失败。"); return NO; }
    return YES;
}
