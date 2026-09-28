#include "editor/AnnotationIcons.h"
#include "ui/AntiAliasedDrawing.h"
#include <array>

namespace qrec {
void DrawAnnotationIcon(HDC dc, RECT bounds, EditorButtonRole role, COLORREF color) {
    if (!ui::SharedGdiPlusRuntime().Ready()) return;
    Gdiplus::Graphics graphics(dc);
    graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    graphics.TranslateTransform(static_cast<float>(bounds.left),static_cast<float>(bounds.top));
    graphics.ScaleTransform(static_cast<float>(bounds.right-bounds.left)/20.0F,
        static_cast<float>(bounds.bottom-bounds.top)/20.0F);
    Gdiplus::Pen pen(ui::ToGdiPlusColor(color),1.65F);
    pen.SetStartCap(Gdiplus::LineCapRound); pen.SetEndCap(Gdiplus::LineCapRound);
    pen.SetLineJoin(Gdiplus::LineJoinRound);
    Gdiplus::SolidBrush brush(ui::ToGdiPlusColor(color));
    switch (role) {
    case EditorButtonRole::AnnotationSelect: {
        const Gdiplus::PointF p[]{{4,2},{15,10},{10,11},{8,17},{4,2}};
        graphics.DrawLines(&pen,p,5); break;
    }
    case EditorButtonRole::AnnotationPen:
        graphics.DrawBezier(&pen,2.0F,15.0F,3.0F,1.0F,9.0F,2.0F,7.0F,10.0F);
        graphics.DrawBezier(&pen,7.0F,10.0F,4.0F,20.0F,14.0F,16.0F,17.0F,5.0F); break;
    case EditorButtonRole::AnnotationCircle: graphics.DrawEllipse(&pen,3.0F,3.0F,14.0F,14.0F); break;
    case EditorButtonRole::AnnotationArrow:
        graphics.DrawLine(&pen,3.0F,16.0F,16.0F,3.0F);
        graphics.DrawLine(&pen,8.0F,3.0F,16.0F,3.0F);
        graphics.DrawLine(&pen,16.0F,3.0F,16.0F,11.0F); break;
    case EditorButtonRole::AnnotationText:
        graphics.DrawLine(&pen,3.0F,4.0F,17.0F,4.0F);
        graphics.DrawLine(&pen,10.0F,4.0F,10.0F,17.0F);
        graphics.DrawLine(&pen,7.0F,17.0F,13.0F,17.0F); break;
    case EditorButtonRole::AnnotationUndo:
    case EditorButtonRole::AnnotationRedo: {
        const bool redo=role==EditorButtonRole::AnnotationRedo;
        if (redo) { graphics.TranslateTransform(20.0F,0.0F); graphics.ScaleTransform(-1.0F,1.0F); }
        graphics.DrawBezier(&pen,4.0F,7.0F,17.0F,3.0F,19.0F,16.0F,8.0F,16.0F);
        graphics.DrawLine(&pen,4.0F,7.0F,8.0F,3.0F);
        graphics.DrawLine(&pen,4.0F,7.0F,8.0F,11.0F); break;
    }
    case EditorButtonRole::AnnotationDelete:
        graphics.DrawLine(&pen,3.0F,5.0F,17.0F,5.0F);
        graphics.DrawLine(&pen,7.0F,2.0F,13.0F,2.0F);
        graphics.DrawLine(&pen,5.0F,5.0F,6.0F,17.0F);
        graphics.DrawLine(&pen,6.0F,17.0F,14.0F,17.0F);
        graphics.DrawLine(&pen,14.0F,17.0F,15.0F,5.0F);
        graphics.DrawLine(&pen,8.0F,8.0F,8.0F,14.0F);
        graphics.DrawLine(&pen,12.0F,8.0F,12.0F,14.0F); break;
    case EditorButtonRole::AnnotationColor: graphics.FillEllipse(&brush,3.0F,3.0F,14.0F,14.0F); break;
    default: break;
    }
}
}
