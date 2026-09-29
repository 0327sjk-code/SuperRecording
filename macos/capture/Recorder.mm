#import "capture/Recorder.h"
#import "common/Support.h"
#import <ScreenCaptureKit/ScreenCaptureKit.h>
#import <VideoToolbox/VideoToolbox.h>
#import <AudioToolbox/AudioToolbox.h>
#include "core/CaptureClock.h"
#include "media/ExportQuality.h"
#include <vector>
#include <deque>
@implementation SRRecording @end
@interface SRRecorder () <SCStreamOutput,SCStreamDelegate>
@property(atomic, readwrite) double recordedSeconds;
@property(atomic, readwrite) BOOL paused;
@end
@implementation SRRecorder {
    SCStream* _stream;
    AVAssetWriter* _videoWriter;
    AVAssetWriter* _audioWriter;
    AVAssetWriterInput* _videoInput;
    AVAssetWriterInput* _audioInput;
    AVAssetWriterInputPixelBufferAdaptor* _adaptor;
    dispatch_queue_t _queue;
    dispatch_source_t _timer;
    CVPixelBufferRef _latestFrame;
    sr::CaptureClock _clock;
    SRRecording* _recording;
    int64_t _lastFrameIndex;
    double _lastAudioEnd;
    double _finishDeadline;
    std::deque<CMSampleBufferRef> _audioPending;
    BOOL _stopping;
    NSError* _fatalError;
}
- (instancetype)init {
    if ((self=[super init])) { _queue=dispatch_queue_create("com.superrecording.capture",DISPATCH_QUEUE_SERIAL); _lastFrameIndex=-1; }
    return self;
}
static double HostNow() { return CMTimeGetSeconds(CMClockGetTime(CMClockGetHostTimeClock())); }
- (void)start:(SRRegionSelection*)selection fps:(NSInteger)fps completion:(void (^)(NSError*))completion {
    _recording=[SRRecording new]; _recording.fps=fps==30?30:60;
    CGFloat scale=selection.screen.backingScaleFactor;
    _recording.size=CGSizeMake(MAX(16,((int)llround(selection.rect.size.width*scale))&~1),
                              MAX(16,((int)llround(selection.rect.size.height*scale))&~1));
    NSURL* folder=sr::UniqueDirectory(@"recording");
    if (!folder) { completion(sr::Error(@"无法创建录制缓存目录。")); return; }
    _recording.videoURL=[folder URLByAppendingPathComponent:@"original.mp4"];
    _recording.audioURL=[folder URLByAppendingPathComponent:@"system-audio.m4a"];
    [SCShareableContent getShareableContentExcludingDesktopWindows:YES onScreenWindowsOnly:YES
        completionHandler:^(SCShareableContent* content,NSError* error) {
        if (error) { dispatch_async(dispatch_get_main_queue(),^{ completion(error); }); return; }
        SCDisplay* display=nil; NSMutableArray* excluded=[NSMutableArray new];
        for (SCDisplay* candidate in content.displays) if (candidate.displayID==selection.displayID) display=candidate;
        for (SCRunningApplication* application in content.applications)
            if (application.processID==NSProcessInfo.processInfo.processIdentifier) [excluded addObject:application];
        if (!display) { dispatch_async(dispatch_get_main_queue(),^{ completion(sr::Error(@"所选显示器已断开，请重新框选。")); }); return; }
        if (excluded.count==0) {
            dispatch_async(dispatch_get_main_queue(),^{ completion(sr::Error(@"暂时无法排除录制控件，请退出软件后重新打开。")); }); return;
        }
        SCContentFilter* filter=[[SCContentFilter alloc] initWithDisplay:display excludingApplications:excluded exceptingWindows:@[]];
        SCStreamConfiguration* config=[SCStreamConfiguration new]; config.sourceRect=selection.rect;
        config.width=self->_recording.size.width; config.height=self->_recording.size.height;
        config.minimumFrameInterval=CMTimeMake(1,(int32_t)self->_recording.fps); config.queueDepth=5;
        config.pixelFormat=kCVPixelFormatType_32BGRA; config.showsCursor=YES;
        config.capturesAudio=YES; config.excludesCurrentProcessAudio=YES;
        config.sampleRate=48000; config.channelCount=2; config.colorSpaceName=kCGColorSpaceSRGB;
        dispatch_async(self->_queue, ^{
            NSError* setupError=nil;
            if (![self prepareWriters:&setupError]) {
                [self cancelPreparedCapture]; dispatch_async(dispatch_get_main_queue(),^{ completion(setupError); }); return;
            }
            self->_stream=[[SCStream alloc] initWithFilter:filter configuration:config delegate:self];
            BOOL ok=[self->_stream addStreamOutput:self type:SCStreamOutputTypeScreen sampleHandlerQueue:self->_queue error:&setupError] &&
                [self->_stream addStreamOutput:self type:SCStreamOutputTypeAudio sampleHandlerQueue:self->_queue error:&setupError];
            if (!ok) { [self cancelPreparedCapture]; dispatch_async(dispatch_get_main_queue(),^{ completion(setupError); }); return; }
            [self->_stream startCaptureWithCompletionHandler:^(NSError* startError) {
                dispatch_async(self->_queue, ^{
                    NSError* failure=startError ?: self->_fatalError;
                    if (!failure) [self startTimer];
                    else [self cancelPreparedCapture];
                    dispatch_async(dispatch_get_main_queue(),^{ completion(failure); });
                });
            }];
        });
    }];
}
- (BOOL)prepareWriters:(NSError**)error {
    _videoWriter=[[AVAssetWriter alloc] initWithURL:_recording.videoURL fileType:AVFileTypeMPEG4 error:error];
    if (!_videoWriter) return NO;
    auto bitrate=qrec::media::ExportQuality::ComputeVideoBitrate(_recording.size.width,_recording.size.height,(int)_recording.fps,100);
    NSDictionary* properties=@{AVVideoAverageBitRateKey:@(bitrate),AVVideoProfileLevelKey:AVVideoProfileLevelH264HighAutoLevel,
        AVVideoMaxKeyFrameIntervalKey:@(_recording.fps/2),AVVideoAllowFrameReorderingKey:@NO};
    NSDictionary* settings=@{AVVideoCodecKey:AVVideoCodecTypeH264,AVVideoWidthKey:@(_recording.size.width),
        AVVideoHeightKey:@(_recording.size.height),AVVideoCompressionPropertiesKey:properties,
        AVVideoEncoderSpecificationKey:@{(__bridge NSString*)kVTVideoEncoderSpecification_EnableHardwareAcceleratedVideoEncoder:@YES}};
    _videoInput=[AVAssetWriterInput assetWriterInputWithMediaType:AVMediaTypeVideo outputSettings:settings];
    _videoInput.expectsMediaDataInRealTime=YES;
    if (![_videoWriter canAddInput:_videoInput]) { *error=sr::Error(@"此 Mac 无法编码选定分辨率，请尝试较小选区。"); return NO; }
    [_videoWriter addInput:_videoInput];
    _adaptor=[AVAssetWriterInputPixelBufferAdaptor assetWriterInputPixelBufferAdaptorWithAssetWriterInput:_videoInput
                                                                       sourcePixelBufferAttributes:nil];
    // Keep stop latency independent of file size; edited exports optimize placement in the background.
    _videoWriter.shouldOptimizeForNetworkUse=NO;
    if (![_videoWriter startWriting]) { *error=_videoWriter.error; return NO; }
    [_videoWriter startSessionAtSourceTime:kCMTimeZero];
    _audioWriter=[[AVAssetWriter alloc] initWithURL:_recording.audioURL fileType:AVFileTypeAppleM4A error:error];
    if (!_audioWriter) return NO;
    _audioInput=[AVAssetWriterInput assetWriterInputWithMediaType:AVMediaTypeAudio outputSettings:@{
        AVFormatIDKey:@(kAudioFormatMPEG4AAC),AVSampleRateKey:@48000,AVNumberOfChannelsKey:@2,AVEncoderBitRateKey:@192000}];
    _audioInput.expectsMediaDataInRealTime=YES;
    if (![_audioWriter canAddInput:_audioInput]) { *error=sr::Error(@"系统声音编码器不可用。"); return NO; }
    [_audioWriter addInput:_audioInput];
    return YES;
}
- (void)startTimer {
    _timer=dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER,0,0,_queue);
    dispatch_source_set_timer(_timer,DISPATCH_TIME_NOW,NSEC_PER_SEC/_recording.fps,NSEC_PER_MSEC);
    __weak SRRecorder* weakSelf=self;
    dispatch_source_set_event_handler(_timer,^{ [weakSelf appendVideoAt:HostNow()]; });
    dispatch_resume(_timer);
}
- (void)detachStream {
    [_stream removeStreamOutput:self type:SCStreamOutputTypeScreen error:nil];
    [_stream removeStreamOutput:self type:SCStreamOutputTypeAudio error:nil];
    _stream=nil;
}
- (void)cancelPreparedCapture {
    if (_videoWriter.status==AVAssetWriterStatusWriting) [_videoWriter cancelWriting];
    if (_audioWriter.status==AVAssetWriterStatusWriting) [_audioWriter cancelWriting];
    [self detachStream];
}
- (void)appendVideoAt:(double)host {
    [self drainAudio];
    if (_stopping || !_latestFrame || _clock.Paused() || !_clock.Started()) return;
    double seconds=_clock.Elapsed(host); self.recordedSeconds=seconds;
    int64_t index=(int64_t)floor(seconds*_recording.fps);
    if (index<=_lastFrameIndex || !_videoInput.readyForMoreMediaData) return;
    if ([_adaptor appendPixelBuffer:_latestFrame withPresentationTime:CMTimeMake(index,(int32_t)_recording.fps)]) _lastFrameIndex=index;
    else [self fail:_videoWriter.error ?: sr::Error(@"视频写入失败，原始文件已保留。")];
}
- (void)stream:(SCStream*)stream didOutputSampleBuffer:(CMSampleBufferRef)sample ofType:(SCStreamOutputType)type {
    if (_stopping || _fatalError || !CMSampleBufferIsValid(sample) || !CMSampleBufferDataIsReady(sample)) return;
    if (type==SCStreamOutputTypeScreen) {
        NSArray* attachments=(__bridge NSArray*)CMSampleBufferGetSampleAttachmentsArray(sample,NO);
        NSNumber* status=attachments.firstObject[SCStreamFrameInfoStatus];
        if (!status || status.integerValue!=SCFrameStatusComplete) return;
        CVPixelBufferRef buffer=CMSampleBufferGetImageBuffer(sample); if (!buffer) return;
        if (_latestFrame) CVPixelBufferRelease(_latestFrame); _latestFrame=CVPixelBufferRetain(buffer);
        if (!_clock.Started()) _clock.Start(CMTimeGetSeconds(CMSampleBufferGetPresentationTimeStamp(sample)));
        [self appendVideoAt:HostNow()];
    } else if (type==SCStreamOutputTypeAudio) [self appendAudio:sample];
}
- (void)appendAudio:(CMSampleBufferRef)sample {
    double host=CMTimeGetSeconds(CMSampleBufferGetPresentationTimeStamp(sample));
    double time=_clock.SampleTime(host);
    if (time<0 || time+0.0001<_lastAudioEnd) return;
    if (_audioWriter.status==AVAssetWriterStatusUnknown) {
        if (![_audioWriter startWriting]) { [self fail:_audioWriter.error]; return; }
        [_audioWriter startSessionAtSourceTime:kCMTimeZero];
    }
    constexpr size_t maximumPendingPackets=200;
    if (_audioPending.size()>=maximumPendingPackets) {
        [self fail:sr::Error(@"音频编码器处理过慢，录制已停止。请尝试 30 FPS 或较小选区。")]; return;
    }
    CMItemCount count=0;
    if (CMSampleBufferGetSampleTimingInfoArray(sample,0,nullptr,&count)!=noErr || count<=0) return;
    std::vector<CMSampleTimingInfo> timing((size_t)count);
    if (CMSampleBufferGetSampleTimingInfoArray(sample,count,timing.data(),&count)!=noErr) return;
    CMTime offset=CMTimeSubtract(CMSampleBufferGetPresentationTimeStamp(sample),sr::Time(time));
    for (auto& item:timing) {
        item.presentationTimeStamp=CMTimeSubtract(item.presentationTimeStamp,offset);
        if (CMTIME_IS_VALID(item.decodeTimeStamp)) item.decodeTimeStamp=CMTimeSubtract(item.decodeTimeStamp,offset);
    }
    CMSampleBufferRef adjusted=nullptr;
    if (CMSampleBufferCreateCopyWithNewTiming(kCFAllocatorDefault,sample,count,timing.data(),&adjusted)==noErr) {
        _audioPending.push_back(adjusted);
        _lastAudioEnd=time+CMTimeGetSeconds(CMSampleBufferGetDuration(sample)); [self drainAudio];
    }
}
- (void)drainAudio {
    while (!_audioPending.empty() && _audioInput.readyForMoreMediaData && !_fatalError) {
        CMSampleBufferRef sample=_audioPending.front(); _audioPending.pop_front();
        BOOL appended=[_audioInput appendSampleBuffer:sample]; CFRelease(sample);
        if (!appended) { [self fail:_audioWriter.error]; break; }
    }
}
- (void)togglePause {
    dispatch_async(_queue, ^{
        if (self->_stopping) return;
        if (self->_clock.Paused()) self->_clock.Resume(HostNow()); else self->_clock.Pause(HostNow());
        self.paused=self->_clock.Paused(); self.recordedSeconds=self->_clock.Elapsed(HostNow());
    });
}
- (void)fail:(NSError*)error {
    if (_fatalError) return;
    _fatalError=error ?: sr::Error(@"录制中断。"); sr::Log(@"capture-error",_fatalError.localizedDescription);
    dispatch_async(dispatch_get_main_queue(),^{ if (self.failure) self.failure(self->_fatalError); });
}
- (void)stream:(SCStream*)stream didStopWithError:(NSError*)error {
    dispatch_async(_queue, ^{ if (!self->_stopping) [self fail:error]; });
}
- (void)stop:(void (^)(SRRecording*,NSError*))completion {
    dispatch_async(_queue, ^{
        if (self->_stopping) return;
        [self appendVideoAt:HostNow()]; self->_stopping=YES;
        self->_finishDeadline=HostNow()+5;
        if (self->_timer) { dispatch_source_cancel(self->_timer); self->_timer=nil; }
        self.recordedSeconds=self->_clock.Elapsed(HostNow());
        [self->_stream stopCaptureWithCompletionHandler:^(NSError* stopError) {
            dispatch_async(self->_queue, ^{ [self finish:completion]; });
        }];
    });
}
- (void)finish:(void (^)(SRRecording*,NSError*))completion {
    [self drainAudio];
    if (!_audioPending.empty() && !_fatalError && HostNow()<_finishDeadline) {
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW,NSEC_PER_MSEC*10),_queue,^{ [self finish:completion]; }); return;
    }
    if (!_audioPending.empty() && !_fatalError) _fatalError=sr::Error(@"音频编码器未能完成，原始录制已保留。请尝试较小选区。");
    for (CMSampleBufferRef sample:_audioPending) CFRelease(sample); _audioPending.clear();
    dispatch_group_t group=dispatch_group_create();
    if (_lastFrameIndex<0) _fatalError=sr::Error(@"没有收到屏幕画面。请授权屏幕录制后重新打开软件。");
    for (AVAssetWriter* writer in @[_videoWriter,_audioWriter]) {
        if (writer.status==AVAssetWriterStatusWriting) {
            [writer endSessionAtSourceTime:sr::Time(MAX(self.recordedSeconds,1.0/_recording.fps))];
            for (AVAssetWriterInput* input in writer.inputs) [input markAsFinished];
            dispatch_group_enter(group);
            [writer finishWritingWithCompletionHandler:^{ dispatch_group_leave(group); }];
        } else if (writer==_audioWriter) _recording.audioURL=nil;
    }
    dispatch_group_notify(group,_queue,^{
        NSError* error=self->_fatalError ?: self->_videoWriter.error ?: self->_audioWriter.error;
        if (self->_latestFrame) { CVPixelBufferRelease(self->_latestFrame); self->_latestFrame=nullptr; }
        [self detachStream];
        sr::Log(@"recording-finished",[NSString stringWithFormat:@"%.3f seconds, %@",self.recordedSeconds,error.localizedDescription ?: @"OK"]);
        dispatch_async(dispatch_get_main_queue(),^{ completion(error?nil:self->_recording,error); });
    });
}
- (void)dealloc {
    if (_latestFrame) CVPixelBufferRelease(_latestFrame);
    for (CMSampleBufferRef sample:_audioPending) CFRelease(sample);
}
@end
