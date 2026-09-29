#import "editor/EditorInternal.h"
#import "common/Support.h"
#import <QuartzCore/QuartzCore.h>
#include <algorithm>
@implementation SRFlippedView
- (BOOL)isFlipped { return YES; }
- (void)layout { [super layout]; if (self.resized) self.resized(); }
@end
@implementation SRWidthSlider
- (void)scrollWheel:(NSEvent*)event {
    double step=event.hasPreciseScrollingDeltas?0.03:0.2;
    self.doubleValue=std::clamp(self.doubleValue+event.scrollingDeltaY*step,self.minValue,self.maxValue);
    [self sendAction:self.action to:self.target];
}
@end
@implementation SREditorController (Layout)
- (void)buildInterface {
    _root=[[SRFlippedView alloc] initWithFrame:self.window.contentView.bounds]; self.window.contentView=_root;
    sr::Surface(_root,0x121315);
    _header=[NSView new]; sr::Surface(_header,0x191B1E);
    _heading=sr::Label(@"裁剪与导出",16); _heading.font=[NSFont systemFontOfSize:16 weight:NSFontWeightSemibold];
    _metadata=sr::Label([NSString stringWithFormat:@"%.0f × %.0f  ·  %ld FPS  ·  总时长 %@",
         _recording.size.width,_recording.size.height,(long)_recording.fps,sr::TimeLabel(_duration)],12,YES);
    _sidebar=[NSView new]; sr::Surface(_sidebar,0x191B1E,8);
    _stage=[NSView new]; sr::Surface(_stage,0x060708,6); _stage.layer.masksToBounds=YES;
    _videoLayer=[AVPlayerLayer playerLayerWithPlayer:_player]; _videoLayer.videoGravity=AVLayerVideoGravityResizeAspect;
    [_stage.layer addSublayer:_videoLayer];
    _canvas=[[SRAnnotationView alloc] initWithFrame:NSZeroRect]; _canvas.document=&_document; _canvas.sourceSize=_recording.size;
    _canvas.autoresizingMask=NSViewWidthSizable|NSViewHeightSizable;
    [_stage addSubview:_canvas];
    _tools=[NSMutableArray new];
    NSArray* symbols=@[@"cursorarrow",@"pencil.tip",@"circle",@"arrow.up.right",@"textformat"];
    NSArray* names=@[@"选择标注",@"自由画笔",@"中心画圆",@"箭头",@"文字"];
    for (NSUInteger i=0;i<symbols.count;++i) {
        NSButton* button=sr::Button(@"",symbols[i],self,@selector(selectTool:));
        button.tag=i; button.toolTip=names[i]; [button setAccessibilityLabel:names[i]];
        [button setButtonType:NSButtonTypePushOnPushOff]; [_tools addObject:button]; [_sidebar addSubview:button];
    }
    _tools[0].state=NSControlStateValueOn;
    _undo=sr::Button(@"",@"arrow.uturn.backward",self,@selector(undo:)); _undo.toolTip=@"撤销 ⌘Z";
    _redo=sr::Button(@"",@"arrow.uturn.forward",self,@selector(redo:)); _redo.toolTip=@"重做 ⇧⌘Z";
    _delete=sr::Button(@"",@"trash",self,@selector(removeMark:)); _delete.toolTip=@"删除所选标注";
    _colorLabel=sr::Label(@"颜色",11,YES); _color=[NSColorWell new]; _color.color=sr::Color(0xFF595E);
    _color.target=self; _color.action=@selector(changeColor:); _color.colorWellStyle=NSColorWellStyleMinimal;
    _widthLabel=sr::Label(@"粗细 5.0",11,YES);
    _width=[[SRWidthSlider alloc] initWithFrame:NSZeroRect]; _width.minValue=1; _width.maxValue=24; _width.doubleValue=5;
    _width.continuous=YES; _width.target=self; _width.action=@selector(changeWidth:); _width.toolTip=@"线条粗细；悬停时可用滚轮微调";
    for (NSView* view in @[_undo,_redo,_delete,_colorLabel,_color,_widthLabel,_width]) [_sidebar addSubview:view];
    _rangeLabel=sr::Label(@"",12);
    _qualityLabel=sr::Label(@"画质",11,YES);
    _quality=[NSSlider sliderWithValue:_preferences.quality minValue:25 maxValue:100 target:self action:@selector(changeExport:)];
    _quality.continuous=YES; _quality.toolTip=@"输出尺寸比例，自动记住上次设置";
    _qualityValue=sr::Label(@"100%",11);
    _fileSize=sr::Label(@"",12,YES);
    _speedLabel=sr::Label(@"倍速",11,YES);
    _speed=[NSSlider sliderWithValue:1 minValue:0.1 maxValue:3 target:self action:@selector(changeExport:)]; _speed.continuous=YES;
    _speedValue=sr::Label(@"1.0×",11);
    _setStart=sr::Button(@"【",@"",self,@selector(setStart:)); _setStart.toolTip=@"把起点设为当前帧";
    _setEnd=sr::Button(@"】",@"",self,@selector(setEnd:)); _setEnd.toolTip=@"把终点设为当前帧";
    _timeline=[SRTimelineView new]; _timeline.duration=_duration; _timeline.start=0; _timeline.end=_duration;
    _timeline.minimumSpan=1.0/_recording.fps;
    _play=sr::Button(@"播放",@"play.fill",self,@selector(togglePlay:));
    _positionLabel=sr::Label(@"",12); _positionLabel.font=[NSFont monospacedDigitSystemFontOfSize:12 weight:NSFontWeightRegular];
    _soundLabel=sr::Label(@"声音",12,YES); _sound=[NSSwitch new]; _sound.target=self; _sound.action=@selector(changeExport:);
    _sound.enabled=_recording.audioURL!=nil; _sound.toolTip=@"加入录制时暂存的电脑声音；预览始终静音";
    _format=[NSSegmentedControl segmentedControlWithLabels:@[@"MP4",@"GIF"] trackingMode:NSSegmentSwitchTrackingSelectOne
                                                   target:self action:@selector(changeExport:)]; _format.selectedSegment=0;
    _copy=sr::Button(@"复制到剪贴板",@"doc.on.doc",self,@selector(copyVideo:));
    _save=sr::Button(@"保存到本地",@"arrow.down.to.line",self,@selector(saveVideo:)); _save.bezelColor=sr::Color(0x2F7442);
    _status=sr::Label(@"正在打开录制…",11,YES);
    NSArray* views=@[_header,_heading,_metadata,_sidebar,_stage,_rangeLabel,_qualityLabel,_quality,_qualityValue,
        _fileSize,_speedLabel,_speed,_speedValue,_setStart,_setEnd,_timeline,_play,_positionLabel,_soundLabel,_sound,_format,_copy,_save,_status];
    for (NSView* view in views) [_root addSubview:view];
    __weak SREditorController* owner=self;
    _root.resized=^{ [owner layoutInterface]; };
    [self layoutInterface];
}
- (void)layoutInterface {
    CGFloat w=_root.bounds.size.width,h=_root.bounds.size.height;
    CGFloat footer=h-74,timeline=footer-92,parameters=timeline-39,stageHeight=MAX(180,parameters-84);
    _header.frame=NSMakeRect(0,0,w,62); _heading.frame=NSMakeRect(20,12,240,22); _metadata.frame=NSMakeRect(20,37,w-40,18);
    _sidebar.frame=NSMakeRect(16,74,64,stageHeight); _stage.frame=NSMakeRect(92,74,w-112,stageHeight);
    [CATransaction begin]; [CATransaction setDisableActions:YES]; _videoLayer.frame=_stage.bounds; [CATransaction commit];
    _canvas.frame=_stage.bounds;
    for (NSUInteger i=0;i<_tools.count;++i) _tools[i].frame=NSMakeRect(14,stageHeight-44-i*42,36,34);
    // Sidebar is a normal (bottom-left) NSView; place tools top-down explicitly.
    CGFloat below=stageHeight-263;
    _undo.frame=NSMakeRect(3,below,20,24); _redo.frame=NSMakeRect(22,below,20,24); _delete.frame=NSMakeRect(41,below,20,24);
    _colorLabel.frame=NSMakeRect(7,below-40,30,18); _color.frame=NSMakeRect(38,below-43,22,24);
    _widthLabel.frame=NSMakeRect(7,below-75,54,18); _width.frame=NSMakeRect(5,below-99,54,20);
    CGFloat right=w-20;
    _setEnd.frame=NSMakeRect(right-32,parameters,32,28); _setStart.frame=NSMakeRect(right-68,parameters,32,28); right-=80;
    _speedValue.frame=NSMakeRect(right-37,parameters+6,37,18); _speed.frame=NSMakeRect(right-137,parameters+4,94,20);
    _speedLabel.frame=NSMakeRect(right-171,parameters+6,30,18); right-=184;
    _fileSize.frame=NSMakeRect(right-78,parameters+6,78,18); right-=92;
    _qualityValue.frame=NSMakeRect(right-36,parameters+6,36,18); _quality.frame=NSMakeRect(right-139,parameters+4,97,20);
    _qualityLabel.frame=NSMakeRect(right-172,parameters+6,30,18);
    _rangeLabel.frame=NSMakeRect(92,parameters+6,MAX(40,right-272),18);
    _timeline.frame=NSMakeRect(92,timeline,w-112,82);
    _play.frame=NSMakeRect(16,footer,82,34); _positionLabel.frame=NSMakeRect(108,footer+8,170,18);
    right=w-20; _save.frame=NSMakeRect(right-130,footer,130,34); right-=140;
    _copy.frame=NSMakeRect(right-140,footer,140,34); right-=150;
    _format.frame=NSMakeRect(right-108,footer+3,108,30); right-=123;
    _sound.frame=NSMakeRect(right-40,footer+4,40,28); _soundLabel.frame=NSMakeRect(right-74,footer+8,32,18);
    _status.frame=NSMakeRect(20,h-27,w-40,18);
}
@end
