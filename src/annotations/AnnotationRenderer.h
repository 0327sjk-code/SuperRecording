#pragma once

// Shared antialiased rasterizer for the editor overlay and encoded video/GIF frames.
#include "annotations/Annotation.h"
#include <span>

namespace qrec::annotations {
enum class Surface { OpaqueVideo, PremultipliedOverlay };
[[nodiscard]] bool Render(
    std::span<std::uint8_t> bgra, unsigned width, unsigned height, unsigned stride,
    const Snapshot& scene, Time time, Surface surface = Surface::OpaqueVideo,
    const Mark* draft = nullptr);
}  // namespace qrec::annotations
