#pragma once
// Shared tapered arrow silhouette for preview, export and pointer hit testing.
#include "annotations/Annotation.h"
#include <array>
#include <algorithm>
#include <cmath>

namespace qrec::annotations {
inline std::array<Point,7> ArrowVertices(const Mark& mark) noexcept {
    std::array<Point,7> result{};
    if (mark.points.size()!=2) return result;
    const Point a=mark.points.front(), b=mark.points.back();
    const float length=std::hypot(b.x-a.x,b.y-a.y);
    if (length<0.01F) { result.fill(a); return result; }
    const float ux=(b.x-a.x)/length, uy=(b.y-a.y)/length;
    const float head=std::min(length*0.42F,mark.strokeWidth*4.5F);
    const float wing=head*0.52F;
    const float neck=std::min(wing*0.48F,mark.strokeWidth*0.8F);
    const float tail=std::min(neck*0.30F,mark.strokeWidth*0.22F);
    const auto point=[&](float along,float across) {
        return Point{a.x+ux*along-uy*across,a.y+uy*along+ux*across};
    };
    result={point(0,tail),point(length-head,neck),point(length-head,wing),b,
        point(length-head,-wing),point(length-head,-neck),point(0,-tail)};
    return result;
}
}  // namespace qrec::annotations
