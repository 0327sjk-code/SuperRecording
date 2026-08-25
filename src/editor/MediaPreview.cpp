#include "editor/MediaPreview.h"

#include "common/Win32Helpers.h"

#include <mfapi.h>
#include <propvarutil.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <new>

#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfplay.lib")
#pragma comment(lib, "mfuuid.lib")

namespace qrec {
namespace {

std::chrono::milliseconds VariantToMilliseconds(const PROPVARIANT& value) noexcept {
    LONGLONG hundredNanoseconds = 0;
    if (value.vt == VT_I8) {
        hundredNanoseconds = value.hVal.QuadPart;
    } else if (value.vt == VT_UI8) {
        hundredNanoseconds = static_cast<LONGLONG>(value.uhVal.QuadPart);
    }
    return std::chrono::milliseconds(std::max<LONGLONG>(0, hundredNanoseconds / 10'000));
}

}  // namespace

class MediaPreview::Callback final : public IMFPMediaPlayerCallback {
public:
    explicit Callback(const HWND notificationWindow) noexcept
        : notificationWindow_(notificationWindow) {}

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID interfaceId, void** object) override {
        if (object == nullptr) {
            return E_POINTER;
        }
        if (interfaceId == __uuidof(IUnknown) || interfaceId == __uuidof(IMFPMediaPlayerCallback)) {
            *object = static_cast<IMFPMediaPlayerCallback*>(this);
            AddRef();
            return S_OK;
        }
        *object = nullptr;
        return E_NOINTERFACE;
    }

    ULONG STDMETHODCALLTYPE AddRef() override {
        return ++referenceCount_;
    }

    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG count = --referenceCount_;
        if (count == 0) {
            delete this;
        }
        return count;
    }

    void STDMETHODCALLTYPE OnMediaPlayerEvent(MFP_EVENT_HEADER* eventHeader) override {
        if (eventHeader == nullptr) {
            return;
        }
        const bool consumedByEditor =
            eventHeader->eEventType == MFP_EVENT_TYPE_MEDIAITEM_SET ||
            eventHeader->eEventType == MFP_EVENT_TYPE_PLAY ||
            eventHeader->eEventType == MFP_EVENT_TYPE_PAUSE ||
            eventHeader->eEventType == MFP_EVENT_TYPE_STOP ||
            eventHeader->eEventType == MFP_EVENT_TYPE_PLAYBACK_ENDED ||
            eventHeader->eEventType == MFP_EVENT_TYPE_POSITION_SET;
        if (!consumedByEditor && SUCCEEDED(eventHeader->hrEvent)) {
            return;
        }
        const HWND target = notificationWindow_.load(std::memory_order_acquire);
        if (target != nullptr) {
            ::PostMessageW(
                target,
                PreviewEventMessage,
                static_cast<WPARAM>(eventHeader->eEventType),
                static_cast<LPARAM>(eventHeader->hrEvent));
        }
    }

    void DetachWindow() noexcept {
        notificationWindow_.store(nullptr, std::memory_order_release);
    }

private:
    std::atomic_ulong referenceCount_{1};
    std::atomic<HWND> notificationWindow_{};
};

MediaPreview::~MediaPreview() {
    Close();
}

bool MediaPreview::Open(
    const std::filesystem::path& mediaPath,
    const HWND videoHost,
    const HWND notificationWindow,
    std::wstring* errorMessage) {
    Close();
    if (mediaPath.empty() || videoHost == nullptr || notificationWindow == nullptr) {
        if (errorMessage != nullptr) {
            *errorMessage = L"预览参数无效。";
        }
        return false;
    }

    HRESULT result = ::MFStartup(MF_VERSION, MFSTARTUP_FULL);
    if (FAILED(result)) {
        if (errorMessage != nullptr) {
            *errorMessage = L"无法启动 Media Foundation：" + win32::FormatError(result);
        }
        return false;
    }
    mediaFoundationStarted_ = true;

    callback_ = new (std::nothrow) Callback(notificationWindow);
    if (callback_ == nullptr) {
        if (errorMessage != nullptr) {
            *errorMessage = L"无法创建预览回调。";
        }
        Close();
        return false;
    }

    IMFPMediaPlayer* rawPlayer = nullptr;
    result = ::MFPCreateMediaPlayer(
        mediaPath.c_str(),
        FALSE,
        MFP_OPTION_FREE_THREADED_CALLBACK,
        callback_,
        videoHost,
        &rawPlayer);
    if (FAILED(result)) {
        if (errorMessage != nullptr) {
            *errorMessage = L"无法打开录屏预览：" + win32::FormatError(result);
        }
        Close();
        return false;
    }
    player_.Attach(rawPlayer);
    return true;
}

void MediaPreview::Close() noexcept {
    if (callback_ != nullptr) {
        callback_->DetachWindow();
    }
    if (player_ != nullptr) {
        player_->Shutdown();
        player_.Reset();
    }
    if (callback_ != nullptr) {
        callback_->Release();
        callback_ = nullptr;
    }
    if (mediaFoundationStarted_) {
        ::MFShutdown();
        mediaFoundationStarted_ = false;
    }
}

bool MediaPreview::Play(std::wstring* errorMessage) {
    if (player_ == nullptr) {
        if (errorMessage != nullptr) {
            *errorMessage = L"预览尚未就绪。";
        }
        return false;
    }
    const HRESULT result = player_->Play();
    if (FAILED(result) && errorMessage != nullptr) {
        *errorMessage = L"无法播放预览：" + win32::FormatError(result);
    }
    return SUCCEEDED(result);
}

bool MediaPreview::Pause(std::wstring* errorMessage) {
    if (player_ == nullptr) {
        return false;
    }
    const HRESULT result = player_->Pause();
    if (FAILED(result) && errorMessage != nullptr) {
        *errorMessage = L"无法暂停预览：" + win32::FormatError(result);
    }
    return SUCCEEDED(result);
}

bool MediaPreview::Seek(
    const std::chrono::milliseconds position,
    std::wstring* errorMessage) {
    if (player_ == nullptr) {
        return false;
    }
    PROPVARIANT value{};
    ::PropVariantInit(&value);
    value.vt = VT_I8;
    value.hVal.QuadPart = std::max<std::int64_t>(0, position.count()) * 10'000;
    const HRESULT result = player_->SetPosition(MFP_POSITIONTYPE_100NS, &value);
    ::PropVariantClear(&value);
    if (FAILED(result) && errorMessage != nullptr) {
        *errorMessage = L"无法定位预览：" + win32::FormatError(result);
    }
    return SUCCEEDED(result);
}

std::chrono::milliseconds MediaPreview::Position() const noexcept {
    if (player_ == nullptr) {
        return {};
    }
    PROPVARIANT value{};
    ::PropVariantInit(&value);
    const HRESULT result = player_->GetPosition(MFP_POSITIONTYPE_100NS, &value);
    const auto position = SUCCEEDED(result) ? VariantToMilliseconds(value)
                                            : std::chrono::milliseconds(0);
    ::PropVariantClear(&value);
    return position;
}

std::chrono::milliseconds MediaPreview::Duration() const noexcept {
    if (player_ == nullptr) {
        return {};
    }
    PROPVARIANT value{};
    ::PropVariantInit(&value);
    const HRESULT result = player_->GetDuration(MFP_POSITIONTYPE_100NS, &value);
    const auto duration = SUCCEEDED(result) ? VariantToMilliseconds(value)
                                            : std::chrono::milliseconds(0);
    ::PropVariantClear(&value);
    return duration;
}

bool MediaPreview::IsPlaying() const noexcept {
    if (player_ == nullptr) {
        return false;
    }
    MFP_MEDIAPLAYER_STATE state = MFP_MEDIAPLAYER_STATE_EMPTY;
    return SUCCEEDED(player_->GetState(&state)) && state == MFP_MEDIAPLAYER_STATE_PLAYING;
}

void MediaPreview::UpdateVideo() noexcept {
    if (player_ != nullptr) {
        player_->UpdateVideo();
    }
}

}  // namespace qrec
