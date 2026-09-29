#pragma once
// One Quartz renderer is shared by the interactive canvas and the export compositor.
#import <CoreGraphics/CoreGraphics.h>
#include "annotations/Annotation.h"
namespace sr {
void DrawMark(CGContextRef context, const qrec::annotations::Mark& mark);
void DrawScene(CGContextRef context, const qrec::annotations::Snapshot& scene, double time,
               std::uint64_t skip = 0);
CGImageRef AnnotationImage(const qrec::annotations::Snapshot& scene, double time) CF_RETURNS_RETAINED;
}
