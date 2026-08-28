#include "media/BgraFrameScaler.h"

#include <wincodec.h>
#include <wrl/client.h>

#include <cstring>
#include <limits>

namespace qrec::media {

using Microsoft::WRL::ComPtr;

struct BgraFrameScaler::Impl final {
    ComPtr<IWICImagingFactory> factory;
};

BgraFrameScaler::BgraFrameScaler() : impl_(std::make_unique<Impl>()) {}

BgraFrameScaler::~BgraFrameScaler() = default;

HRESULT BgraFrameScaler::Initialize() noexcept {
    impl_->factory.Reset();
    HRESULT result = ::CoCreateInstance(
        CLSID_WICImagingFactory2,
        nullptr,
        CLSCTX_INPROC_SERVER,
        IID_PPV_ARGS(&impl_->factory));
    if (FAILED(result)) {
        result = ::CoCreateInstance(
            CLSID_WICImagingFactory,
            nullptr,
            CLSCTX_INPROC_SERVER,
            IID_PPV_ARGS(&impl_->factory));
    }
    return result;
}

HRESULT BgraFrameScaler::Scale(
    const std::span<const std::uint8_t> sourcePixels,
    const std::uint32_t sourceWidth,
    const std::uint32_t sourceHeight,
    const std::uint32_t sourceStride,
    const std::uint32_t outputWidth,
    const std::uint32_t outputHeight,
    std::vector<std::uint8_t>* const outputPixels) const noexcept {
    if (impl_->factory == nullptr || outputPixels == nullptr ||
        sourceWidth == 0 || sourceHeight == 0 || sourceStride < sourceWidth * 4U ||
        outputWidth == 0 || outputHeight == 0) {
        return E_INVALIDARG;
    }

    const std::uint64_t packedSourceStride =
        static_cast<std::uint64_t>(sourceWidth) * 4U;
    const std::uint64_t minimumSourceBytes =
        static_cast<std::uint64_t>(sourceStride) * (sourceHeight - 1U) +
        packedSourceStride;
    const std::uint64_t outputStride =
        static_cast<std::uint64_t>(outputWidth) * 4U;
    const std::uint64_t outputBytes = outputStride * outputHeight;
    if (sourcePixels.size() < minimumSourceBytes ||
        sourceStride > std::numeric_limits<UINT>::max() ||
        minimumSourceBytes > std::numeric_limits<UINT>::max() ||
        outputStride > std::numeric_limits<UINT>::max() ||
        outputBytes > std::numeric_limits<UINT>::max()) {
        return E_OUTOFMEMORY;
    }

    try {
        if (sourceWidth == outputWidth && sourceHeight == outputHeight) {
            outputPixels->resize(static_cast<std::size_t>(outputBytes));
            for (std::uint32_t row = 0; row < sourceHeight; ++row) {
                std::memcpy(
                    outputPixels->data() +
                        static_cast<std::size_t>(row) * packedSourceStride,
                    sourcePixels.data() +
                        static_cast<std::size_t>(row) * sourceStride,
                    static_cast<std::size_t>(packedSourceStride));
            }
            return S_OK;
        }

        ComPtr<IWICBitmap> bitmap;
        HRESULT result = impl_->factory->CreateBitmapFromMemory(
            sourceWidth,
            sourceHeight,
            GUID_WICPixelFormat32bppBGRA,
            sourceStride,
            static_cast<UINT>(minimumSourceBytes),
            const_cast<BYTE*>(sourcePixels.data()),
            &bitmap);
        if (FAILED(result)) {
            return result;
        }

        ComPtr<IWICBitmapScaler> scaler;
        result = impl_->factory->CreateBitmapScaler(&scaler);
        if (SUCCEEDED(result)) {
            result = scaler->Initialize(
                bitmap.Get(),
                outputWidth,
                outputHeight,
                WICBitmapInterpolationModeFant);
        }
        if (FAILED(result)) {
            return result;
        }

        outputPixels->resize(static_cast<std::size_t>(outputBytes));
        return scaler->CopyPixels(
            nullptr,
            static_cast<UINT>(outputStride),
            static_cast<UINT>(outputBytes),
            outputPixels->data());
    } catch (const std::bad_alloc&) {
        return E_OUTOFMEMORY;
    } catch (...) {
        return E_FAIL;
    }
}

}  // namespace qrec::media
