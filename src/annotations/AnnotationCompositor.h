#pragma once
// Rasterize only when visible marks change; blend only nontransparent runs per frame.
#include "annotations/AnnotationRenderer.h"
namespace qrec::annotations {
class FrameCompositor final {
public:
    explicit FrameCompositor(Snapshot scene) : scene_(std::move(scene)) {}
    [[nodiscard]] bool Apply(std::span<std::uint8_t> pixels,unsigned width,unsigned height,unsigned stride,Time time);
private:
    struct Run final { std::size_t offset{},count{}; bool opaque{}; };
    Snapshot scene_;
    std::vector<std::uint64_t> active_;
    std::vector<std::uint8_t> overlay_;
    std::vector<Run> runs_;
    unsigned width_{},height_{},stride_{};
};
}
