#import "tests/SelfTest.h"
#import "capture/Recorder.h"
#import "editor/EditorController.h"
#import "media/ExportJob.h"
#import "common/Support.h"
#import <AudioToolbox/AudioToolbox.h>
#import <ImageIO/ImageIO.h>
#include <thread>
#include <iostream>
#include <stdexcept>
#include <vector>
#include <algorithm>
namespace {
void Check(bool value,const char* label) {
    if (!value) throw std::runtime_error(label);
    std::cout<<"PASS "<<label<<'\n';
}
bool Pump(BOOL (^done)(void),double seconds=60) {
    NSDate* deadline=[NSDate dateWithTimeIntervalSinceNow:seconds];
    while (!done() && deadline.timeIntervalSinceNow>0)
        [NSRunLoop.currentRunLoop runUntilDate:[NSDate dateWithTimeIntervalSinceNow:0.01]];
    return done();
}
void Ready(AVAssetWriterInput* input,AVAssetWriter* writer) {
    NSDate* deadline=[NSDate dateWithTimeIntervalSinceNow:10];
    while (!input.readyForMoreMediaData && writer.status==AVAssetWriterStatusWriting && deadline.timeIntervalSinceNow>0)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    if (!input.readyForMoreMediaData) throw std::runtime_error(writer.error.localizedDescription.UTF8String ?: "encoder timed out");
}
void Finish(AVAssetWriter* writer,AVAssetWriterInput* input) {
    [writer endSessionAtSourceTime:CMTimeMake(2,1)]; [input markAsFinished];
    __block BOOL done=NO; [writer finishWritingWithCompletionHandler:^{ done=YES; }];
    Check(Pump(^BOOL{ return done; }) && writer.status==AVAssetWriterStatusCompleted,"synthetic source finalized");
}
SRRecording* Fixture(NSURL* directory) {
    SRRecording* recording=[SRRecording new]; recording.size=CGSizeMake(320,180); recording.fps=60;
    recording.videoURL=[directory URLByAppendingPathComponent:@"fixture.mp4"];
    recording.audioURL=[directory URLByAppendingPathComponent:@"fixture-audio.m4a"];
    NSError* error=nil;
    AVAssetWriter* video=[[AVAssetWriter alloc] initWithURL:recording.videoURL fileType:AVFileTypeMPEG4 error:&error];
    AVAssetWriterInput* input=[AVAssetWriterInput assetWriterInputWithMediaType:AVMediaTypeVideo outputSettings:@{
        AVVideoCodecKey:AVVideoCodecTypeH264,AVVideoWidthKey:@320,AVVideoHeightKey:@180,
        AVVideoCompressionPropertiesKey:@{AVVideoAverageBitRateKey:@2000000,AVVideoMaxKeyFrameIntervalKey:@15}}];
    [video addInput:input];
    AVAssetWriterInputPixelBufferAdaptor* adaptor=[AVAssetWriterInputPixelBufferAdaptor
        assetWriterInputPixelBufferAdaptorWithAssetWriterInput:input sourcePixelBufferAttributes:@{
            (__bridge NSString*)kCVPixelBufferPixelFormatTypeKey:@(kCVPixelFormatType_32BGRA),
            (__bridge NSString*)kCVPixelBufferWidthKey:@320,(__bridge NSString*)kCVPixelBufferHeightKey:@180}];
    Check([video startWriting],"fixture H.264 encoder starts"); [video startSessionAtSourceTime:kCMTimeZero];
    for (int frame=0;frame<120;++frame) {
        Ready(input,video); CVPixelBufferRef buffer=nullptr;
        Check(CVPixelBufferPoolCreatePixelBuffer(kCFAllocatorDefault,adaptor.pixelBufferPool,&buffer)==kCVReturnSuccess,"fixture pixel allocation");
        CVPixelBufferLockBaseAddress(buffer,0); auto* bytes=(uint8_t*)CVPixelBufferGetBaseAddress(buffer);
        size_t stride=CVPixelBufferGetBytesPerRow(buffer);
        for (int y=0;y<180;++y) for (int x=0;x<320;++x) {
            uint8_t* pixel=bytes+y*stride+x*4;
            pixel[0]=(uint8_t)(80+y/3); pixel[1]=(uint8_t)(40+x/4); pixel[2]=22; pixel[3]=255;
            if (x>frame*2 && x<frame*2+4) pixel[0]=pixel[1]=pixel[2]=220;
        }
        CVPixelBufferUnlockBaseAddress(buffer,0);
        BOOL ok=[adaptor appendPixelBuffer:buffer withPresentationTime:CMTimeMake(frame,60)]; CVPixelBufferRelease(buffer);
        if (!ok) throw std::runtime_error("fixture frame append");
    }
    Finish(video,input);
    AVAssetWriter* audio=[[AVAssetWriter alloc] initWithURL:recording.audioURL fileType:AVFileTypeAppleM4A error:&error];
    AVAssetWriterInput* audioInput=[AVAssetWriterInput assetWriterInputWithMediaType:AVMediaTypeAudio outputSettings:@{
        AVFormatIDKey:@(kAudioFormatMPEG4AAC),AVSampleRateKey:@48000,AVNumberOfChannelsKey:@2,AVEncoderBitRateKey:@192000}];
    [audio addInput:audioInput]; Check([audio startWriting],"fixture AAC encoder starts"); [audio startSessionAtSourceTime:kCMTimeZero];
    AudioStreamBasicDescription format={}; format.mSampleRate=48000; format.mFormatID=kAudioFormatLinearPCM;
    format.mFormatFlags=kAudioFormatFlagIsFloat|kAudioFormatFlagIsPacked; format.mFramesPerPacket=1;
    format.mBytesPerFrame=8; format.mBytesPerPacket=8; format.mChannelsPerFrame=2; format.mBitsPerChannel=32;
    CMAudioFormatDescriptionRef description=nullptr;
    Check(CMAudioFormatDescriptionCreate(kCFAllocatorDefault,&format,0,nullptr,0,nullptr,nullptr,&description)==noErr,"fixture PCM format");
    for (int start=0;start<96000;start+=480) {
        Ready(audioInput,audio); std::vector<float> samples(960);
        for (int i=0;i<480;++i) samples[i*2]=samples[i*2+1]=0.2F*std::sin(2*3.141592653589793*440*(start+i)/48000);
        CMBlockBufferRef block=nullptr; CMSampleBufferRef sample=nullptr;
        CMBlockBufferCreateWithMemoryBlock(kCFAllocatorDefault,nullptr,samples.size()*sizeof(float),kCFAllocatorDefault,
            nullptr,0,samples.size()*sizeof(float),0,&block);
        CMBlockBufferReplaceDataBytes(samples.data(),block,0,samples.size()*sizeof(float));
        CMSampleTimingInfo timing={CMTimeMake(1,48000),CMTimeMake(start,48000),kCMTimeInvalid};
        OSStatus status=CMSampleBufferCreateReady(kCFAllocatorDefault,block,description,480,1,&timing,0,nullptr,&sample);
        BOOL ok=status==noErr && [audioInput appendSampleBuffer:sample];
        if (sample) CFRelease(sample); CFRelease(block);
        if (!ok) throw std::runtime_error("fixture audio append");
    }
    CFRelease(description); Finish(audio,audioInput); return recording;
}
NSURL* Export(SRExportRequest* request) {
    SRExportJob* job=[SRExportJob new]; __block BOOL done=NO; __block NSURL* result=nil; __block NSError* failure=nil;
    [job run:request completion:^(NSURL* url,NSError* error) {
        dispatch_async(dispatch_get_main_queue(),^{ result=url; failure=error; done=YES; });
    }];
    Check(Pump(^BOOL { return done; },90),"export completed before timeout");
    if (failure) throw std::runtime_error(failure.localizedDescription.UTF8String);
    Check(result!=nil,"export produced an artifact"); return result;
}
double AudioRMS(AVAsset* asset) {
    NSError* error=nil; AVAssetReader* reader=[[AVAssetReader alloc] initWithAsset:asset error:&error];
    AVAssetTrack* track=[asset tracksWithMediaType:AVMediaTypeAudio].firstObject;
    Check(track!=nil,"export contains audio track");
    AVAssetReaderTrackOutput* output=[AVAssetReaderTrackOutput assetReaderTrackOutputWithTrack:track outputSettings:@{
        AVFormatIDKey:@(kAudioFormatLinearPCM),AVLinearPCMBitDepthKey:@32,AVLinearPCMIsFloatKey:@YES,
        AVLinearPCMIsBigEndianKey:@NO,AVLinearPCMIsNonInterleaved:@NO}];
    [reader addOutput:output]; Check([reader startReading],"audio decode starts");
    double squares=0,peak=0; size_t count=0;
    while (CMSampleBufferRef sample=[output copyNextSampleBuffer]) {
        CMBlockBufferRef buffer=CMSampleBufferGetDataBuffer(sample); size_t size=CMBlockBufferGetDataLength(buffer);
        std::vector<float> values(size/sizeof(float));
        CMBlockBufferCopyDataBytes(buffer,0,size,values.data());
        for (float value:values) { squares+=value*value; peak=MAX(peak,fabs(value)); ++count; }
        CFRelease(sample);
    }
    Check(reader.status==AVAssetReaderStatusCompleted && count>1000,"audio fully decodes");
    Check(peak<0.35,"audio is not clipped or amplified"); return sqrt(squares/count);
}
size_t RedPixels(NSURL* url,double time,NSURL* png) {
    AVURLAsset* asset=[AVURLAsset URLAssetWithURL:url options:nil];
    AVAssetImageGenerator* generator=[AVAssetImageGenerator assetImageGeneratorWithAsset:asset];
    generator.requestedTimeToleranceBefore=kCMTimeZero; generator.requestedTimeToleranceAfter=kCMTimeZero;
    NSError* error=nil; CGImageRef image=[generator copyCGImageAtTime:sr::Time(time) actualTime:nullptr error:&error];
    Check(image!=nullptr,"exported video frame decodes");
    if (png) {
        CGImageDestinationRef output=CGImageDestinationCreateWithURL((__bridge CFURLRef)png,CFSTR("public.png"),1,nullptr);
        CGImageDestinationAddImage(output,image,nullptr); CGImageDestinationFinalize(output); CFRelease(output);
    }
    size_t w=CGImageGetWidth(image),h=CGImageGetHeight(image); std::vector<uint8_t> pixels(w*h*4);
    CGColorSpaceRef space=CGColorSpaceCreateDeviceRGB();
    CGContextRef context=CGBitmapContextCreate(pixels.data(),w,h,8,w*4,space,kCGImageAlphaPremultipliedLast|kCGBitmapByteOrder32Big);
    CGContextDrawImage(context,CGRectMake(0,0,w,h),image); size_t red=0;
    for (size_t p=0;p<pixels.size();p+=4) if (pixels[p]>pixels[p+1]+45 && pixels[p]>130) ++red;
    CGContextRelease(context); CGColorSpaceRelease(space); CGImageRelease(image); return red;
}
}
int SRMediaTests(NSURL* directory) {
    @try {
        try {
            NSError* error=nil;
            Check([NSFileManager.defaultManager createDirectoryAtURL:directory withIntermediateDirectories:YES attributes:nil error:&error],"test output directory");
            SRRecording* recording=Fixture(directory);
            SRExportRequest* request=[SRExportRequest new]; request.videoURL=recording.videoURL; request.audioURL=recording.audioURL;
            request.end=2; request.fps=60;
            auto started=std::chrono::steady_clock::now(); NSURL* raw=Export(request);
            Check([raw isEqual:recording.videoURL],"untouched MP4 reuses original without encoding");
            std::cout<<"Original-ready milliseconds: "<<std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now()-started).count()<<'\n';
            request.start=0.3; request.end=1.5; request.audio=YES;
            NSURL* trimmed=Export(request); AVURLAsset* trimmedAsset=[AVURLAsset URLAssetWithURL:trimmed options:nil];
            Check(fabs(CMTimeGetSeconds(trimmedAsset.duration)-1.2)<0.05,"passthrough trim duration");
            double rms=AudioRMS(trimmedAsset); Check(rms>0.10 && rms<0.20,"trimmed audio retains input volume");
            using namespace qrec::annotations;
            Document document; document.Reset({320,180},Time{2000});
            Mark circle=document.NewMark(Tool::Circle,Time{500}); circle.points={{80,80},{112,80}}; circle.strokeWidth=6;
            Check(document.Put(circle),"test circle inserted");
            Mark arrow=document.NewMark(Tool::Arrow,Time{500}); arrow.points={{160,140},{230,60}}; Check(document.Put(arrow),"test arrow inserted");
            Mark text=document.NewMark(Tool::Text,Time{500}); text.points={{8,8}}; text.fontSize=24; text.text=L"Mac 标注";
            Check(document.Put(text),"test text inserted");
            request.annotations=document.Current(); request.start=0.2; request.end=1.8; request.speed=2; request.quality=50;
            NSURL* rendered=Export(request); AVURLAsset* renderedAsset=[AVURLAsset URLAssetWithURL:rendered options:nil];
            Check(fabs(CMTimeGetSeconds(renderedAsset.duration)-0.8)<0.06,"speed conversion duration");
            CGSize size=[renderedAsset tracksWithMediaType:AVMediaTypeVideo].firstObject.naturalSize;
            Check(size.width==160 && size.height==90,"quality export size");
            Check(AudioRMS(renderedAsset)>0.09,"speed conversion keeps audible audio");
            size_t visible=RedPixels(rendered,0.2,[directory URLByAppendingPathComponent:@"annotations-visible.png"]);
            size_t hidden=RedPixels(rendered,0.7,[directory URLByAppendingPathComponent:@"annotations-hidden.png"]);
            Check(visible>hidden+20,"annotation timing follows source time through speed conversion");
            request.gif=YES; request.audio=NO; request.speed=1.5; request.start=0.2; request.end=1.2; request.quality=60;
            NSURL* gif=Export(request);
            CGImageSourceRef gifSource=CGImageSourceCreateWithURL((__bridge CFURLRef)gif,nullptr);
            Check(gifSource && CGImageSourceGetCount(gifSource)>=12,"GIF contains expected animation frames");
            if (gifSource) CFRelease(gifSource);
            NSURL* delivered=[directory URLByAppendingPathComponent:@"delivered.mp4"];
            Check(sr::DeliverFile(rendered,delivered,&error),"atomic file delivery");
            Check([[NSData dataWithContentsOfURL:rendered] isEqual:[NSData dataWithContentsOfURL:delivered]],"delivered bytes match cache exactly");
            SRPreferences* preferences=[SRPreferences new];
            SREditorController* editor=[[SREditorController alloc] initWithRecording:recording preferences:preferences];
            [editor writeUISnapshot:[directory URLByAppendingPathComponent:@"editor.png"]];
            [editor.window setContentSize:NSMakeSize(1000,680)];
            [editor writeUISnapshot:[directory URLByAppendingPathComponent:@"editor-compact.png"]];
            [editor close];
            std::cout<<"All macOS media tests passed\n"; return 0;
        } catch (const std::exception& failure) { std::cerr<<"FAIL "<<failure.what()<<'\n'; return 1; }
    } @catch (NSException* exception) { std::cerr<<"FAIL Objective-C exception "<<exception.reason.UTF8String<<'\n'; return 1; }
}
