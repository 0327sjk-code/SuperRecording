#pragma once
// Single video trim rail plus the currently selected annotation's interval (no empty track).
#import <AppKit/AppKit.h>
typedef NS_ENUM(NSInteger, SRTimelineChange) { SRTimelineSeek, SRTimelineTrim, SRTimelineAnnotation };
@interface SRTimelineView : NSView
@property double duration;
@property double start;
@property double end;
@property double position;
@property double minimumSpan;
@property BOOL hasAnnotation;
@property BOOL locked;
@property double annotationStart;
@property double annotationEnd;
@property(copy) void (^changed)(SRTimelineChange,BOOL);
@end
