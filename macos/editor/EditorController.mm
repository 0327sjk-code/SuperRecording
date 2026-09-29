#import "editor/EditorInternal.h"
#import "media/ExportComposition.h"
#import "common/Support.h"
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>
#include "media/ExportQuality.h"
#include <algorithm>
using namespace qrec::annotations;
@implementation SREditorController
- (instancetype)initWithRecording:(SRRecording*)recording preferences:(SRPreferences*)preferences {
    NSRect visible=NSScreen.mainScreen.visibleFrame;
    NSRect frame=NSMakeRect(0,0,MIN(1500,visible.size.width-64),MIN(1000,visible.size.height-70));
    NSWindow* window=[[NSWindow alloc] initWithContentRect:frame
        styleMask:NSWindowStyleMaskTitled|NSWindowStyleMaskClosable|NSWindowStyleMaskMiniaturizable|NSWindowStyleMaskResizable
        backing:NSBackingStoreBuffered defer:NO];
    if ((self=[super initWithWindow:window])) {
        _recording=recording; _preferences=preferences;
        window.title=@"SuperRecording · 编辑录屏"; window.delegate=self; window.releasedWhenClosed=NO;
        window.appearance=[NSAppearance appearanceNamed:NSAppearanceNameDarkAqua];
        window.backgroundColor=sr::Color(0x121315); window.contentMinSize=NSMakeSize(1000,680); [window center];
        AVURLAsset* asset=[AVURLAsset URLAssetWithURL:recording.videoURL options:nil];
        _duration=CMTimeGetSeconds(asset.duration);
        if (!std::isfinite(_duration) || _duration<=0) _duration=1.0/recording.fps;
        _document.Reset({(float)recording.size.width,(float)recording.size.height},Time{(int64_t)llround(_duration*1000)});
        _player=[AVPlayer playerWithPlayerItem:[AVPlayerItem playerItemWithAsset:asset]];
        _player.muted=YES; _player.actionAtItemEnd=AVPlayerActionAtItemEndPause;
        _exporter=[SRExportCoordinator new];
        [self buildInterface]; [self connectEvents]; [self refreshLabels]; [self prepareExport];
        if (preferences.quality!=100) [self schedulePreviewQuality];
    }
    return self;
}
- (void)connectEvents {
    __weak SREditorController* weakSelf=self;
    _canvas.willInteract=^{ [weakSelf pause]; };
    _canvas.edited=^{ [weakSelf refreshLabels]; [weakSelf prepareExport]; };
    _canvas.selectionChanged=^{ [weakSelf annotationSelection]; };
    _timeline.changed=^(SRTimelineChange kind,BOOL finished) {
        SREditorController* owner=weakSelf; if (!owner) return;
        [owner pause]; [owner->_canvas finishText:YES];
        if (kind==SRTimelineSeek) [owner seek:owner->_timeline.position];
        else if (kind==SRTimelineTrim) {
            [owner refreshLabels];
            if (finished) [owner prepareExport];
        } else if (finished) {
            const Mark* selected=owner->_document.Find(owner->_canvas.selectedID);
            if (selected) {
                Mark mark=*selected; mark.start=Time{(int64_t)llround(owner->_timeline.annotationStart*1000)};
                mark.end=Time{(int64_t)llround(owner->_timeline.annotationEnd*1000)};
                (void)owner->_document.Put(std::move(mark)); owner->_canvas.needsDisplay=YES;
                [owner refreshLabels]; [owner prepareExport];
            }
        }
    };
    _timeObserver=[_player addPeriodicTimeObserverForInterval:CMTimeMake(1,30) queue:dispatch_get_main_queue()
        usingBlock:^(CMTime time) {
        SREditorController* owner=weakSelf; if (!owner || owner->_seeking || owner->_closed) return;
        double seconds=CMTimeGetSeconds(time); if (!std::isfinite(seconds)) return;
        if (owner->_player.rate!=0 && seconds>=owner->_timeline.end-0.5/owner->_recording.fps) {
            [owner pause]; [owner seek:owner->_timeline.end]; return;
        }
        owner->_timeline.position=seconds; owner->_timeline.needsDisplay=YES;
        owner->_canvas.time=seconds; [owner refreshPosition];
    }];
    _exporter.changed=^(NSURL* url,NSError* error,BOOL preparing) {
        SREditorController* owner=weakSelf; if (!owner || owner->_closed) return;
        if (error) {
            owner->_status.stringValue=[@"导出失败：" stringByAppendingString:error.localizedDescription];
            owner->_status.textColor=sr::Color(0xFFB4AF); owner->_pendingDelivery=0;
            [owner deliveryBusy:NO]; sr::Log(@"export-error",error.localizedDescription);
        } else if (url) {
            owner->_status.stringValue=@"成片已就绪，复制或保存无需再次编码";
            owner->_status.textColor=sr::Color(0x67BE7E);
            NSNumber* size=nil; [url getResourceValue:&size forKey:NSURLFileSizeKey error:nil];
            owner->_fileSize.stringValue=[NSString stringWithFormat:@"%.2f MB",size.doubleValue/(1024*1024)];
            if (owner->_pendingDelivery) [owner deliver];
        } else if (preparing) {
            owner->_status.stringValue=owner->_pendingDelivery?@"正在完成当前成片，完成后立即交付…":@"正在后台准备成片，可继续编辑";
            owner->_status.textColor=sr::Color(0xB8BBC0); owner->_fileSize.stringValue=@"计算中";
        }
    };
    _keyMonitor=[NSEvent addLocalMonitorForEventsMatchingMask:NSEventMaskKeyDown handler:^NSEvent*(NSEvent* event) {
        SREditorController* owner=weakSelf;
        if (!owner || event.window!=owner.window || owner->_pendingDelivery || owner->_delivering ||
            [owner.window.firstResponder isKindOfClass:NSTextView.class]) return event;
        if ((event.modifierFlags&NSEventModifierFlagCommand) && [event.charactersIgnoringModifiers.lowercaseString isEqualToString:@"z"]) {
            if (event.modifierFlags&NSEventModifierFlagShift) [owner redo:nil]; else [owner undo:nil]; return nil;
        }
        if (event.keyCode==49 && !event.isARepeat) { [owner togglePlay:nil]; return nil; }
        if (event.keyCode==51) { [owner removeMark:nil]; return nil; }
        return event;
    }];
}
- (SRExportRequest*)request {
    SRExportRequest* request=[SRExportRequest new]; request.videoURL=_recording.videoURL; request.audioURL=_recording.audioURL;
    request.start=_timeline.start; request.end=_timeline.end; request.speed=round(_speed.doubleValue*10)/10;
    request.quality=qrec::media::ExportQuality::Normalize((int)lround(_quality.doubleValue));
    request.fps=_recording.fps; request.audio=_sound.state==NSControlStateValueOn;
    request.gif=_format.selectedSegment==1; request.annotations=_document.Current(); return request;
}
- (void)prepareExport { [_exporter prepare:self.request immediately:NO]; }
- (void)refreshPosition {
    _positionLabel.stringValue=[NSString stringWithFormat:@"%@ / %@",sr::TimeLabel(_timeline.position),sr::TimeLabel(_duration)];
}
- (void)refreshLabels {
    double speed=round(_speed.doubleValue*10)/10;
    NSInteger quality=qrec::media::ExportQuality::Normalize((int)lround(_quality.doubleValue));
    _qualityValue.stringValue=[NSString stringWithFormat:@"%ld%%",(long)quality];
    _speedValue.stringValue=[NSString stringWithFormat:@"%.1f×",speed];
    _rangeLabel.stringValue=[NSString stringWithFormat:@"保留 %@ — %@ · 输出 %@",sr::TimeLabel(_timeline.start),
        sr::TimeLabel(_timeline.end),sr::TimeLabel((_timeline.end-_timeline.start)/speed)];
    _rangeLabel.toolTip=_rangeLabel.stringValue;
    _widthLabel.stringValue=[NSString stringWithFormat:@"粗细 %.1f",_width.doubleValue];
    BOOL idle=!_pendingDelivery && !_delivering;
    _undo.enabled=idle&&_document.CanUndo(); _redo.enabled=idle&&_document.CanRedo();
    _delete.enabled=idle&&_document.Find(_canvas.selectedID)!=nullptr;
    _sound.enabled=idle&&_recording.audioURL!=nil&&_format.selectedSegment==0;
    [self refreshPosition];
}
- (void)annotationSelection {
    const Mark* mark=_document.Find(_canvas.selectedID);
    _timeline.hasAnnotation=mark!=nullptr;
    if (mark) { _timeline.annotationStart=mark->start.count()/1000.0; _timeline.annotationEnd=mark->end.count()/1000.0; }
    _timeline.needsDisplay=YES; [self refreshLabels];
}
- (void)selectTool:(NSButton*)sender {
    [_canvas finishText:YES]; [self pause];
    _canvas.tool=(Tool)sender.tag;
    for (NSButton* button in _tools) button.state=button==sender?NSControlStateValueOn:NSControlStateValueOff;
    [self.window makeFirstResponder:_canvas];
}
- (void)undo:(id)sender {
    [_canvas finishText:YES]; if (_document.Undo()) { _canvas.needsDisplay=YES; [self annotationSelection]; [self prepareExport]; }
}
- (void)redo:(id)sender {
    [_canvas finishText:YES]; if (_document.Redo()) { _canvas.needsDisplay=YES; [self annotationSelection]; [self prepareExport]; }
}
- (void)removeMark:(id)sender { [_canvas deleteSelection]; }
- (void)changeColor:(id)sender {
    NSColor* color=[_color.color colorUsingColorSpace:NSColorSpace.sRGBColorSpace];
    _canvas.argb=0xFF000000|((uint32_t)lround(color.redComponent*255)<<16)|((uint32_t)lround(color.greenComponent*255)<<8)|
        (uint32_t)lround(color.blueComponent*255);
    const Mark* selected=_document.Find(_canvas.selectedID);
    if (selected) { Mark mark=*selected; mark.argb=_canvas.argb; (void)_document.Put(mark); _canvas.needsDisplay=YES; [self prepareExport]; }
}
- (void)changeWidth:(id)sender {
    _canvas.strokeWidth=_width.doubleValue;
    const Mark* selected=_document.Find(_canvas.selectedID);
    if (selected && selected->tool!=Tool::Text) {
        Mark mark=*selected; mark.strokeWidth=(float)_width.doubleValue; (void)_document.Put(mark);
        _canvas.needsDisplay=YES; [self prepareExport];
    }
    [self refreshLabels];
}
- (void)changeExport:(id)sender {
    [_canvas finishText:YES];
    if (sender==_quality) {
        _preferences.quality=qrec::media::ExportQuality::Normalize((int)lround(_quality.doubleValue)); [self schedulePreviewQuality];
    }
    if (sender==_speed && _player.rate!=0) _player.rate=round(_speed.doubleValue*10)/10;
    [self refreshLabels]; [self prepareExport];
}
- (void)schedulePreviewQuality {
    NSUInteger generation=++_previewGeneration; __weak SREditorController* weakSelf=self;
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW,NSEC_PER_MSEC*200),dispatch_get_main_queue(),^{
        SREditorController* owner=weakSelf; if (!owner || owner->_closed || generation!=owner->_previewGeneration) return;
        SRExportRequest* request=owner.request; request.start=0; request.end=owner->_duration; request.speed=1; request.annotations=nullptr;
        AVPlayerItem* item=owner->_player.currentItem;
        item.videoComposition=request.quality==100?nil:SRVideoComposition(item.asset,request,NO);
    });
}
- (void)setStart:(id)sender {
    _timeline.start=std::clamp(_timeline.position,0.0,MAX(0.0,_timeline.end-_timeline.minimumSpan));
    _timeline.needsDisplay=YES; [self refreshLabels]; [self prepareExport];
}
- (void)setEnd:(id)sender {
    _timeline.end=std::clamp(_timeline.position,MIN(_duration,_timeline.start+_timeline.minimumSpan),_duration);
    _timeline.needsDisplay=YES; [self refreshLabels]; [self prepareExport];
}
- (void)pause { [_player pause]; _play.title=@"播放"; _play.image=[NSImage imageWithSystemSymbolName:@"play.fill" accessibilityDescription:@"播放"]; }
- (void)togglePlay:(id)sender {
    [_canvas finishText:YES];
    if (_player.rate!=0) { [self pause]; return; }
    _play.title=@"暂停"; _play.image=[NSImage imageWithSystemSymbolName:@"pause.fill" accessibilityDescription:@"暂停"];
    double current=CMTimeGetSeconds(_player.currentTime);
    if (current<_timeline.start || current>=_timeline.end-0.5/_recording.fps) {
        __weak SREditorController* weakSelf=self;
        [_player seekToTime:sr::Time(_timeline.start) toleranceBefore:kCMTimeZero toleranceAfter:kCMTimeZero completionHandler:^(BOOL done) {
            dispatch_async(dispatch_get_main_queue(),^{ SREditorController* owner=weakSelf; if (owner && !owner->_closed && done) owner->_player.rate=round(owner->_speed.doubleValue*10)/10; });
        }];
    } else _player.rate=round(_speed.doubleValue*10)/10;
}
- (void)seek:(double)time {
    _pendingSeek=time; _timeline.position=time; _timeline.needsDisplay=YES; _canvas.time=time; [self refreshPosition];
    if (_seeking) return; _seeking=YES;
    __weak SREditorController* weakSelf=self;
    [_player seekToTime:sr::Time(time) toleranceBefore:kCMTimeZero toleranceAfter:CMTimeMake(1,(int32_t)_recording.fps)
        completionHandler:^(BOOL completed) {
        dispatch_async(dispatch_get_main_queue(),^{
            SREditorController* owner=weakSelf; if (!owner || owner->_closed) return;
            owner->_seeking=NO; if (fabs(owner->_pendingSeek-time)>0.0001) [owner seek:owner->_pendingSeek];
        });
    }];
}
- (void)copyVideo:(id)sender { [self beginDelivery:1]; }
- (void)saveVideo:(id)sender { [self beginDelivery:2]; }
- (void)beginDelivery:(NSInteger)kind {
    if (_pendingDelivery || _delivering) return;
    [_canvas finishText:YES]; [self pause]; [self prepareExport]; _pendingDelivery=kind;
    [self deliveryBusy:YES];
    if (_exporter.readyURL) [self deliver];
    else _status.stringValue=@"正在完成当前成片，完成后立即交付…";
}
static void EnableControls(NSView* root,BOOL enabled) {
    for (NSView* view in root.subviews) {
        if ([view isKindOfClass:NSControl.class] && ![view isKindOfClass:NSTextField.class]) ((NSControl*)view).enabled=enabled;
        EnableControls(view,enabled);
    }
}
- (void)deliveryBusy:(BOOL)busy {
    EnableControls(_root,!busy); _canvas.locked=busy; _timeline.locked=busy;
    if (!busy) [self refreshLabels];
}
- (void)deliver {
    if (_delivering || !_pendingDelivery || !_exporter.readyURL) return;
    _delivering=YES; NSInteger kind=_pendingDelivery; NSURL* source=_exporter.readyURL;
    NSURL* directory=kind==2?_preferences.saveDirectory:[sr::CacheDirectory().URLByDeletingLastPathComponent URLByAppendingPathComponent:@"Clipboard"];
    NSDateFormatter* formatter=[NSDateFormatter new]; formatter.dateFormat=@"yyyyMMdd_HHmmss";
    NSString* name=[NSString stringWithFormat:@"录屏_%@_%@.%@",[formatter stringFromDate:NSDate.date],
        [NSUUID.UUID.UUIDString substringToIndex:4],source.pathExtension];
    NSURL* destination=[directory URLByAppendingPathComponent:name];
    __weak SREditorController* weakSelf=self;
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED,0),^{
        NSError* error=nil;
        BOOL ok=[NSFileManager.defaultManager createDirectoryAtURL:directory withIntermediateDirectories:YES attributes:nil error:&error];
        if (ok) ok=sr::DeliverFile(source,destination,&error);
        dispatch_async(dispatch_get_main_queue(),^{
            SREditorController* owner=weakSelf; if (!owner || owner->_closed) return;
            owner->_delivering=NO; owner->_pendingDelivery=0; [owner deliveryBusy:NO];
            if (!ok) { sr::Alert(owner.window,error.localizedDescription); return; }
            if (kind==1) {
                [NSPasteboard.generalPasteboard clearContents];
                if (![NSPasteboard.generalPasteboard writeObjects:@[destination]]) { sr::Alert(owner.window,@"无法写入文件剪贴板，请重试。"); return; }
                owner->_status.stringValue=@"视频文件已复制，可以粘贴到支持文件的应用";
            } else owner->_status.stringValue=[@"已保存：" stringByAppendingString:destination.path];
            owner->_status.toolTip=owner->_status.stringValue;
            if (!owner->_preferences.keepEditor) [owner close];
        });
    });
}
- (void)windowWillClose:(NSNotification*)notification {
    _closed=YES; [_player pause]; [_exporter cancel];
    if (_timeObserver) { [_player removeTimeObserver:_timeObserver]; _timeObserver=nil; }
    if (_keyMonitor) { [NSEvent removeMonitor:_keyMonitor]; _keyMonitor=nil; }
    _root.resized=nil; _videoLayer.player=nil; [_player replaceCurrentItemWithPlayerItem:nil];
    if (self.closed) self.closed();
}
- (void)writeUISnapshot:(NSURL*)url {
    [self layoutInterface]; [_root layoutSubtreeIfNeeded];
    AVAssetImageGenerator* generator=[AVAssetImageGenerator assetImageGeneratorWithAsset:_player.currentItem.asset];
    CGImageRef image=[generator copyCGImageAtTime:kCMTimeZero actualTime:nullptr error:nil];
    NSImageView* still=[[NSImageView alloc] initWithFrame:_stage.bounds];
    still.imageScaling=NSImageScaleProportionallyUpOrDown;
    if (image) { still.image=[[NSImage alloc] initWithCGImage:image size:NSZeroSize]; CGImageRelease(image); }
    [_stage addSubview:still positioned:NSWindowBelow relativeTo:_canvas];
    NSBitmapImageRep* bitmap=[_root bitmapImageRepForCachingDisplayInRect:_root.bounds];
    [_root cacheDisplayInRect:_root.bounds toBitmapImageRep:bitmap];
    [[bitmap representationUsingType:NSBitmapImageFileTypePNG properties:@{}] writeToURL:url atomically:YES];
    [still removeFromSuperview];
}
@end
