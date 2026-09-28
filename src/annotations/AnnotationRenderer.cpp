#include "annotations/AnnotationRenderer.h"
#include "annotations/ArrowGeometry.h"
#include "ui/AntiAliasedDrawing.h"

#include <cmath>
#include <limits>

namespace qrec::annotations {
namespace {
using namespace Gdiplus;

Status DrawMark(Graphics& graphics, const Mark& mark) {
    if (mark.points.empty()) return Ok;
    const Point start=mark.points.front();
    Pen pen(Color(mark.argb),mark.strokeWidth);
    pen.SetStartCap(LineCapRound); pen.SetEndCap(LineCapRound); pen.SetLineJoin(LineJoinRound);
    if (mark.tool==Tool::Circle && mark.points.size()==2) {
        const Point end=mark.points.back();
        const float radius=std::hypot(end.x-start.x,end.y-start.y);
        if (radius<0.01F) return Ok;
        return graphics.DrawEllipse(&pen,start.x-radius,start.y-radius,radius*2,radius*2);
    }
    if (mark.tool==Tool::Text) {
        if (mark.text.empty()) return Ok;
        FontFamily family(L"Microsoft YaHei UI");
        const FontFamily* selectedFamily = family.GetLastStatus()==Ok ? &family : FontFamily::GenericSansSerif();
        GraphicsPath path;
        StringFormat format(StringFormat::GenericTypographic());
        format.SetFormatFlags(StringFormatFlagsNoClip | StringFormatFlagsMeasureTrailingSpaces);
        Status result=path.AddString(mark.text.c_str(),static_cast<INT>(mark.text.size()),
            selectedFamily,FontStyleRegular,mark.fontSize,PointF(start.x,start.y),&format);
        if (result!=Ok) return result;
        Pen outline(Color(190,10,10,12),std::max(1.0F,mark.fontSize*0.065F));
        outline.SetLineJoin(LineJoinRound);
        graphics.DrawPath(&outline,&path);
        SolidBrush brush(Color(mark.argb));
        return graphics.FillPath(&brush,&path);
    }
    if (mark.tool==Tool::Arrow && mark.points.size()==2) {
        const auto shape=ArrowVertices(mark);
        std::array<PointF,7> vertices{};
        for (std::size_t i=0;i<shape.size();++i) vertices[i]={shape[i].x,shape[i].y};
        SolidBrush brush(Color(mark.argb));
        return graphics.FillPolygon(&brush,vertices.data(),static_cast<INT>(vertices.size()));
    }
    if (mark.points.size()==1) {
        SolidBrush brush(Color(mark.argb)); const float r=mark.strokeWidth*0.5F;
        return graphics.FillEllipse(&brush,start.x-r,start.y-r,r*2,r*2);
    }
    std::vector<PointF> points; points.reserve(mark.points.size());
    for (Point p : mark.points) points.emplace_back(p.x,p.y);
    return graphics.DrawLines(&pen,points.data(),static_cast<INT>(points.size()));
}
}  // namespace

bool Render(std::span<std::uint8_t> bgra, unsigned width, unsigned height, unsigned stride,
    const Snapshot& scene, Time time, Surface surface, const Mark* draft) {
    if (!scene || (scene->Marks().empty() && draft==nullptr)) return true;
    if (width==0 || height==0 || width>16'384 || height>16'384 || stride<width*4U ||
        stride>static_cast<unsigned>(std::numeric_limits<int>::max()) ||
        bgra.size()<static_cast<std::size_t>(stride)*height || !ui::SharedGdiPlusRuntime().Ready()) return false;
    const Size canvas=scene->Canvas();
    const auto format = surface==Surface::OpaqueVideo ? PixelFormat32bppRGB : PixelFormat32bppPARGB;
    Gdiplus::Bitmap bitmap(static_cast<INT>(width),static_cast<INT>(height),static_cast<INT>(stride),format,bgra.data());
    Gdiplus::Graphics graphics(&bitmap);
    if (bitmap.GetLastStatus()!=Gdiplus::Ok || graphics.GetLastStatus()!=Gdiplus::Ok) return false;
    graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    graphics.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);
    graphics.SetCompositingQuality(Gdiplus::CompositingQualityHighQuality);
    graphics.ScaleTransform(static_cast<float>(width)/canvas.width,static_cast<float>(height)/canvas.height);
    for (const Mark& mark : scene->Marks()) {
        if (draft!=nullptr && draft->id!=0 && draft->id==mark.id) continue;
        if (!Visible(mark,time)) continue;
        if (DrawMark(graphics,mark)!=Gdiplus::Ok) return false;
    }
    if (draft!=nullptr && Visible(*draft,time)) {
        if (DrawMark(graphics,*draft)!=Gdiplus::Ok) return false;
    }
    graphics.Flush(Gdiplus::FlushIntentionSync);
    return graphics.GetLastStatus()==Gdiplus::Ok;
}
}  // namespace qrec::annotations
