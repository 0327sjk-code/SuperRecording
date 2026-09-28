#pragma once

// Source-space annotation model, immutable export snapshots and bounded undo history.
#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace qrec::annotations {

enum class Tool : std::uint8_t { Select, Pen, Circle, Arrow, Text };
using Time = std::chrono::milliseconds;
inline constexpr Time DefaultDuration{500};
inline constexpr std::size_t MaximumPoints = 16'384;
inline constexpr std::size_t MaximumTextLength = 2'048;

struct Point final {
    float x{};
    float y{};
    bool operator==(const Point&) const = default;
};
struct Size final { float width{1}; float height{1}; };
struct Bounds final { float left{}, top{}, right{}, bottom{}; };

struct Mark final {
    std::uint64_t id{};
    Tool tool{Tool::Pen};
    // Coordinates and widths always refer to the original recording, not the proxy.
    std::vector<Point> points;
    std::wstring text;
    std::uint32_t argb{0xFFFF595E};
    float strokeWidth{5};
    float fontSize{32};
    Time start{};
    Time end{DefaultDuration};
};

class Scene final {
public:
    [[nodiscard]] static std::shared_ptr<const Scene> Create(Size canvas, std::vector<Mark> marks);
    [[nodiscard]] Size Canvas() const noexcept { return canvas_; }
    [[nodiscard]] const std::vector<Mark>& Marks() const noexcept { return marks_; }
    [[nodiscard]] const std::wstring& Identity() const noexcept { return identity_; }
private:
    Scene(Size canvas, std::vector<Mark> marks, std::wstring identity);
    Size canvas_;
    std::vector<Mark> marks_;
    std::wstring identity_;
};
using Snapshot = std::shared_ptr<const Scene>;

[[nodiscard]] bool Visible(const Mark& mark, Time time) noexcept;
[[nodiscard]] bool HasVisibleMarks(const Snapshot& scene, Time start, Time end) noexcept;
[[nodiscard]] Bounds MeasureBounds(const Mark& mark) noexcept;
[[nodiscard]] bool HitTest(const Mark& mark, Point point, float tolerance) noexcept;
void Translate(Mark& mark, Point delta) noexcept;

class Document final {
public:
    void Reset(Size canvas, Time duration);
    [[nodiscard]] Mark NewMark(Tool tool, Time start) const;
    [[nodiscard]] const Mark* Find(std::uint64_t id) const noexcept;
    [[nodiscard]] bool Put(Mark mark);
    [[nodiscard]] bool Erase(std::uint64_t id);
    [[nodiscard]] bool Undo();
    [[nodiscard]] bool Redo();
    [[nodiscard]] bool CanUndo() const noexcept { return !undo_.empty(); }
    [[nodiscard]] bool CanRedo() const noexcept { return !redo_.empty(); }
    [[nodiscard]] Snapshot Current() const noexcept { return current_; }
    [[nodiscard]] Time Duration() const noexcept { return duration_; }
    [[nodiscard]] std::uint64_t LastId() const noexcept { return nextId_ - 1; }
private:
    bool Commit(std::vector<Mark> marks);
    Snapshot current_;
    std::vector<Snapshot> undo_;
    std::vector<Snapshot> redo_;
    Time duration_{1};
    std::uint64_t nextId_{1};
};

}  // namespace qrec::annotations
