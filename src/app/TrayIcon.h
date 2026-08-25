#pragma once

#include <windows.h>
#include <shellapi.h>

#include <string>
#include <string_view>

namespace qrec {

class TrayIcon final {
public:
    TrayIcon() = default;
    ~TrayIcon();
    TrayIcon(const TrayIcon&) = delete;
    TrayIcon& operator=(const TrayIcon&) = delete;

    [[nodiscard]] bool Add(HWND owner, UINT callbackMessage);
    void Remove();
    [[nodiscard]] bool RestoreAfterExplorerRestart();
    void UpdateShortcutLabel(std::wstring_view shortcutLabel);
    void ShowNotification(
        std::wstring_view title,
        std::wstring_view text,
        DWORD flags = NIIF_INFO);

private:
    [[nodiscard]] static HICON CreateRecorderIcon();

    NOTIFYICONDATAW data_{};
    HICON icon_{};
    std::wstring shortcutLabel_{L"F3"};
    bool added_{};
};

}  // namespace qrec
