#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>
#include <dxgicommon.h>
#include <dxgitype.h>

#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>

namespace qrec::capture::desktop_frame_transform {

inline constexpr std::uint32_t BytesPerPixel = 4;

struct Extent final {
    std::uint32_t width{};
    std::uint32_t height{};

    [[nodiscard]] constexpr bool IsValid() const noexcept {
        return width != 0 && height != 0;
    }

    [[nodiscard]] constexpr bool operator==(const Extent&) const noexcept = default;
};

struct HalfOpenRect final {
    std::uint32_t left{};
    std::uint32_t top{};
    std::uint32_t right{};
    std::uint32_t bottom{};

    [[nodiscard]] constexpr std::uint32_t Width() const noexcept {
        return right >= left ? right - left : 0;
    }

    [[nodiscard]] constexpr std::uint32_t Height() const noexcept {
        return bottom >= top ? bottom - top : 0;
    }

    [[nodiscard]] constexpr bool IsValid() const noexcept {
        return right > left && bottom > top;
    }

    [[nodiscard]] constexpr bool operator==(const HalfOpenRect&) const noexcept = default;
};

struct SignedHalfOpenRect final {
    std::int64_t left{};
    std::int64_t top{};
    std::int64_t right{};
    std::int64_t bottom{};

    [[nodiscard]] constexpr bool IsValid() const noexcept {
        return right > left && bottom > top;
    }
};

struct LocalRectMapping final {
    HalfOpenRect desktopRect{};
    bool valid{};
};

struct RotationLayout final {
    DXGI_MODE_ROTATION rotation{DXGI_MODE_ROTATION_UNSPECIFIED};
    Extent desktop{};
    Extent texture{};
};

struct RectMapping final {
    HalfOpenRect textureRect{};
    bool valid{};
};

struct PixelPoint final {
    std::uint32_t x{};
    std::uint32_t y{};

    [[nodiscard]] constexpr bool operator==(const PixelPoint&) const noexcept = default;
};

struct PointMapping final {
    PixelPoint desktopPoint{};
    bool valid{};
};

[[nodiscard]] constexpr LocalRectMapping MapScreenRectToOutputLocal(
    const SignedHalfOpenRect output,
    const SignedHalfOpenRect selection) noexcept {
    if (!output.IsValid() || !selection.IsValid() ||
        selection.left < output.left || selection.top < output.top ||
        selection.right > output.right || selection.bottom > output.bottom) {
        return {};
    }

    const std::int64_t left = selection.left - output.left;
    const std::int64_t top = selection.top - output.top;
    const std::int64_t right = selection.right - output.left;
    const std::int64_t bottom = selection.bottom - output.top;
    constexpr std::int64_t maximum =
        static_cast<std::int64_t>(std::numeric_limits<std::uint32_t>::max());
    if (left < 0 || top < 0 || right > maximum || bottom > maximum) {
        return {};
    }

    const HalfOpenRect local{
        static_cast<std::uint32_t>(left),
        static_cast<std::uint32_t>(top),
        static_cast<std::uint32_t>(right),
        static_cast<std::uint32_t>(bottom)};
    return {local, local.IsValid()};
}

[[nodiscard]] constexpr bool IsSupportedRotation(
    const DXGI_MODE_ROTATION rotation) noexcept {
    switch (rotation) {
    case DXGI_MODE_ROTATION_UNSPECIFIED:
    case DXGI_MODE_ROTATION_IDENTITY:
    case DXGI_MODE_ROTATION_ROTATE90:
    case DXGI_MODE_ROTATION_ROTATE180:
    case DXGI_MODE_ROTATION_ROTATE270:
        return true;
    default:
        return false;
    }
}

[[nodiscard]] constexpr Extent ExpectedTextureExtent(
    const Extent desktop,
    const DXGI_MODE_ROTATION rotation) noexcept {
    switch (rotation) {
    case DXGI_MODE_ROTATION_UNSPECIFIED:
    case DXGI_MODE_ROTATION_IDENTITY:
    case DXGI_MODE_ROTATION_ROTATE180:
        return desktop;
    case DXGI_MODE_ROTATION_ROTATE90:
    case DXGI_MODE_ROTATION_ROTATE270:
        return {desktop.height, desktop.width};
    default:
        return {};
    }
}

[[nodiscard]] constexpr bool IsValidRotationLayout(
    const RotationLayout& layout) noexcept {
    return layout.desktop.IsValid() && layout.texture.IsValid() &&
        IsSupportedRotation(layout.rotation) &&
        ExpectedTextureExtent(layout.desktop, layout.rotation) == layout.texture;
}

[[nodiscard]] constexpr RectMapping MapDesktopRectToTexture(
    const RotationLayout& layout,
    const HalfOpenRect desktopRect) noexcept {
    if (!IsValidRotationLayout(layout) || !desktopRect.IsValid() ||
        desktopRect.right > layout.desktop.width ||
        desktopRect.bottom > layout.desktop.height) {
        return {};
    }

    HalfOpenRect textureRect{};
    switch (layout.rotation) {
    case DXGI_MODE_ROTATION_UNSPECIFIED:
    case DXGI_MODE_ROTATION_IDENTITY:
        textureRect = desktopRect;
        break;
    case DXGI_MODE_ROTATION_ROTATE90:
        textureRect = {
            desktopRect.top,
            layout.texture.height - desktopRect.right,
            desktopRect.bottom,
            layout.texture.height - desktopRect.left};
        break;
    case DXGI_MODE_ROTATION_ROTATE180:
        textureRect = {
            layout.texture.width - desktopRect.right,
            layout.texture.height - desktopRect.bottom,
            layout.texture.width - desktopRect.left,
            layout.texture.height - desktopRect.top};
        break;
    case DXGI_MODE_ROTATION_ROTATE270:
        textureRect = {
            layout.texture.width - desktopRect.bottom,
            desktopRect.left,
            layout.texture.width - desktopRect.top,
            desktopRect.right};
        break;
    default:
        return {};
    }

    const bool insideTexture = textureRect.IsValid() &&
        textureRect.right <= layout.texture.width &&
        textureRect.bottom <= layout.texture.height;
    return {textureRect, insideTexture};
}

[[nodiscard]] constexpr PointMapping MapTexturePointToDesktop(
    const RotationLayout& layout,
    const PixelPoint texturePoint) noexcept {
    if (!IsValidRotationLayout(layout) ||
        texturePoint.x >= layout.texture.width ||
        texturePoint.y >= layout.texture.height) {
        return {};
    }

    PixelPoint desktopPoint{};
    switch (layout.rotation) {
    case DXGI_MODE_ROTATION_UNSPECIFIED:
    case DXGI_MODE_ROTATION_IDENTITY:
        desktopPoint = texturePoint;
        break;
    case DXGI_MODE_ROTATION_ROTATE90:
        desktopPoint = {
            layout.texture.height - 1 - texturePoint.y,
            texturePoint.x};
        break;
    case DXGI_MODE_ROTATION_ROTATE180:
        desktopPoint = {
            layout.texture.width - 1 - texturePoint.x,
            layout.texture.height - 1 - texturePoint.y};
        break;
    case DXGI_MODE_ROTATION_ROTATE270:
        desktopPoint = {
            texturePoint.y,
            layout.texture.width - 1 - texturePoint.x};
        break;
    default:
        return {};
    }
    const bool insideDesktop = desktopPoint.x < layout.desktop.width &&
        desktopPoint.y < layout.desktop.height;
    return {desktopPoint, insideDesktop};
}

enum class BgraTransformStatus : std::uint8_t {
    Success,
    InvalidDimensions,
    UnsupportedRotation,
    ArithmeticOverflow,
    InvalidSourceRowPitch,
    DestinationTooSmall,
};

// Copies one mapped BGRA8 raw-texture ROI into a packed, top-down desktop-
// oriented frame. For 90/270-degree outputs, sourceExtent is the transposed
// raw ROI and destinationExtent is the user-visible selection extent.
[[nodiscard]] BgraTransformStatus CopyMappedBgraToDesktop(
    const void* source,
    std::size_t sourceRowPitch,
    Extent sourceExtent,
    DXGI_MODE_ROTATION rotation,
    std::span<std::uint8_t> destination,
    std::uint32_t destinationStride,
    Extent destinationExtent) noexcept;

}  // namespace qrec::capture::desktop_frame_transform
