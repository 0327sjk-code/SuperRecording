#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>

#include <cstdint>
#include <memory>
#include <span>
#include <vector>

namespace qrec::media {

// Reusable high-quality BGRA scaler. The COM apartment must be initialized on
// the calling thread before Initialize() is invoked.
class BgraFrameScaler final {
public:
    BgraFrameScaler();
    ~BgraFrameScaler();

    BgraFrameScaler(const BgraFrameScaler&) = delete;
    BgraFrameScaler& operator=(const BgraFrameScaler&) = delete;

    [[nodiscard]] HRESULT Initialize() noexcept;
    [[nodiscard]] HRESULT Scale(
        std::span<const std::uint8_t> sourcePixels,
        std::uint32_t sourceWidth,
        std::uint32_t sourceHeight,
        std::uint32_t sourceStride,
        std::uint32_t outputWidth,
        std::uint32_t outputHeight,
        std::vector<std::uint8_t>* outputPixels) const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace qrec::media
