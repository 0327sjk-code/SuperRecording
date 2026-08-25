#include "capture/DesktopFrameTransform.h"

#include <algorithm>
#include <cstring>
#include <limits>

namespace qrec::capture::desktop_frame_transform {
namespace {

constexpr RotationLayout kIdentityLayout{
    DXGI_MODE_ROTATION_IDENTITY,
    {8, 6},
    {8, 6}};
constexpr RotationLayout kUnspecifiedLayout{
    DXGI_MODE_ROTATION_UNSPECIFIED,
    {8, 6},
    {8, 6}};
constexpr RotationLayout kRotate90Layout{
    DXGI_MODE_ROTATION_ROTATE90,
    {4, 6},
    {6, 4}};
constexpr RotationLayout kRotate180Layout{
    DXGI_MODE_ROTATION_ROTATE180,
    {8, 6},
    {8, 6}};
constexpr RotationLayout kRotate270Layout{
    DXGI_MODE_ROTATION_ROTATE270,
    {4, 6},
    {6, 4}};

static_assert(IsValidRotationLayout(kIdentityLayout));
static_assert(IsValidRotationLayout(kUnspecifiedLayout));
static_assert(IsValidRotationLayout(kRotate90Layout));
static_assert(IsValidRotationLayout(kRotate180Layout));
static_assert(IsValidRotationLayout(kRotate270Layout));
static_assert(!IsValidRotationLayout({DXGI_MODE_ROTATION_ROTATE90, {4, 6}, {4, 6}}));
static_assert(
    MapScreenRectToOutputLocal({-1920, -120, 0, 960}, {-1800, 30, -200, 900})
        .desktopRect == HalfOpenRect{120, 150, 1720, 1020});
static_assert(
    !MapScreenRectToOutputLocal({-1920, -120, 0, 960}, {-2000, 30, -200, 900})
         .valid);
static_assert(ExpectedTextureExtent({4, 6}, DXGI_MODE_ROTATION_ROTATE90) == Extent{6, 4});
static_assert(ExpectedTextureExtent({4, 6}, DXGI_MODE_ROTATION_ROTATE270) == Extent{6, 4});
static_assert(
    MapDesktopRectToTexture(kIdentityLayout, {1, 2, 7, 5}).textureRect ==
    HalfOpenRect{1, 2, 7, 5});
static_assert(
    MapDesktopRectToTexture(kUnspecifiedLayout, {1, 2, 7, 5}).textureRect ==
    HalfOpenRect{1, 2, 7, 5});
static_assert(
    MapDesktopRectToTexture(kRotate90Layout, {1, 2, 4, 5}).textureRect ==
    HalfOpenRect{2, 0, 5, 3});
static_assert(
    MapDesktopRectToTexture(kRotate180Layout, {1, 0, 5, 3}).textureRect ==
    HalfOpenRect{3, 3, 7, 6});
static_assert(
    MapDesktopRectToTexture(kRotate270Layout, {1, 2, 4, 5}).textureRect ==
    HalfOpenRect{1, 1, 4, 4});
static_assert(
    MapDesktopRectToTexture(kRotate90Layout, {0, 0, 4, 6}).textureRect ==
    HalfOpenRect{0, 0, 6, 4});
static_assert(!MapDesktopRectToTexture(kRotate90Layout, {0, 0, 5, 6}).valid);
static_assert(
    MapTexturePointToDesktop(kIdentityLayout, {2, 3}).desktopPoint == PixelPoint{2, 3});
static_assert(
    MapTexturePointToDesktop(kUnspecifiedLayout, {2, 3}).desktopPoint == PixelPoint{2, 3});
static_assert(
    MapTexturePointToDesktop(kRotate90Layout, {0, 0}).desktopPoint == PixelPoint{3, 0});
static_assert(
    MapTexturePointToDesktop(kRotate90Layout, {5, 3}).desktopPoint == PixelPoint{0, 5});
static_assert(
    MapTexturePointToDesktop(kRotate180Layout, {0, 0}).desktopPoint == PixelPoint{7, 5});
static_assert(
    MapTexturePointToDesktop(kRotate270Layout, {0, 0}).desktopPoint == PixelPoint{0, 5});
static_assert(
    MapTexturePointToDesktop(kRotate270Layout, {5, 3}).desktopPoint == PixelPoint{3, 0});

constexpr std::uint32_t kCacheTileSize = 32;

[[nodiscard]] bool CheckedMultiply(
    const std::size_t left,
    const std::size_t right,
    std::size_t& product) noexcept {
    if (left != 0 && right > std::numeric_limits<std::size_t>::max() / left) {
        return false;
    }
    product = left * right;
    return true;
}

[[nodiscard]] bool ValidateBufferSpan(
    const std::size_t stride,
    const std::size_t rowBytes,
    const std::uint32_t height,
    std::size_t& requiredBytes) noexcept {
    if (height == 0 || stride < rowBytes) {
        return false;
    }
    std::size_t precedingRowsBytes{};
    if (!CheckedMultiply(stride, static_cast<std::size_t>(height - 1), precedingRowsBytes) ||
        rowBytes > std::numeric_limits<std::size_t>::max() - precedingRowsBytes) {
        return false;
    }
    requiredBytes = precedingRowsBytes + rowBytes;
    return true;
}

void CopyPixel(const std::uint8_t* source, std::uint8_t* destination) noexcept {
    std::uint32_t pixel{};
    std::memcpy(&pixel, source, sizeof(pixel));
    std::memcpy(destination, &pixel, sizeof(pixel));
}

}  // namespace

BgraTransformStatus CopyMappedBgraToDesktop(
    const void* source,
    const std::size_t sourceRowPitch,
    const Extent sourceExtent,
    const DXGI_MODE_ROTATION rotation,
    const std::span<std::uint8_t> destination,
    const std::uint32_t destinationStride,
    const Extent destinationExtent) noexcept {
    if (source == nullptr || !sourceExtent.IsValid() || !destinationExtent.IsValid()) {
        return BgraTransformStatus::InvalidDimensions;
    }
    if (!IsSupportedRotation(rotation)) {
        return BgraTransformStatus::UnsupportedRotation;
    }

    const Extent expectedSource = ExpectedTextureExtent(destinationExtent, rotation);
    if (sourceExtent != expectedSource) {
        return BgraTransformStatus::InvalidDimensions;
    }

    std::size_t sourceRowBytes{};
    std::size_t destinationRowBytes{};
    if (!CheckedMultiply(sourceExtent.width, BytesPerPixel, sourceRowBytes) ||
        !CheckedMultiply(destinationExtent.width, BytesPerPixel, destinationRowBytes) ||
        destinationRowBytes > std::numeric_limits<std::uint32_t>::max()) {
        return BgraTransformStatus::ArithmeticOverflow;
    }

    std::size_t sourceRequiredBytes{};
    if (!ValidateBufferSpan(
            sourceRowPitch,
            sourceRowBytes,
            sourceExtent.height,
            sourceRequiredBytes)) {
        return sourceRowPitch < sourceRowBytes
            ? BgraTransformStatus::InvalidSourceRowPitch
            : BgraTransformStatus::ArithmeticOverflow;
    }
    (void)sourceRequiredBytes;

    std::size_t destinationRequiredBytes{};
    if (!ValidateBufferSpan(
            destinationStride,
            destinationRowBytes,
            destinationExtent.height,
            destinationRequiredBytes)) {
        return destinationStride < destinationRowBytes
            ? BgraTransformStatus::DestinationTooSmall
            : BgraTransformStatus::ArithmeticOverflow;
    }
    if (destination.size() < destinationRequiredBytes) {
        return BgraTransformStatus::DestinationTooSmall;
    }

    const auto* sourceBytes = static_cast<const std::uint8_t*>(source);
    std::uint8_t* destinationBytes = destination.data();
    if (rotation == DXGI_MODE_ROTATION_UNSPECIFIED ||
        rotation == DXGI_MODE_ROTATION_IDENTITY) {
        for (std::uint32_t row = 0; row < destinationExtent.height; ++row) {
            std::memcpy(
                destinationBytes + static_cast<std::size_t>(row) * destinationStride,
                sourceBytes + static_cast<std::size_t>(row) * sourceRowPitch,
                destinationRowBytes);
        }
        return BgraTransformStatus::Success;
    }

    if (rotation == DXGI_MODE_ROTATION_ROTATE180) {
        for (std::uint32_t destinationY = 0;
             destinationY < destinationExtent.height;
             ++destinationY) {
            const std::uint32_t sourceY = sourceExtent.height - 1 - destinationY;
            const std::uint8_t* sourceRow =
                sourceBytes + static_cast<std::size_t>(sourceY) * sourceRowPitch;
            std::uint8_t* destinationRow =
                destinationBytes + static_cast<std::size_t>(destinationY) * destinationStride;
            for (std::uint32_t destinationX = 0;
                 destinationX < destinationExtent.width;
                 ++destinationX) {
                const std::uint32_t sourceX = sourceExtent.width - 1 - destinationX;
                CopyPixel(
                    sourceRow + static_cast<std::size_t>(sourceX) * BytesPerPixel,
                    destinationRow + static_cast<std::size_t>(destinationX) * BytesPerPixel);
            }
        }
        return BgraTransformStatus::Success;
    }

    // The 90/270-degree paths walk small source tiles. This bounds the working
    // set while converting row-major source pixels into column-major output.
    for (std::uint32_t tileY = 0; tileY < sourceExtent.height;) {
        const std::uint32_t sourceYEnd = tileY + std::min(
            kCacheTileSize, sourceExtent.height - tileY);
        for (std::uint32_t tileX = 0; tileX < sourceExtent.width;) {
            const std::uint32_t sourceXEnd = tileX + std::min(
                kCacheTileSize, sourceExtent.width - tileX);
            for (std::uint32_t sourceY = tileY; sourceY < sourceYEnd; ++sourceY) {
                const std::uint8_t* sourceRow =
                    sourceBytes + static_cast<std::size_t>(sourceY) * sourceRowPitch;
                for (std::uint32_t sourceX = tileX; sourceX < sourceXEnd; ++sourceX) {
                    std::uint32_t destinationX{};
                    std::uint32_t destinationY{};
                    if (rotation == DXGI_MODE_ROTATION_ROTATE90) {
                        destinationX = destinationExtent.width - 1 - sourceY;
                        destinationY = sourceX;
                    } else {
                        destinationX = sourceY;
                        destinationY = destinationExtent.height - 1 - sourceX;
                    }
                    CopyPixel(
                        sourceRow + static_cast<std::size_t>(sourceX) * BytesPerPixel,
                        destinationBytes +
                            static_cast<std::size_t>(destinationY) * destinationStride +
                            static_cast<std::size_t>(destinationX) * BytesPerPixel);
                }
            }
            tileX = sourceXEnd;
        }
        tileY = sourceYEnd;
    }
    return BgraTransformStatus::Success;
}

}  // namespace qrec::capture::desktop_frame_transform
