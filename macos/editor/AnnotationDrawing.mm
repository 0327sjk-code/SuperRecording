#import "editor/AnnotationDrawing.h"
#import "common/Support.h"
#import <CoreText/CoreText.h>
#include "annotations/ArrowGeometry.h"
#include <cmath>
namespace sr {
void DrawMark(CGContextRef context, const qrec::annotations::Mark& mark) {
    using qrec::annotations::Tool;
    if (mark.points.empty()) return;
    CGContextSaveGState(context);
    CGFloat components[]={((mark.argb>>16)&255)/255.0,((mark.argb>>8)&255)/255.0,
                          (mark.argb&255)/255.0,((mark.argb>>24)&255)/255.0};
    CGContextSetRGBStrokeColor(context,components[0],components[1],components[2],components[3]);
    CGContextSetRGBFillColor(context,components[0],components[1],components[2],components[3]);
    CGContextSetLineWidth(context,mark.strokeWidth); CGContextSetLineCap(context,kCGLineCapRound);
    CGContextSetLineJoin(context,kCGLineJoinRound);
    const auto start=mark.points.front();
    if (mark.tool==Tool::Circle && mark.points.size()==2) {
        float radius=std::hypot(start.x-mark.points.back().x,start.y-mark.points.back().y);
        CGContextStrokeEllipseInRect(context,CGRectMake(start.x-radius,start.y-radius,radius*2,radius*2));
    } else if (mark.tool==Tool::Arrow && mark.points.size()==2) {
        const auto points=qrec::annotations::ArrowVertices(mark);
        CGContextMoveToPoint(context,points[0].x,points[0].y);
        for (size_t i=1;i<points.size();++i) CGContextAddLineToPoint(context,points[i].x,points[i].y);
        CGContextClosePath(context); CGContextFillPath(context);
    } else if (mark.tool==Tool::Text) {
        CTFontRef font=CTFontCreateUIFontForLanguage(kCTFontUIFontSystem,mark.fontSize,CFSTR("zh-Hans"));
        CGColorSpaceRef space=CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
        CGColorRef color=CGColorCreate(space,components);
        NSDictionary* attributes=@{(__bridge NSString*)kCTFontAttributeName:(__bridge id)font,
            (__bridge NSString*)kCTForegroundColorAttributeName:(__bridge id)color};
        NSArray<NSString*>* lines=[String(mark.text) componentsSeparatedByString:@"\n"];
        CGContextTranslateCTM(context,start.x,start.y+CTFontGetAscent(font));
        CGContextScaleCTM(context,1,-1); CGContextSetTextMatrix(context,CGAffineTransformIdentity);
        CGFloat y=0;
        for (NSString* text in lines) {
            NSAttributedString* attributed=[[NSAttributedString alloc] initWithString:text attributes:attributes];
            CTLineRef line=CTLineCreateWithAttributedString((__bridge CFAttributedStringRef)attributed);
            CGContextSetTextPosition(context,0,y); CTLineDraw(line,context); CFRelease(line); y-=mark.fontSize*1.4;
        }
        CGColorRelease(color); CGColorSpaceRelease(space); CFRelease(font);
    } else {
        if (mark.points.size()==1) CGContextFillEllipseInRect(context,CGRectMake(start.x-mark.strokeWidth/2,
            start.y-mark.strokeWidth/2,mark.strokeWidth,mark.strokeWidth));
        else {
            CGContextMoveToPoint(context,start.x,start.y);
            for (size_t i=1;i<mark.points.size();++i) CGContextAddLineToPoint(context,mark.points[i].x,mark.points[i].y);
            CGContextStrokePath(context);
        }
    }
    CGContextRestoreGState(context);
}
void DrawScene(CGContextRef context,const qrec::annotations::Snapshot& scene,double time,std::uint64_t skip) {
    if (!scene) return;
    qrec::annotations::Time value{(int64_t)llround(time*1000)};
    for (const auto& mark:scene->Marks()) if (mark.id!=skip && qrec::annotations::Visible(mark,value)) DrawMark(context,mark);
}
CGImageRef AnnotationImage(const qrec::annotations::Snapshot& scene,double time) {
    if (!scene) return nullptr;
    const size_t width=(size_t)ceil(scene->Canvas().width),height=(size_t)ceil(scene->Canvas().height);
    CGColorSpaceRef space=CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
    CGContextRef context=CGBitmapContextCreate(nullptr,width,height,8,width*4,space,
        kCGImageAlphaPremultipliedLast|kCGBitmapByteOrder32Big);
    CGColorSpaceRelease(space); if (!context) return nullptr;
    CGContextTranslateCTM(context,0,height); CGContextScaleCTM(context,1,-1);
    DrawScene(context,scene,time);
    CGImageRef image=CGBitmapContextCreateImage(context); CGContextRelease(context); return image;
}
}
