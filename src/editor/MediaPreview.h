#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>
#include <mfplay.h>
#include <wrl/client.h>

#include <chrono>
#include <filesystem>
#include <string>

namespace qrec {

inline constexpr UINT PreviewEventMessage = WM_APP + 0x221;

struct PreviewEvent final {
    MFP_EVENT_TYPE type{MFP_EVENT_TYPE_ERROR};
    HRESULT status{E_FAIL};
};

class MediaPreview final {
public:
    MediaPreview() = default;
    ~MediaPreview();

    MediaPreview(const MediaPreview&) = delete;
    MediaPreview& operator=(const MediaPreview&) = delete;

    [[nodiscard]] bool Open(
        const std::filesystem::path& mediaPath,
        HWND videoHost,
        HWND notificationWindow,
        std::wstring* errorMessage = nullptr);
    void Close() noexcept;
    [[nodiscard]] bool Play(std::wstring* errorMessage = nullptr);
    [[nodiscard]] bool Pause(std::wstring* errorMessage = nullptr);
    [[nodiscard]] bool Seek(std::chrono::milliseconds position, std::wstring* errorMessage = nullptr);
    [[nodiscard]] std::chrono::milliseconds Position() const noexcept;
    [[nodiscard]] std::chrono::milliseconds Duration() const noexcept;
    [[nodiscard]] bool IsPlaying() const noexcept;
    void UpdateVideo() noexcept;

private:
    class Callback;
    Microsoft::WRL::ComPtr<IMFPMediaPlayer> player_;
    Callback* callback_{};
    bool mediaFoundationStarted_{};
};

}  // namespace qrec
