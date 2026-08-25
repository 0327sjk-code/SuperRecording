#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>
#include <objidl.h>
#include <gdiplus.h>

#include <algorithm>
#include <memory>
#include <span>
#include <vector>

#pragma comment(lib, "gdiplus.lib")

namespace qrec::ui {

class GdiPlusRuntime final {
public:
    GdiPlusRuntime() noexcept {
        Gdiplus::GdiplusStartupInput input{};
        ready_ = Gdiplus::GdiplusStartup(&token_, &input, nullptr) == Gdiplus::Ok;
    }

    ~GdiPlusRuntime() {
        if (ready_) {
            Gdiplus::GdiplusShutdown(token_);
        }
    }

    GdiPlusRuntime(const GdiPlusRuntime&) = delete;
    GdiPlusRuntime& operator=(const GdiPlusRuntime&) = delete;

    [[nodiscard]] bool Ready() const noexcept { return ready_; }

private:
    ULONG_PTR token_{};
    bool ready_{};
};

inline GdiPlusRuntime& SharedGdiPlusRuntime() noexcept {
    static GdiPlusRuntime runtime;
    return runtime;
}

inline Gdiplus::Color ToGdiPlusColor(const COLORREF color, const BYTE alpha = 255) noexcept {
    return Gdiplus::Color(
        alpha,
        GetRValue(color),
        GetGValue(color),
        GetBValue(color));
}

class Canvas final {
public:
    explicit Canvas(const HDC device) {
        if (device == nullptr || !SharedGdiPlusRuntime().Ready()) {
            return;
        }
        graphics_ = std::make_unique<Gdiplus::Graphics>(device);
        if (graphics_->GetLastStatus() != Gdiplus::Ok) {
            graphics_.reset();
            return;
        }
        graphics_->SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        graphics_->SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);
        graphics_->SetCompositingQuality(Gdiplus::CompositingQualityHighQuality);
    }

    Canvas(const Canvas&) = delete;
    Canvas& operator=(const Canvas&) = delete;

    [[nodiscard]] bool Valid() const noexcept { return graphics_ != nullptr; }

    void FillRectangle(
        const RECT& bounds,
        const COLORREF color,
        const BYTE alpha = 255) {
        if (!Valid()) {
            return;
        }
        Gdiplus::SolidBrush brush(ToGdiPlusColor(color, alpha));
        const Gdiplus::RectF rectangle = ToRect(bounds);
        graphics_->FillRectangle(&brush, rectangle);
    }

    void FillRoundedRectangle(
        const RECT& bounds,
        const float radius,
        const COLORREF color,
        const BYTE alpha = 255) {
        if (!Valid()) {
            return;
        }
        Gdiplus::GraphicsPath path;
        AddRoundedRectangle(path, ToRect(bounds), radius);
        Gdiplus::SolidBrush brush(ToGdiPlusColor(color, alpha));
        graphics_->FillPath(&brush, &path);
    }

    void StrokeRoundedRectangle(
        const RECT& bounds,
        const float radius,
        const COLORREF color,
        const float width = 1.0F,
        const BYTE alpha = 255) {
        if (!Valid()) {
            return;
        }
        const float safeWidth = std::max(1.0F, width);
        Gdiplus::RectF rectangle = ToRect(bounds);
        const float inset = safeWidth * 0.5F;
        rectangle.X += inset;
        rectangle.Y += inset;
        rectangle.Width = std::max(1.0F, rectangle.Width - safeWidth);
        rectangle.Height = std::max(1.0F, rectangle.Height - safeWidth);
        Gdiplus::GraphicsPath path;
        AddRoundedRectangle(path, rectangle, std::max(0.0F, radius - inset));
        Gdiplus::Pen pen(ToGdiPlusColor(color, alpha), safeWidth);
        pen.SetLineJoin(Gdiplus::LineJoinRound);
        graphics_->DrawPath(&pen, &path);
    }

    void DrawRoundedRectangle(
        const RECT& bounds,
        const float radius,
        const COLORREF fill,
        const COLORREF border,
        const float borderWidth = 1.0F) {
        FillRoundedRectangle(bounds, radius, fill);
        StrokeRoundedRectangle(bounds, radius, border, borderWidth);
    }

    void FillEllipse(
        const float centerX,
        const float centerY,
        const float radiusX,
        const float radiusY,
        const COLORREF color,
        const BYTE alpha = 255) {
        if (!Valid()) {
            return;
        }
        Gdiplus::SolidBrush brush(ToGdiPlusColor(color, alpha));
        graphics_->FillEllipse(
            &brush,
            centerX - radiusX,
            centerY - radiusY,
            radiusX * 2.0F,
            radiusY * 2.0F);
    }

    void FillPolygon(
        const std::span<const POINT> points,
        const COLORREF color,
        const BYTE alpha = 255) {
        if (!Valid() || points.size() < 3) {
            return;
        }
        std::vector<Gdiplus::PointF> converted;
        converted.reserve(points.size());
        for (const POINT point : points) {
            converted.emplace_back(
                static_cast<float>(point.x),
                static_cast<float>(point.y));
        }
        Gdiplus::SolidBrush brush(ToGdiPlusColor(color, alpha));
        graphics_->FillPolygon(
            &brush,
            converted.data(),
            static_cast<INT>(converted.size()),
            Gdiplus::FillModeWinding);
    }

    void DrawLine(
        const float startX,
        const float startY,
        const float endX,
        const float endY,
        const COLORREF color,
        const float width = 1.0F,
        const bool rounded = true,
        const BYTE alpha = 255) {
        if (!Valid()) {
            return;
        }
        Gdiplus::Pen pen(ToGdiPlusColor(color, alpha), std::max(1.0F, width));
        if (rounded) {
            pen.SetStartCap(Gdiplus::LineCapRound);
            pen.SetEndCap(Gdiplus::LineCapRound);
            pen.SetLineJoin(Gdiplus::LineJoinRound);
        }
        graphics_->DrawLine(&pen, startX, startY, endX, endY);
    }

private:
    static Gdiplus::RectF ToRect(const RECT& bounds) noexcept {
        return Gdiplus::RectF(
            static_cast<float>(bounds.left),
            static_cast<float>(bounds.top),
            static_cast<float>(std::max<LONG>(1, bounds.right - bounds.left)),
            static_cast<float>(std::max<LONG>(1, bounds.bottom - bounds.top)));
    }

    static void AddRoundedRectangle(
        Gdiplus::GraphicsPath& path,
        const Gdiplus::RectF& rectangle,
        const float requestedRadius) {
        const float radius = std::clamp(
            requestedRadius,
            0.0F,
            std::min(rectangle.Width, rectangle.Height) * 0.5F);
        if (radius <= 0.0F) {
            path.AddRectangle(rectangle);
            return;
        }
        const float diameter = radius * 2.0F;
        path.AddArc(rectangle.X, rectangle.Y, diameter, diameter, 180.0F, 90.0F);
        path.AddArc(
            rectangle.GetRight() - diameter,
            rectangle.Y,
            diameter,
            diameter,
            270.0F,
            90.0F);
        path.AddArc(
            rectangle.GetRight() - diameter,
            rectangle.GetBottom() - diameter,
            diameter,
            diameter,
            0.0F,
            90.0F);
        path.AddArc(
            rectangle.X,
            rectangle.GetBottom() - diameter,
            diameter,
            diameter,
            90.0F,
            90.0F);
        path.CloseFigure();
    }

    std::unique_ptr<Gdiplus::Graphics> graphics_;
};

}  // namespace qrec::ui
