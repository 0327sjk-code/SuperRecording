#include "annotations/Annotation.h"
#include "annotations/ArrowGeometry.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <iomanip>
#include <sstream>
#include <stdexcept>

namespace qrec::annotations {
namespace {
constexpr std::size_t kMaximumMarks = 256;
constexpr std::size_t kMaximumScenePoints = 65'536;
constexpr std::size_t kMaximumHistory = 32;

float Distance(Point a, Point b) noexcept { return std::hypot(a.x - b.x, a.y - b.y); }
float SegmentDistance(Point p, Point a, Point b) noexcept {
    const float dx = b.x - a.x, dy = b.y - a.y;
    const float square = dx * dx + dy * dy;
    const float t = square > 0 ? std::clamp(((p.x-a.x)*dx+(p.y-a.y)*dy)/square, 0.0F, 1.0F) : 0;
    return Distance(p, {a.x + t * dx, a.y + t * dy});
}
bool FinitePositive(float value) noexcept { return std::isfinite(value) && value > 0; }

std::wstring Serialize(Size size, const std::vector<Mark>& marks) {
    if (marks.empty()) return {};
    // Keep the wire identity independent of std::format's OS availability.
    std::wostringstream stream;
    stream.imbue(std::locale::classic());
    const auto hex = [&](std::uint32_t value, int digits = 8) {
        stream << std::hex << std::setfill(L'0') << std::setw(digits) << value << std::dec;
    };
    stream << L"annotations-v2:";
    hex(std::bit_cast<std::uint32_t>(size.width)); stream << L':';
    hex(std::bit_cast<std::uint32_t>(size.height));
    for (const Mark& mark : marks) {
        stream << L'|' << static_cast<unsigned>(mark.tool) << L':' << mark.start.count()
               << L':' << mark.end.count() << L':';
        hex(mark.argb); stream << L':';
        hex(std::bit_cast<std::uint32_t>(mark.strokeWidth)); stream << L':';
        hex(std::bit_cast<std::uint32_t>(mark.fontSize)); stream << L':' << mark.text.size() << L':';
        // Hex encoding prevents text from impersonating manifest separators.
        for (wchar_t c : mark.text) hex(static_cast<unsigned>(c), 4);
        stream << L':' << mark.points.size();
        for (Point p : mark.points) {
            stream << L':'; hex(std::bit_cast<std::uint32_t>(p.x));
            stream << L','; hex(std::bit_cast<std::uint32_t>(p.y));
        }
    }
    return stream.str();
}
}  // namespace

Scene::Scene(Size canvas, std::vector<Mark> marks, std::wstring identity)
    : canvas_(canvas), marks_(std::move(marks)), identity_(std::move(identity)) {}

Snapshot Scene::Create(Size canvas, std::vector<Mark> marks) {
    if (!FinitePositive(canvas.width) || !FinitePositive(canvas.height) || marks.size() > kMaximumMarks)
        throw std::invalid_argument("Invalid annotation canvas or mark count");
    std::size_t totalPoints = 0;
    for (const Mark& mark : marks) {
        totalPoints += mark.points.size();
        if (mark.tool < Tool::Pen || mark.tool > Tool::Text || mark.points.empty() ||
            mark.points.size() > MaximumPoints || totalPoints > kMaximumScenePoints ||
            ((mark.tool == Tool::Circle || mark.tool == Tool::Arrow) && mark.points.size() != 2) ||
            (mark.tool == Tool::Text && (mark.points.size() != 1 || mark.text.empty())) ||
            mark.text.size() > MaximumTextLength || !FinitePositive(mark.strokeWidth) ||
            !FinitePositive(mark.fontSize) || mark.start < Time::zero() || mark.end <= mark.start)
            throw std::invalid_argument("Invalid annotation");
        for (Point p : mark.points) {
            if (!std::isfinite(p.x) || !std::isfinite(p.y) ||
                std::abs(p.x) > canvas.width * 4 || std::abs(p.y) > canvas.height * 4)
                throw std::invalid_argument("Invalid annotation point");
        }
    }
    auto identity = Serialize(canvas, marks);
    return Snapshot(new Scene(canvas, std::move(marks), std::move(identity)));
}

bool Visible(const Mark& mark, Time time) noexcept { return time >= mark.start && time < mark.end; }
bool HasVisibleMarks(const Snapshot& scene, Time start, Time end) noexcept {
    if (!scene) return false;
    return std::ranges::any_of(scene->Marks(), [=](const Mark& m) { return m.end > start && m.start < end; });
}

Bounds MeasureBounds(const Mark& mark) noexcept {
    if (mark.points.empty()) return {};
    const Point first = mark.points.front();
    Bounds bounds{first.x, first.y, first.x, first.y};
    if (mark.tool == Tool::Circle && mark.points.size() == 2) {
        const float radius = Distance(first, mark.points.back());
        bounds = {first.x-radius, first.y-radius, first.x+radius, first.y+radius};
    } else if (mark.tool == Tool::Arrow && mark.points.size()==2) {
        for (Point p : ArrowVertices(mark)) {
            bounds.left=std::min(bounds.left,p.x); bounds.top=std::min(bounds.top,p.y);
            bounds.right=std::max(bounds.right,p.x); bounds.bottom=std::max(bounds.bottom,p.y);
        }
    } else if (mark.tool == Tool::Text) {
        float lineWidth = 0, maximumWidth = 0;
        unsigned lines = 1;
        for (wchar_t c : mark.text) {
            if (c == L'\n') { maximumWidth = std::max(maximumWidth, lineWidth); lineWidth = 0; ++lines; }
            else if (c != L'\r') lineWidth += mark.fontSize * (c < 128 ? 0.65F : 1.0F);
        }
        bounds.right += std::max(maximumWidth, lineWidth);
        bounds.bottom += mark.fontSize * 1.4F * static_cast<float>(lines);
    } else {
        for (Point p : mark.points) {
            bounds.left=std::min(bounds.left,p.x); bounds.top=std::min(bounds.top,p.y);
            bounds.right=std::max(bounds.right,p.x); bounds.bottom=std::max(bounds.bottom,p.y);
        }
    }
    const float pad = mark.tool == Tool::Arrow ? 1.0F : mark.strokeWidth * 0.5F;
    bounds.left-=pad; bounds.top-=pad; bounds.right+=pad; bounds.bottom+=pad;
    return bounds;
}

bool HitTest(const Mark& mark, Point point, float tolerance) noexcept {
    if (mark.points.empty()) return false;
    const float threshold = tolerance + mark.strokeWidth * 0.5F;
    if (mark.tool == Tool::Circle && mark.points.size() == 2)
        return std::abs(Distance(point, mark.points.front()) - Distance(mark.points.front(),mark.points.back())) <= threshold;
    if (mark.tool == Tool::Text) {
        const Bounds b = MeasureBounds(mark);
        return point.x >= b.left-tolerance && point.x <= b.right+tolerance &&
            point.y >= b.top-tolerance && point.y <= b.bottom+tolerance;
    }
    if (mark.tool == Tool::Arrow && mark.points.size()==2) {
        const auto vertices=ArrowVertices(mark);
        bool inside=false;
        for (std::size_t i=0,j=vertices.size()-1;i<vertices.size();j=i++) {
            const auto a=vertices[i],b=vertices[j];
            if (SegmentDistance(point,a,b)<=tolerance) return true;
            if ((a.y>point.y)!=(b.y>point.y) &&
                point.x<(b.x-a.x)*(point.y-a.y)/(b.y-a.y)+a.x) inside=!inside;
        }
        return inside;
    }
    for (std::size_t i=1; i<mark.points.size(); ++i)
        if (SegmentDistance(point,mark.points[i-1],mark.points[i]) <= threshold) return true;
    return Distance(point,mark.points.front()) <= threshold;
}
void Translate(Mark& mark, Point delta) noexcept {
    for (Point& p : mark.points) { p.x+=delta.x; p.y+=delta.y; }
}

void Document::Reset(Size canvas, Time duration) {
    current_ = Scene::Create(canvas, {});
    duration_=std::max(duration,Time{1}); nextId_=1; undo_.clear(); redo_.clear();
}
Mark Document::NewMark(Tool tool, Time start) const {
    Mark mark{}; mark.tool=tool;
    mark.start=std::clamp(start,Time::zero(),duration_-Time{1});
    mark.end=std::min(duration_,mark.start+DefaultDuration);
    const Size size = current_ ? current_->Canvas() : Size{};
    const float shortEdge = std::min(size.width,size.height);
    mark.strokeWidth=std::max(2.0F,shortEdge*0.006F);
    mark.fontSize=std::max(16.0F,shortEdge*0.05F);
    return mark;
}
const Mark* Document::Find(std::uint64_t id) const noexcept {
    if (current_) for (const Mark& mark : current_->Marks()) if (mark.id==id) return &mark;
    return nullptr;
}
bool Document::Commit(std::vector<Mark> marks) {
    Snapshot next;
    try { next=Scene::Create(current_->Canvas(),std::move(marks)); }
    catch (const std::invalid_argument&) { return false; }
    if (next->Identity()==current_->Identity()) return false;
    undo_.push_back(current_);
    if (undo_.size()>kMaximumHistory) undo_.erase(undo_.begin());
    current_=std::move(next); redo_.clear(); return true;
}
bool Document::Put(Mark mark) {
    if (!current_) return false;
    mark.start=std::clamp(mark.start,Time::zero(),duration_-Time{1});
    mark.end=std::clamp(mark.end,mark.start+Time{1},duration_);
    auto marks=current_->Marks();
    for (Mark& existing : marks) if (existing.id==mark.id && mark.id!=0) {
        existing=std::move(mark); return Commit(std::move(marks));
    }
    mark.id=nextId_;
    marks.push_back(std::move(mark));
    if (!Commit(std::move(marks))) return false;
    ++nextId_; return true;
}
bool Document::Erase(std::uint64_t id) {
    if (!Find(id)) return false;
    auto marks=current_->Marks();
    std::erase_if(marks,[=](const Mark& m){return m.id==id;});
    return Commit(std::move(marks));
}
bool Document::Undo() {
    if (undo_.empty()) return false;
    redo_.push_back(current_); current_=undo_.back(); undo_.pop_back(); return true;
}
bool Document::Redo() {
    if (redo_.empty()) return false;
    undo_.push_back(current_); current_=redo_.back(); redo_.pop_back(); return true;
}
}  // namespace qrec::annotations
