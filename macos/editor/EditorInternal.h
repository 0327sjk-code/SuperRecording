#pragma once
// Internal editor composition and layout contract; never imported by capture/media layers.
#import "editor/EditorController.h"
#import "editor/AnnotationView.h"
#import "editor/TimelineView.h"
#import "media/ExportCoordinator.h"
#import <AVKit/AVKit.h>
@interface SRFlippedView : NSView
@property(copy) void (^resized)(void);
@end
@interface SRWidthSlider : NSSlider @end
@interface SREditorController () {
@public
    qrec::annotations::Document _document;
    SRRecording* _recording;
    SRPreferences* _preferences;
    SRExportCoordinator* _exporter;
    AVPlayer* _player;
    AVPlayerLayer* _videoLayer;
    id _timeObserver;
    id _keyMonitor;
    BOOL _closed;
    BOOL _seeking;
    double _pendingSeek;
    NSUInteger _previewGeneration;
    NSInteger _pendingDelivery;
    BOOL _delivering;
    NSURL* _saveDestination;
    double _duration;
    SRFlippedView* _root;
    NSView* _header;
    NSTextField* _heading;
    NSTextField* _metadata;
    NSView* _sidebar;
    NSView* _stage;
    SRAnnotationView* _canvas;
    NSMutableArray<NSButton*>* _tools;
    NSButton* _undo;
    NSButton* _redo;
    NSButton* _delete;
    NSColorWell* _color;
    NSTextField* _colorLabel;
    SRWidthSlider* _width;
    NSTextField* _widthLabel;
    NSTextField* _rangeLabel;
    NSTextField* _qualityLabel;
    NSSlider* _quality;
    NSTextField* _qualityValue;
    NSTextField* _fileSize;
    NSTextField* _speedLabel;
    NSSlider* _speed;
    NSTextField* _speedValue;
    NSButton* _setStart;
    NSButton* _setEnd;
    SRTimelineView* _timeline;
    NSButton* _play;
    NSTextField* _positionLabel;
    NSTextField* _soundLabel;
    NSSwitch* _sound;
    NSSegmentedControl* _format;
    NSButton* _copy;
    NSButton* _save;
    NSTextField* _status;
}
- (void)buildInterface;
- (void)layoutInterface;
- (void)refreshLabels;
- (void)selectTool:(NSButton*)sender;
- (void)undo:(id)sender;
- (void)redo:(id)sender;
- (void)removeMark:(id)sender;
- (void)changeColor:(id)sender;
- (void)changeWidth:(id)sender;
- (void)changeExport:(id)sender;
- (void)setStart:(id)sender;
- (void)setEnd:(id)sender;
- (void)togglePlay:(id)sender;
- (void)copyVideo:(id)sender;
- (void)saveVideo:(id)sender;
- (void)pause;
- (void)seek:(double)time;
- (void)prepareExport;
- (void)annotationSelection;
@end
