#pragma once
// Transparent direct-manipulation canvas; document coordinates stay in source pixels.
#import <AppKit/AppKit.h>
#include "annotations/Annotation.h"
@interface SRAnnotationView : NSView <NSTextViewDelegate>
@property(nonatomic) qrec::annotations::Document* document;
@property(nonatomic) qrec::annotations::Tool tool;
@property(nonatomic) double time;
@property CGFloat strokeWidth;
@property uint32_t argb;
@property uint64_t selectedID;
@property BOOL locked;
@property NSSize sourceSize;
@property(copy) void (^selectionChanged)(void);
@property(copy) void (^edited)(void);
@property(copy) void (^willInteract)(void);
- (NSRect)videoRect;
- (void)finishText:(BOOL)commit;
- (void)deleteSelection;
@end
