#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "DesktopDuplicator.h"
#include "capture/DesktopFrameTransform.h"

#include <windows.h>

#include <d3d11.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

#include <algorithm>
#include <cstring>
#include <limits>
#include <new>
#include <span>
#include <stdexcept>
#include <utility>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "user32.lib")

namespace qrec::capture {
namespace {

using Microsoft::WRL::ComPtr;
namespace frame_transform = desktop_frame_transform;

[[nodiscard]] std::wstring HResultText(const HRESULT result) {
    wchar_t* systemText = nullptr;
    const DWORD length = ::FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
            FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr,
        static_cast<DWORD>(result),
        MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
        reinterpret_cast<wchar_t*>(&systemText),
        0,
        nullptr);

    std::wstring text;
    if (length != 0 && systemText != nullptr) {
        text.assign(systemText, length);
        while (!text.empty() && (text.back() == L'\r' || text.back() == L'\n')) {
            text.pop_back();
        }
    }
    if (systemText != nullptr) {
        ::LocalFree(systemText);
    }

    wchar_t code[24]{};
    ::swprintf_s(code, L"0x%08X", static_cast<unsigned int>(result));
    if (text.empty()) {
        return code;
    }
    return text + L" (" + code + L")";
}

[[nodiscard]] bool Contains(const RECT& output, const IntRect& region) noexcept {
    return region.left >= output.left && region.top >= output.top &&
           region.right <= output.right && region.bottom <= output.bottom;
}

[[nodiscard]] bool Intersects(const RECT& output, const IntRect& region) noexcept {
    return region.left < output.right && region.right > output.left &&
           region.top < output.bottom && region.bottom > output.top;
}

void SetFailure(
    std::wstring& destination,
    long& nativeError,
    DesktopDuplicatorError* category,
    const DesktopDuplicatorError value,
    std::wstring message,
    const HRESULT result = S_OK) {
    destination = std::move(message);
    nativeError = static_cast<long>(result);
    if (category != nullptr) {
        *category = value;
    }
}

}  // namespace

struct DesktopDuplicator::Impl final {
    struct CursorSnapshot final {
        CURSORINFO info{sizeof(CURSORINFO)};
        bool visible{};
    };

    DesktopDuplicatorOptions options{};
    DXGI_OUTPUT_DESC outputDescription{};
    ComPtr<IDXGIAdapter1> adapter;
    ComPtr<IDXGIOutput1> output;
    ComPtr<IDXGIOutputDuplication> duplication;
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<ID3D11Texture2D> stagingTexture;
    DXGI_FORMAT stagingFormat{DXGI_FORMAT_UNKNOWN};
    std::uint32_t stagingWidth{};
    std::uint32_t stagingHeight{};
    frame_transform::Extent desktopExtent{};
    frame_transform::HalfOpenRect desktopSelection{};

    HDC cursorDc{};
    HBITMAP cursorBitmap{};
    HGDIOBJ previousCursorBitmap{};
    void* cursorPixels{};

    void DestroyCursorSurface() noexcept {
        if (cursorDc != nullptr && previousCursorBitmap != nullptr) {
            ::SelectObject(cursorDc, previousCursorBitmap);
        }
        previousCursorBitmap = nullptr;
        if (cursorBitmap != nullptr) {
            ::DeleteObject(cursorBitmap);
        }
        cursorBitmap = nullptr;
        cursorPixels = nullptr;
        if (cursorDc != nullptr) {
            ::DeleteDC(cursorDc);
        }
        cursorDc = nullptr;
    }

    void Reset() noexcept {
        DestroyCursorSurface();
        stagingTexture.Reset();
        context.Reset();
        device.Reset();
        duplication.Reset();
        output.Reset();
        adapter.Reset();
        outputDescription = {};
        options = {};
        stagingFormat = DXGI_FORMAT_UNKNOWN;
        stagingWidth = 0;
        stagingHeight = 0;
        desktopExtent = {};
        desktopSelection = {};
    }

    [[nodiscard]] bool CreateCursorSurface(std::wstring& errorMessage, long& nativeError) {
        if (!options.includeCursor) {
            return true;
        }

        BITMAPINFO bitmapInfo{};
        bitmapInfo.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bitmapInfo.bmiHeader.biWidth = options.region.Width();
        bitmapInfo.bmiHeader.biHeight = -options.region.Height();
        bitmapInfo.bmiHeader.biPlanes = 1;
        bitmapInfo.bmiHeader.biBitCount = 32;
        bitmapInfo.bmiHeader.biCompression = BI_RGB;

        cursorDc = ::CreateCompatibleDC(nullptr);
        if (cursorDc == nullptr) {
            nativeError = static_cast<long>(::GetLastError());
            errorMessage = L"创建光标合成 DC 失败。";
            return false;
        }

        cursorBitmap = ::CreateDIBSection(
            cursorDc,
            &bitmapInfo,
            DIB_RGB_COLORS,
            &cursorPixels,
            nullptr,
            0);
        if (cursorBitmap == nullptr || cursorPixels == nullptr) {
            nativeError = static_cast<long>(::GetLastError());
            errorMessage = L"创建光标合成缓冲区失败。";
            DestroyCursorSurface();
            return false;
        }

        previousCursorBitmap = ::SelectObject(cursorDc, cursorBitmap);
        if (previousCursorBitmap == nullptr || previousCursorBitmap == HGDI_ERROR) {
            nativeError = static_cast<long>(::GetLastError());
            errorMessage = L"选择光标合成缓冲区失败。";
            previousCursorBitmap = nullptr;
            DestroyCursorSurface();
            return false;
        }
        return true;
    }

    [[nodiscard]] CursorSnapshot CaptureCursorSnapshot() const noexcept {
        CursorSnapshot snapshot{};
        snapshot.visible = options.includeCursor &&
            ::GetCursorInfo(&snapshot.info) != FALSE &&
            (snapshot.info.flags & CURSOR_SHOWING) != 0 &&
            snapshot.info.hCursor != nullptr;
        return snapshot;
    }

    void CompositeCursor(
        DesktopFrame& frame,
        const CursorSnapshot& snapshot) noexcept {
        if (!options.includeCursor || cursorDc == nullptr || cursorPixels == nullptr ||
            frame.bgra.empty() || !snapshot.visible) {
            return;
        }

        const CURSORINFO& cursorInfo = snapshot.info;

        ICONINFO iconInfo{};
        const bool hasIconInfo = ::GetIconInfo(cursorInfo.hCursor, &iconInfo) != FALSE;
        const int hotspotX = hasIconInfo ? static_cast<int>(iconInfo.xHotspot) : 0;
        const int hotspotY = hasIconInfo ? static_cast<int>(iconInfo.yHotspot) : 0;
        const int drawX = cursorInfo.ptScreenPos.x - hotspotX - options.region.left;
        const int drawY = cursorInfo.ptScreenPos.y - hotspotY - options.region.top;

        int cursorWidth = std::max(1, ::GetSystemMetrics(SM_CXCURSOR));
        int cursorHeight = std::max(1, ::GetSystemMetrics(SM_CYCURSOR));
        if (hasIconInfo) {
            const HBITMAP shapeBitmap = iconInfo.hbmColor != nullptr
                                            ? iconInfo.hbmColor
                                            : iconInfo.hbmMask;
            BITMAP shapeDescription{};
            if (shapeBitmap != nullptr &&
                ::GetObjectW(shapeBitmap, sizeof(shapeDescription), &shapeDescription) ==
                    sizeof(shapeDescription)) {
                cursorWidth = std::max(1L, shapeDescription.bmWidth);
                cursorHeight = std::max(
                    1L,
                    iconInfo.hbmColor != nullptr
                        ? shapeDescription.bmHeight
                        : shapeDescription.bmHeight / 2);
            }
        }
        const int frameWidth = static_cast<int>(frame.width);
        const int frameHeight = static_cast<int>(frame.height);
        const int copyLeft = std::max(0, drawX);
        const int copyTop = std::max(0, drawY);
        const int copyRight = std::min(frameWidth, drawX + cursorWidth);
        const int copyBottom = std::min(frameHeight, drawY + cursorHeight);
        const bool overlaps = copyRight > copyLeft && copyBottom > copyTop;

        if (overlaps) {
            const std::size_t pixelOffset =
                static_cast<std::size_t>(copyLeft) * frame_transform::BytesPerPixel;
            const std::size_t copyBytes =
                static_cast<std::size_t>(copyRight - copyLeft) *
                frame_transform::BytesPerPixel;
            auto* cursorBuffer = static_cast<std::uint8_t*>(cursorPixels);
            for (int row = copyTop; row < copyBottom; ++row) {
                const std::size_t rowOffset =
                    static_cast<std::size_t>(row) * frame.stride + pixelOffset;
                std::memcpy(
                    cursorBuffer + rowOffset,
                    frame.bgra.data() + rowOffset,
                    copyBytes);
            }
            if (::DrawIconEx(
                    cursorDc,
                    drawX,
                    drawY,
                    cursorInfo.hCursor,
                    cursorWidth,
                    cursorHeight,
                    0,
                    nullptr,
                    DI_NORMAL)) {
                for (int row = copyTop; row < copyBottom; ++row) {
                    const std::size_t rowOffset =
                        static_cast<std::size_t>(row) * frame.stride + pixelOffset;
                    std::memcpy(
                        frame.bgra.data() + rowOffset,
                        cursorBuffer + rowOffset,
                        copyBytes);
                }
            }
        }

        if (hasIconInfo) {
            if (iconInfo.hbmColor != nullptr) {
                ::DeleteObject(iconInfo.hbmColor);
            }
            if (iconInfo.hbmMask != nullptr) {
                ::DeleteObject(iconInfo.hbmMask);
            }
        }
    }
};

DesktopDuplicator::DesktopDuplicator() : impl_(std::make_unique<Impl>()) {}

DesktopDuplicator::~DesktopDuplicator() = default;

bool DesktopDuplicator::Initialize(
    const DesktopDuplicatorOptions& options,
    std::wstring& errorMessage,
    long& nativeError,
    DesktopDuplicatorError* errorCategory) noexcept {
    errorMessage.clear();
    nativeError = 0;
    if (errorCategory != nullptr) {
        *errorCategory = DesktopDuplicatorError::None;
    }
    impl_->Reset();

    if (!options.region.IsValid() || options.region.Width() % 2 != 0 ||
        options.region.Height() % 2 != 0) {
        SetFailure(
            errorMessage,
            nativeError,
            errorCategory,
            DesktopDuplicatorError::InvalidRegion,
            L"录制区域必须有效，且宽高必须为偶数。",
            E_INVALIDARG);
        return false;
    }

    ComPtr<IDXGIFactory1> factory;
    HRESULT result = ::CreateDXGIFactory1(IID_PPV_ARGS(&factory));
    if (FAILED(result)) {
        SetFailure(
            errorMessage,
            nativeError,
            errorCategory,
            DesktopDuplicatorError::GraphicsInitialization,
            L"创建 DXGI 工厂失败：" + HResultText(result),
            result);
        return false;
    }

    std::uint32_t intersectingOutputCount = 0;
    bool foundContainingOutput = false;
    for (UINT adapterIndex = 0;; ++adapterIndex) {
        ComPtr<IDXGIAdapter1> adapter;
        result = factory->EnumAdapters1(adapterIndex, &adapter);
        if (result == DXGI_ERROR_NOT_FOUND) {
            break;
        }
        if (FAILED(result)) {
            continue;
        }

        for (UINT outputIndex = 0;; ++outputIndex) {
            ComPtr<IDXGIOutput> baseOutput;
            result = adapter->EnumOutputs(outputIndex, &baseOutput);
            if (result == DXGI_ERROR_NOT_FOUND) {
                break;
            }
            if (FAILED(result)) {
                continue;
            }

            DXGI_OUTPUT_DESC description{};
            if (FAILED(baseOutput->GetDesc(&description)) || !description.AttachedToDesktop) {
                continue;
            }
            if (Intersects(description.DesktopCoordinates, options.region)) {
                ++intersectingOutputCount;
            }
            if (foundContainingOutput || !Contains(description.DesktopCoordinates, options.region)) {
                continue;
            }

            ComPtr<IDXGIOutput1> output1;
            if (FAILED(baseOutput.As(&output1))) {
                continue;
            }
            impl_->adapter = adapter;
            impl_->output = output1;
            impl_->outputDescription = description;
            foundContainingOutput = true;
        }
    }

    if (!foundContainingOutput) {
        const bool crossesDisplays = intersectingOutputCount > 1;
        SetFailure(
            errorMessage,
            nativeError,
            errorCategory,
            crossesDisplays ? DesktopDuplicatorError::CrossDisplayRegion
                            : DesktopDuplicatorError::DisplayNotFound,
            crossesDisplays
                ? L"当前版本不支持跨显示器录制区域，请将选区限制在同一显示器内。"
                : L"录制区域不在任何已连接的显示器内。",
            E_INVALIDARG);
        impl_->Reset();
        return false;
    }

    if (!frame_transform::IsSupportedRotation(impl_->outputDescription.Rotation)) {
        SetFailure(
            errorMessage,
            nativeError,
            errorCategory,
            DesktopDuplicatorError::UnsupportedRotation,
            L"桌面复制返回了未知的显示器旋转方向。",
            DXGI_ERROR_UNSUPPORTED);
        impl_->Reset();
        return false;
    }

    const RECT& desktopCoordinates = impl_->outputDescription.DesktopCoordinates;
    const frame_transform::SignedHalfOpenRect outputScreenRect{
        desktopCoordinates.left,
        desktopCoordinates.top,
        desktopCoordinates.right,
        desktopCoordinates.bottom};
    const frame_transform::SignedHalfOpenRect selectionScreenRect{
        options.region.left,
        options.region.top,
        options.region.right,
        options.region.bottom};
    const frame_transform::LocalRectMapping fullOutputMapping =
        frame_transform::MapScreenRectToOutputLocal(outputScreenRect, outputScreenRect);
    const frame_transform::LocalRectMapping selectionMapping =
        frame_transform::MapScreenRectToOutputLocal(outputScreenRect, selectionScreenRect);
    if (!fullOutputMapping.valid || !selectionMapping.valid) {
        SetFailure(
            errorMessage,
            nativeError,
            errorCategory,
            DesktopDuplicatorError::InvalidRegion,
            L"无法将录制区域转换到目标显示器的局部坐标。",
            E_BOUNDS);
        impl_->Reset();
        return false;
    }
    impl_->desktopExtent = {
        fullOutputMapping.desktopRect.Width(),
        fullOutputMapping.desktopRect.Height()};
    impl_->desktopSelection = selectionMapping.desktopRect;

    constexpr D3D_FEATURE_LEVEL requestedLevels[] = {
        D3D_FEATURE_LEVEL_11_1,
        D3D_FEATURE_LEVEL_11_0,
        D3D_FEATURE_LEVEL_10_1,
        D3D_FEATURE_LEVEL_10_0,
    };
    D3D_FEATURE_LEVEL selectedLevel{};
    const UINT deviceFlags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
    result = ::D3D11CreateDevice(
        impl_->adapter.Get(),
        D3D_DRIVER_TYPE_UNKNOWN,
        nullptr,
        deviceFlags,
        requestedLevels,
        static_cast<UINT>(std::size(requestedLevels)),
        D3D11_SDK_VERSION,
        &impl_->device,
        &selectedLevel,
        &impl_->context);
    if (result == E_INVALIDARG) {
        result = ::D3D11CreateDevice(
            impl_->adapter.Get(),
            D3D_DRIVER_TYPE_UNKNOWN,
            nullptr,
            deviceFlags,
            requestedLevels + 1,
            static_cast<UINT>(std::size(requestedLevels) - 1),
            D3D11_SDK_VERSION,
            &impl_->device,
            &selectedLevel,
            &impl_->context);
    }
    if (FAILED(result)) {
        SetFailure(
            errorMessage,
            nativeError,
            errorCategory,
            DesktopDuplicatorError::GraphicsInitialization,
            L"创建 D3D11 录屏设备失败：" + HResultText(result),
            result);
        impl_->Reset();
        return false;
    }

    result = impl_->output->DuplicateOutput(impl_->device.Get(), &impl_->duplication);
    if (FAILED(result)) {
        SetFailure(
            errorMessage,
            nativeError,
            errorCategory,
            DesktopDuplicatorError::DuplicationInitialization,
            L"初始化桌面复制失败：" + HResultText(result),
            result);
        impl_->Reset();
        return false;
    }

    impl_->options = options;
    if (!impl_->CreateCursorSurface(errorMessage, nativeError)) {
        if (errorCategory != nullptr) {
            *errorCategory = DesktopDuplicatorError::GraphicsInitialization;
        }
        impl_->Reset();
        return false;
    }
    return true;
}

FrameAcquireStatus DesktopDuplicator::AcquireFrame(
    DesktopFrame& destination,
    const std::uint32_t timeoutMilliseconds,
    std::wstring& errorMessage,
    long& nativeError) noexcept {
    errorMessage.clear();
    nativeError = 0;
    if (!impl_->duplication || !impl_->device || !impl_->context) {
        errorMessage = L"桌面复制器尚未初始化。";
        nativeError = static_cast<long>(E_UNEXPECTED);
        return FrameAcquireStatus::Failed;
    }

    DXGI_OUTDUPL_FRAME_INFO frameInformation{};
    ComPtr<IDXGIResource> desktopResource;
    HRESULT result = impl_->duplication->AcquireNextFrame(
        timeoutMilliseconds,
        &frameInformation,
        &desktopResource);
    if (result == DXGI_ERROR_WAIT_TIMEOUT) {
        return FrameAcquireStatus::NoDesktopChange;
    }
    if (result == DXGI_ERROR_ACCESS_LOST) {
        errorMessage = L"显示模式或桌面会话发生变化，桌面复制访问已丢失。";
        nativeError = static_cast<long>(result);
        return FrameAcquireStatus::AccessLost;
    }
    if (FAILED(result)) {
        errorMessage = L"获取桌面帧失败：" + HResultText(result);
        nativeError = static_cast<long>(result);
        return FrameAcquireStatus::Failed;
    }
    // Snapshot the system cursor immediately after the desktop frame arrives.
    // Rotation and CPU readback can take several milliseconds at 4K; deferring
    // GetCursorInfo until after that work would visibly lead the captured frame.
    const Impl::CursorSnapshot cursorSnapshot = impl_->CaptureCursorSnapshot();

    struct FrameRelease final {
        IDXGIOutputDuplication* duplication{};
        ~FrameRelease() {
            if (duplication != nullptr) {
                duplication->ReleaseFrame();
            }
        }
    } frameRelease{impl_->duplication.Get()};

    ComPtr<ID3D11Texture2D> desktopTexture;
    result = desktopResource.As(&desktopTexture);
    if (FAILED(result)) {
        errorMessage = L"桌面帧不是 D3D11 纹理：" + HResultText(result);
        nativeError = static_cast<long>(result);
        return FrameAcquireStatus::Failed;
    }

    D3D11_TEXTURE2D_DESC sourceDescription{};
    desktopTexture->GetDesc(&sourceDescription);
    if (sourceDescription.Format != DXGI_FORMAT_B8G8R8A8_UNORM &&
        sourceDescription.Format != DXGI_FORMAT_B8G8R8A8_UNORM_SRGB) {
        errorMessage = L"桌面复制返回了不受支持的像素格式。";
        nativeError = static_cast<long>(DXGI_ERROR_UNSUPPORTED);
        return FrameAcquireStatus::Failed;
    }

    const frame_transform::Extent textureExtent{
        sourceDescription.Width,
        sourceDescription.Height};
    const frame_transform::RotationLayout rotationLayout{
        impl_->outputDescription.Rotation,
        impl_->desktopExtent,
        textureExtent};
    if (!frame_transform::IsValidRotationLayout(rotationLayout)) {
        const frame_transform::Extent expectedTextureExtent =
            frame_transform::ExpectedTextureExtent(
                impl_->desktopExtent,
                impl_->outputDescription.Rotation);
        errorMessage =
            L"桌面复制纹理尺寸与显示器旋转布局不一致：期望 " +
            std::to_wstring(expectedTextureExtent.width) + L"x" +
            std::to_wstring(expectedTextureExtent.height) + L"，实际 " +
            std::to_wstring(textureExtent.width) + L"x" +
            std::to_wstring(textureExtent.height) + L"。";
        nativeError = static_cast<long>(E_UNEXPECTED);
        return FrameAcquireStatus::Failed;
    }

    const frame_transform::RectMapping textureMapping =
        frame_transform::MapDesktopRectToTexture(
            rotationLayout,
            impl_->desktopSelection);
    if (!textureMapping.valid) {
        errorMessage = L"录制区域无法映射到桌面复制纹理的物理像素范围。";
        nativeError = static_cast<long>(E_BOUNDS);
        return FrameAcquireStatus::Failed;
    }

    const std::uint32_t rawWidth = textureMapping.textureRect.Width();
    const std::uint32_t rawHeight = textureMapping.textureRect.Height();
    if (!impl_->stagingTexture || impl_->stagingFormat != sourceDescription.Format ||
        impl_->stagingWidth != rawWidth || impl_->stagingHeight != rawHeight) {
        D3D11_TEXTURE2D_DESC stagingDescription{};
        stagingDescription.Width = rawWidth;
        stagingDescription.Height = rawHeight;
        stagingDescription.MipLevels = 1;
        stagingDescription.ArraySize = 1;
        stagingDescription.Format = sourceDescription.Format;
        stagingDescription.SampleDesc.Count = 1;
        stagingDescription.Usage = D3D11_USAGE_STAGING;
        stagingDescription.CPUAccessFlags = D3D11_CPU_ACCESS_READ;

        impl_->stagingTexture.Reset();
        impl_->stagingFormat = DXGI_FORMAT_UNKNOWN;
        impl_->stagingWidth = 0;
        impl_->stagingHeight = 0;
        result = impl_->device->CreateTexture2D(
            &stagingDescription,
            nullptr,
            &impl_->stagingTexture);
        if (FAILED(result)) {
            errorMessage = L"创建桌面帧读取缓冲区失败：" + HResultText(result);
            nativeError = static_cast<long>(result);
            return FrameAcquireStatus::Failed;
        }
        impl_->stagingFormat = sourceDescription.Format;
        impl_->stagingWidth = rawWidth;
        impl_->stagingHeight = rawHeight;
    }

    D3D11_BOX sourceBox{};
    sourceBox.left = textureMapping.textureRect.left;
    sourceBox.top = textureMapping.textureRect.top;
    sourceBox.front = 0;
    sourceBox.right = textureMapping.textureRect.right;
    sourceBox.bottom = textureMapping.textureRect.bottom;
    sourceBox.back = 1;
    impl_->context->CopySubresourceRegion(
        impl_->stagingTexture.Get(),
        0,
        0,
        0,
        0,
        desktopTexture.Get(),
        0,
        &sourceBox);

    D3D11_MAPPED_SUBRESOURCE mapped{};
    result = impl_->context->Map(
        impl_->stagingTexture.Get(),
        0,
        D3D11_MAP_READ,
        0,
        &mapped);
    if (FAILED(result)) {
        errorMessage = L"读取桌面帧失败：" + HResultText(result);
        nativeError = static_cast<long>(result);
        return result == DXGI_ERROR_DEVICE_REMOVED || result == DXGI_ERROR_DEVICE_RESET
                   ? FrameAcquireStatus::AccessLost
                   : FrameAcquireStatus::Failed;
    }

    struct TextureUnmap final {
        ID3D11DeviceContext* context{};
        ID3D11Texture2D* texture{};
        ~TextureUnmap() {
            if (context != nullptr && texture != nullptr) {
                context->Unmap(texture, 0);
            }
        }
    } textureUnmap{impl_->context.Get(), impl_->stagingTexture.Get()};

    const frame_transform::Extent destinationExtent{
        impl_->desktopSelection.Width(),
        impl_->desktopSelection.Height()};
    if (destinationExtent.width >
        std::numeric_limits<std::uint32_t>::max() / frame_transform::BytesPerPixel) {
        errorMessage = L"录制区域行跨度溢出，已中止本帧。";
        nativeError = static_cast<long>(E_BOUNDS);
        return FrameAcquireStatus::Failed;
    }
    const std::uint32_t packedStride =
        destinationExtent.width * frame_transform::BytesPerPixel;
    if (destinationExtent.height != 0 &&
        static_cast<std::size_t>(packedStride) >
            std::numeric_limits<std::size_t>::max() / destinationExtent.height) {
        errorMessage = L"录制区域帧缓冲区大小溢出，已中止本帧。";
        nativeError = static_cast<long>(E_BOUNDS);
        return FrameAcquireStatus::Failed;
    }
    const std::size_t requiredBytes =
        static_cast<std::size_t>(packedStride) * destinationExtent.height;
    if (requiredBytes > destination.bgra.max_size()) {
        errorMessage = L"录制区域帧缓冲区超过当前进程可分配范围。";
        nativeError = static_cast<long>(E_OUTOFMEMORY);
        return FrameAcquireStatus::Failed;
    }
    try {
        destination.bgra.resize(requiredBytes);
    } catch (const std::bad_alloc&) {
        errorMessage = L"分配桌面帧缓冲区时内存不足。";
        nativeError = static_cast<long>(E_OUTOFMEMORY);
        return FrameAcquireStatus::Failed;
    } catch (const std::length_error&) {
        errorMessage = L"桌面帧缓冲区大小无效。";
        nativeError = static_cast<long>(E_BOUNDS);
        return FrameAcquireStatus::Failed;
    }

    const frame_transform::BgraTransformStatus transformStatus =
        frame_transform::CopyMappedBgraToDesktop(
            mapped.pData,
            mapped.RowPitch,
            {rawWidth, rawHeight},
            impl_->outputDescription.Rotation,
            std::span<std::uint8_t>(destination.bgra.data(), destination.bgra.size()),
            packedStride,
            destinationExtent);
    if (transformStatus != frame_transform::BgraTransformStatus::Success) {
        switch (transformStatus) {
        case frame_transform::BgraTransformStatus::InvalidSourceRowPitch:
            errorMessage = L"桌面帧行跨度无效，已中止本帧以避免生成错行视频。";
            nativeError = static_cast<long>(E_UNEXPECTED);
            break;
        case frame_transform::BgraTransformStatus::UnsupportedRotation:
            errorMessage = L"桌面复制返回了未知的显示器旋转方向。";
            nativeError = static_cast<long>(DXGI_ERROR_UNSUPPORTED);
            break;
        case frame_transform::BgraTransformStatus::ArithmeticOverflow:
        case frame_transform::BgraTransformStatus::DestinationTooSmall:
            errorMessage = L"桌面帧缓冲区布局无效，已中止本帧。";
            nativeError = static_cast<long>(E_BOUNDS);
            break;
        case frame_transform::BgraTransformStatus::InvalidDimensions:
        default:
            errorMessage = L"桌面帧旋转前后的尺寸契约不一致。";
            nativeError = static_cast<long>(E_UNEXPECTED);
            break;
        }
        return FrameAcquireStatus::Failed;
    }

    destination.width = destinationExtent.width;
    destination.height = destinationExtent.height;
    destination.stride = packedStride;

    impl_->CompositeCursor(destination, cursorSnapshot);
    return FrameAcquireStatus::FrameReady;
}

void DesktopDuplicator::Reset() noexcept {
    impl_->Reset();
}

bool DesktopDuplicator::IsInitialized() const noexcept {
    return impl_->duplication != nullptr;
}

IntRect DesktopDuplicator::Region() const noexcept {
    return impl_->options.region;
}

}  // namespace qrec::capture
