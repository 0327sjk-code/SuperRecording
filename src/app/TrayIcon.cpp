#include "app/TrayIcon.h"

#include "app/resource.h"
#include "common/AppMessages.h"
#include "common/ProductInfo.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cmath>
#include <cstring>
#include <strsafe.h>
#include <string>

namespace qrec {

TrayIcon::~TrayIcon() {
    Remove();
    if (icon_ != nullptr) {
        ::DestroyIcon(icon_);
    }
}

bool TrayIcon::Add(const HWND owner, const UINT callbackMessage) {
    if (added_) {
        return true;
    }
    if (icon_ == nullptr) {
        icon_ = CreateRecorderIcon();
    }

    data_ = {};
    data_.cbSize = sizeof(data_);
    data_.hWnd = owner;
    data_.uID = 1;
    data_.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP | NIF_SHOWTIP;
    data_.uCallbackMessage = callbackMessage;
    data_.hIcon = icon_ != nullptr ? icon_ : ::LoadIconW(nullptr, IDI_APPLICATION);
    const std::wstring toolTip =
        std::wstring(product::Name) + L" · 单击开始 · " + shortcutLabel_ + L" 快捷录制";
    ::StringCchCopyW(data_.szTip, ARRAYSIZE(data_.szTip), toolTip.c_str());

    added_ = ::Shell_NotifyIconW(NIM_ADD, &data_) != FALSE;
    if (added_) {
        data_.uVersion = NOTIFYICON_VERSION_4;
        ::Shell_NotifyIconW(NIM_SETVERSION, &data_);
    }
    return added_;
}

void TrayIcon::Remove() {
    if (!added_) {
        return;
    }
    ::Shell_NotifyIconW(NIM_DELETE, &data_);
    added_ = false;
}

bool TrayIcon::RestoreAfterExplorerRestart() {
    added_ = false;
    return Add(data_.hWnd, data_.uCallbackMessage);
}

void TrayIcon::UpdateShortcutLabel(const std::wstring_view shortcutLabel) {
    shortcutLabel_.assign(shortcutLabel);
    const std::wstring toolTip =
        std::wstring(product::Name) + L" · 单击开始 · " + shortcutLabel_ + L" 快捷录制";
    ::StringCchCopyW(data_.szTip, ARRAYSIZE(data_.szTip), toolTip.c_str());
    if (!added_) {
        return;
    }
    data_.uFlags = NIF_TIP | NIF_SHOWTIP;
    static_cast<void>(::Shell_NotifyIconW(NIM_MODIFY, &data_));
    data_.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP | NIF_SHOWTIP;
}

void TrayIcon::ShowNotification(
    const std::wstring_view title,
    const std::wstring_view text,
    const DWORD flags) {
    if (!added_) {
        return;
    }
    data_.uFlags = NIF_INFO;
    ::StringCchCopyW(data_.szInfoTitle, ARRAYSIZE(data_.szInfoTitle),
                     std::wstring(title).c_str());
    ::StringCchCopyW(data_.szInfo, ARRAYSIZE(data_.szInfo),
                     std::wstring(text).c_str());
    data_.dwInfoFlags = flags;
    data_.uTimeout = 3500;
    ::Shell_NotifyIconW(NIM_MODIFY, &data_);
    data_.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP | NIF_SHOWTIP;
}

HICON TrayIcon::CreateRecorderIcon() {
    const int iconWidth = ::GetSystemMetrics(SM_CXSMICON);
    const int iconHeight = ::GetSystemMetrics(SM_CYSMICON);
    if (HICON resourceIcon = static_cast<HICON>(::LoadImageW(
            ::GetModuleHandleW(nullptr),
            MAKEINTRESOURCEW(IDI_SUPER_RECORDING),
            IMAGE_ICON,
            iconWidth,
            iconHeight,
            LR_DEFAULTCOLOR)); resourceIcon != nullptr) {
        return resourceIcon;
    }

    constexpr int size = 32;
    BITMAPV5HEADER header{};
    header.bV5Size = sizeof(header);
    header.bV5Width = size;
    header.bV5Height = -size;
    header.bV5Planes = 1;
    header.bV5BitCount = 32;
    header.bV5Compression = BI_BITFIELDS;
    header.bV5RedMask = 0x00FF0000;
    header.bV5GreenMask = 0x0000FF00;
    header.bV5BlueMask = 0x000000FF;
    header.bV5AlphaMask = 0xFF000000;

    void* rawPixels = nullptr;
    HDC screen = ::GetDC(nullptr);
    HBITMAP color = ::CreateDIBSection(
        screen, reinterpret_cast<BITMAPINFO*>(&header), DIB_RGB_COLORS,
        &rawPixels, nullptr, 0);
    ::ReleaseDC(nullptr, screen);
    if (color == nullptr || rawPixels == nullptr) {
        return nullptr;
    }

    auto* pixels = static_cast<std::uint32_t*>(rawPixels);
    std::fill(pixels, pixels + size * size, 0u);
    for (int y = 0; y < size; ++y) {
        for (int x = 0; x < size; ++x) {
            const double dx = static_cast<double>(x) - 15.5;
            const double dy = static_cast<double>(y) - 15.5;
            const double distance = std::sqrt(dx * dx + dy * dy);
            std::uint32_t value = 0;
            if (distance <= 14.5) {
                value = 0xFF367840u;
            }
            if (distance <= 6.0) {
                value = 0xFFFFFFFFu;
            }
            if (x >= 21 && x <= 25 && y >= 12 && y <= 19) {
                value = 0xFFFFFFFFu;
            }
            pixels[y * size + x] = value;
        }
    }

    HBITMAP mask = ::CreateBitmap(size, size, 1, 1, nullptr);
    ICONINFO info{};
    info.fIcon = TRUE;
    info.hbmColor = color;
    info.hbmMask = mask;
    HICON icon = ::CreateIconIndirect(&info);
    ::DeleteObject(color);
    ::DeleteObject(mask);
    return icon;
}

}  // namespace qrec
