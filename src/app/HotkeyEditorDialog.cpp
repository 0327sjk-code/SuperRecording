#include "app/HotkeyEditorDialog.h"

#include "app/HotkeyCaptureSession.h"
#include "app/HotkeyEditorChrome.h"
#include "app/resource.h"

#include <windowsx.h>

#include <algorithm>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

#pragma comment(lib, "user32.lib")

namespace qrec {
namespace {

constexpr wchar_t kWindowClassName[] = L"SuperRecording.HotkeyEditorDialog";
constexpr wchar_t kWindowTitle[] = L"SuperRecording · 录制快捷键";

void AppendDisplayToken(std::wstring& destination, const std::wstring_view token) {
    if (!destination.empty()) {
        destination += L" + ";
    }
    destination.append(token);
}

[[nodiscard]] std::wstring ModifierCaptureText(const UINT modifiers) {
    std::wstring text;
    if ((modifiers & MOD_CONTROL) != 0U) {
        AppendDisplayToken(text, L"Ctrl");
    }
    if ((modifiers & MOD_ALT) != 0U) {
        AppendDisplayToken(text, L"Alt");
    }
    if ((modifiers & MOD_SHIFT) != 0U) {
        AppendDisplayToken(text, L"Shift");
    }
    if ((modifiers & MOD_WIN) != 0U) {
        AppendDisplayToken(text, L"Win");
    }
    if (text.empty()) {
        return L"请按下快捷键…";
    }
    text += L" + …";
    return text;
}

}  // namespace

class HotkeyEditorDialog::Impl final {
public:
    Impl(const HINSTANCE instance, const HWND owner)
        : instance_(instance), owner_(owner) {}

    ~Impl() {
        captureSession_.Stop();
        if (IsOpen()) {
            ::DestroyWindow(window_);
        }
        chrome_.Shutdown();
    }

    [[nodiscard]] bool Open(
        const HotkeyBinding& currentBinding,
        HotkeySaveCallback saveCallback,
        std::wstring* errorMessage) {
        if (!saveCallback) {
            if (errorMessage != nullptr) {
                *errorMessage = L"快捷键保存回调不可用。";
            }
            return false;
        }
        if (IsOpen()) {
            ::ShowWindow(window_, SW_RESTORE);
            ::SetForegroundWindow(window_);
            StartCapture();
            return true;
        }
        if (!RegisterWindowClass()) {
            if (errorMessage != nullptr) {
                *errorMessage = L"无法注册快捷键设置窗口。";
            }
            return false;
        }

        candidate_ = IsValidHotkeyBinding(currentBinding)
            ? currentBinding
            : DefaultHotkeyBinding();
        hasCandidate_ = true;
        saveCallback_ = std::move(saveCallback);
        displayText_ = FormatHotkeyBinding(candidate_);
        inlineMessage_ = L"字母和数字必须搭配 Ctrl、Alt、Shift 或 Win。";
        inlineTone_ = HotkeyEditorTone::Neutral;
        closeAfterKeyRelease_ = false;
        creationError_.clear();

        const UINT dpi = owner_ != nullptr && ::IsWindow(owner_) != FALSE
            ? ::GetDpiForWindow(owner_)
            : USER_DEFAULT_SCREEN_DPI;
        RECT windowBounds{
            0,
            0,
            ::MulDiv(
                HotkeyEditorChrome::ClientWidth,
                static_cast<int>(dpi),
                USER_DEFAULT_SCREEN_DPI),
            ::MulDiv(
                HotkeyEditorChrome::ClientHeight,
                static_cast<int>(dpi),
                USER_DEFAULT_SCREEN_DPI)};
        ::AdjustWindowRectExForDpi(
            &windowBounds,
            WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU,
            FALSE,
            WS_EX_TOOLWINDOW,
            dpi);

        window_ = ::CreateWindowExW(
            WS_EX_TOOLWINDOW,
            kWindowClassName,
            kWindowTitle,
            WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_CLIPCHILDREN,
            CW_USEDEFAULT,
            CW_USEDEFAULT,
            windowBounds.right - windowBounds.left,
            windowBounds.bottom - windowBounds.top,
            owner_,
            nullptr,
            instance_,
            this);
        if (window_ == nullptr) {
            if (errorMessage != nullptr) {
                *errorMessage = creationError_.empty()
                    ? L"无法创建快捷键设置窗口。"
                    : creationError_;
            }
            return false;
        }

        chrome_.RefreshWindowChrome();
        chrome_.UpdateWindowRegion();
        CenterOnCursorMonitor();
        ::ShowWindow(window_, SW_SHOW);
        ::UpdateWindow(window_);
        ::SetForegroundWindow(window_);
        StartCapture();
        return RunModalMessageLoop(errorMessage);
    }

    void Close() noexcept {
        if (IsOpen()) {
            ::SendMessageW(window_, WM_CLOSE, 0, 0);
        }
    }

    [[nodiscard]] HWND WindowHandle() const noexcept { return window_; }

    [[nodiscard]] bool IsOpen() const noexcept {
        return window_ != nullptr && ::IsWindow(window_) != FALSE;
    }

private:
    [[nodiscard]] bool RunModalMessageLoop(std::wstring* errorMessage) {
        MSG message{};
        while (IsOpen()) {
            const BOOL result = ::GetMessageW(&message, nullptr, 0, 0);
            if (result == 0) {
                const int exitCode = static_cast<int>(message.wParam);
                if (IsOpen()) {
                    captureSession_.Stop();
                    ::DestroyWindow(window_);
                }
                ::PostQuitMessage(exitCode);
                return true;
            }
            if (result < 0) {
                const DWORD nativeError = ::GetLastError();
                if (IsOpen()) {
                    captureSession_.Stop();
                    ::DestroyWindow(window_);
                }
                if (errorMessage != nullptr) {
                    *errorMessage = L"快捷键设置窗口的消息循环失败，错误码：" +
                        std::to_wstring(nativeError) + L"。";
                }
                return false;
            }
            ::TranslateMessage(&message);
            ::DispatchMessageW(&message);
        }
        return true;
    }

    [[nodiscard]] bool RegisterWindowClass() const noexcept {
        WNDCLASSEXW existing{};
        existing.cbSize = sizeof(existing);
        if (::GetClassInfoExW(instance_, kWindowClassName, &existing) != FALSE) {
            return true;
        }

        WNDCLASSEXW windowClass{};
        windowClass.cbSize = sizeof(windowClass);
        windowClass.style = CS_HREDRAW | CS_VREDRAW;
        windowClass.lpfnWndProc = &Impl::WindowProc;
        windowClass.hInstance = instance_;
        const HICON productIcon = ::LoadIconW(
            instance_, MAKEINTRESOURCEW(IDI_SUPER_RECORDING));
        windowClass.hIcon = productIcon != nullptr
            ? productIcon
            : ::LoadIconW(nullptr, IDI_APPLICATION);
        windowClass.hIconSm = windowClass.hIcon;
        windowClass.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
        windowClass.hbrBackground = nullptr;
        windowClass.lpszClassName = kWindowClassName;
        return ::RegisterClassExW(&windowClass) != 0 ||
            ::GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
    }

    static LRESULT CALLBACK WindowProc(
        const HWND window,
        const UINT message,
        const WPARAM wParam,
        const LPARAM lParam) noexcept {
        Impl* self = reinterpret_cast<Impl*>(
            ::GetWindowLongPtrW(window, GWLP_USERDATA));
        if (message == WM_NCCREATE) {
            const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lParam);
            self = static_cast<Impl*>(create->lpCreateParams);
            self->window_ = window;
            ::SetWindowLongPtrW(
                window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        }
        if (self != nullptr) {
            try {
                return self->HandleMessage(message, wParam, lParam);
            } catch (...) {
                return 0;
            }
        }
        return ::DefWindowProcW(window, message, wParam, lParam);
    }

    LRESULT HandleMessage(
        const UINT message,
        const WPARAM wParam,
        const LPARAM lParam) {
        switch (message) {
        case WM_CREATE:
            if (!chrome_.Initialize(instance_, window_)) {
                creationError_ = L"无法创建快捷键设置控件。";
                return -1;
            }
            chrome_.Layout();
            return 0;
        case WM_SIZE:
            chrome_.Layout();
            chrome_.UpdateWindowRegion();
            return 0;
        case WM_DPICHANGED: {
            const auto* suggested = reinterpret_cast<const RECT*>(lParam);
            ::SetWindowPos(
                window_,
                nullptr,
                suggested->left,
                suggested->top,
                suggested->right - suggested->left,
                suggested->bottom - suggested->top,
                SWP_NOZORDER | SWP_NOACTIVATE);
            if (!chrome_.RecreateFonts()) {
                creationError_ = L"无法重建快捷键设置字体。";
            }
            chrome_.Layout();
            chrome_.UpdateWindowRegion();
            chrome_.InvalidateAll();
            return 0;
        }
        case WM_THEMECHANGED:
        case WM_SETTINGCHANGE:
            chrome_.RefreshWindowChrome();
            chrome_.UpdateWindowRegion();
            chrome_.InvalidateAll();
            return 0;
        case WM_PAINT:
            chrome_.Paint(CurrentChromeView());
            return 0;
        case WM_DRAWITEM:
            return chrome_.DrawButton(
                reinterpret_cast<const DRAWITEMSTRUCT*>(lParam)) ? TRUE : FALSE;
        case WM_COMMAND:
            if (HIWORD(wParam) == BN_CLICKED) {
                HandleCommand(LOWORD(wParam));
            }
            return 0;
        case WM_MOUSEMOVE:
            chrome_.UpdateCaptureHover(
                POINT{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)});
            return 0;
        case WM_MOUSELEAVE:
            chrome_.ClearCaptureHover();
            return 0;
        case WM_LBUTTONUP: {
            const POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            if (chrome_.HitTestCaptureCard(point)) {
                StartCapture();
            }
            return 0;
        }
        case WM_SETCURSOR:
            if (chrome_.SetCaptureCardCursor(lParam)) {
                return TRUE;
            }
            break;
        case WM_KEYDOWN:
            if (wParam == VK_ESCAPE) {
                ::SendMessageW(window_, WM_CLOSE, 0, 0);
                return 0;
            }
            if (wParam == VK_TAB) {
                chrome_.FocusFirstButton();
                return 0;
            }
            if (wParam == VK_RETURN && !captureSession_.IsActive()) {
                StartCapture();
                return 0;
            }
            break;
        case WM_ACTIVATE:
            if (LOWORD(wParam) == WA_INACTIVE && captureSession_.IsCapturing()) {
                PauseCapture();
            }
            break;
        case WM_TIMER:
            if (chrome_.HandleTimer(static_cast<UINT_PTR>(wParam))) {
                return 0;
            }
            break;
        case WM_ERASEBKGND:
            return 1;
        case WM_CLOSE:
            if (captureSession_.IsDraining()) {
                closeAfterKeyRelease_ = true;
                inlineMessage_ = L"请松开当前按键，窗口随后关闭。";
                inlineTone_ = HotkeyEditorTone::Neutral;
                chrome_.InvalidateInlineMessage();
                return 0;
            }
            captureSession_.Stop();
            ::DestroyWindow(window_);
            return 0;
        case WM_NCDESTROY:
            captureSession_.Stop();
            ::SetWindowLongPtrW(window_, GWLP_USERDATA, 0);
            window_ = nullptr;
            chrome_.Shutdown();
            saveCallback_ = {};
            return 0;
        default:
            break;
        }
        return ::DefWindowProcW(window_, message, wParam, lParam);
    }

    [[nodiscard]] HotkeyEditorChromeView CurrentChromeView() const noexcept {
        return HotkeyEditorChromeView{
            captureSession_.IsCapturing(),
            inlineTone_,
            displayText_,
            inlineMessage_};
    }

    void HandleCommand(const int id) {
        switch (id) {
        case HotkeyEditorChrome::DefaultButtonId:
            RestoreDefaultBinding();
            break;
        case HotkeyEditorChrome::CancelButtonId:
            ::SendMessageW(window_, WM_CLOSE, 0, 0);
            break;
        case HotkeyEditorChrome::SaveButtonId:
            SaveBinding();
            break;
        default:
            break;
        }
    }

    void RestoreDefaultBinding() {
        if (!captureSession_.IsDraining()) {
            captureSession_.Stop();
        }
        candidate_ = DefaultHotkeyBinding();
        hasCandidate_ = true;
        displayText_ = FormatHotkeyBinding(candidate_);
        inlineMessage_ = L"已恢复默认 F3，点击保存后生效。";
        inlineTone_ = HotkeyEditorTone::Success;
        chrome_.SetSaveEnabled(true);
        chrome_.FocusSaveButton();
        chrome_.InvalidateAll();
    }

    void SaveBinding() {
        if (captureSession_.IsCapturing() || !hasCandidate_) {
            inlineMessage_ = L"请先按下新的快捷键。";
            inlineTone_ = HotkeyEditorTone::Error;
            chrome_.InvalidateInlineMessage();
            return;
        }
        const std::wstring validationError = HotkeyValidationError(candidate_);
        if (!validationError.empty()) {
            inlineMessage_ = validationError;
            inlineTone_ = HotkeyEditorTone::Error;
            chrome_.InvalidateAll();
            return;
        }

        std::wstring saveError;
        try {
            saveError = saveCallback_
                ? saveCallback_(candidate_)
                : L"快捷键保存回调不可用。";
        } catch (...) {
            saveError = L"保存快捷键时发生未知错误，请重试。";
        }
        if (!saveError.empty()) {
            inlineMessage_ = std::move(saveError);
            inlineTone_ = HotkeyEditorTone::Error;
            chrome_.InvalidateAll();
            return;
        }
        ::SendMessageW(window_, WM_CLOSE, 0, 0);
    }

    void StartCapture() {
        if (window_ == nullptr) {
            return;
        }
        if (captureSession_.IsDraining()) {
            inlineMessage_ = L"请先松开当前按键，再重新捕获。";
            inlineTone_ = HotkeyEditorTone::Neutral;
            chrome_.InvalidateAll();
            return;
        }

        captureSession_.Stop();
        closeAfterKeyRelease_ = false;
        displayText_ = L"请按下快捷键…";
        inlineMessage_ = L"正在捕获：按 Esc 可取消设置。";
        inlineTone_ = HotkeyEditorTone::Neutral;

        HotkeyCaptureCallbacks callbacks;
        callbacks.modifiersChanged = [this](const UINT modifiers) {
            displayText_ = ModifierCaptureText(modifiers);
            chrome_.InvalidateCaptureCard();
        };
        callbacks.candidateAccepted = [this](const HotkeyBinding& binding) {
            candidate_ = binding;
            hasCandidate_ = true;
            displayText_ = FormatHotkeyBinding(binding);
            inlineMessage_ = L"组合有效，点击保存后立即生效。";
            inlineTone_ = HotkeyEditorTone::Success;
            chrome_.SetSaveEnabled(true);
            chrome_.FocusSaveButton();
            chrome_.InvalidateAll();
        };
        callbacks.candidateRejected = [this](
            const HotkeyBinding&,
            const std::wstring& validationError) {
            inlineMessage_ = validationError;
            inlineTone_ = HotkeyEditorTone::Error;
            chrome_.InvalidateAll();
        };
        callbacks.cancelRequested = [this]() {
            closeAfterKeyRelease_ = true;
            inlineMessage_ = L"请松开当前按键，窗口随后关闭。";
            inlineTone_ = HotkeyEditorTone::Neutral;
            chrome_.SetSaveEnabled(false);
            chrome_.InvalidateAll();
        };
        callbacks.keysReleased = [this](const HotkeyCaptureCompletion completion) {
            const bool shouldClose = closeAfterKeyRelease_ ||
                completion == HotkeyCaptureCompletion::Cancelled;
            closeAfterKeyRelease_ = false;
            if (shouldClose && window_ != nullptr) {
                ::PostMessageW(window_, WM_CLOSE, 0, 0);
            } else {
                chrome_.InvalidateAll();
            }
        };

        const HotkeyCaptureStartResult result =
            captureSession_.Start(std::move(callbacks));
        if (result.Succeeded()) {
            chrome_.SetSaveEnabled(false);
            ::SetFocus(window_);
        } else {
            displayText_ = FormatHotkeyBinding(candidate_);
            inlineTone_ = HotkeyEditorTone::Error;
            if (result.error == HotkeyCaptureStartError::AlreadyInUse) {
                inlineMessage_ = L"另一个快捷键捕获窗口正在工作。";
            } else {
                inlineMessage_ = L"无法启动键盘捕获，请稍后重试（错误码 " +
                    std::to_wstring(result.nativeError) + L"）。";
            }
            chrome_.SetSaveEnabled(hasCandidate_);
        }
        chrome_.InvalidateAll();
    }

    void PauseCapture() {
        captureSession_.Stop();
        closeAfterKeyRelease_ = false;
        displayText_ = FormatHotkeyBinding(candidate_);
        inlineMessage_ = L"捕获已暂停，点击按键区域可继续。";
        inlineTone_ = HotkeyEditorTone::Neutral;
        chrome_.SetSaveEnabled(hasCandidate_);
        chrome_.InvalidateAll();
    }

    void CenterOnCursorMonitor() const noexcept {
        RECT bounds{};
        if (::GetWindowRect(window_, &bounds) == FALSE) {
            return;
        }
        POINT cursor{};
        ::GetCursorPos(&cursor);
        const HMONITOR monitor =
            ::MonitorFromPoint(cursor, MONITOR_DEFAULTTONEAREST);
        MONITORINFO information{};
        information.cbSize = sizeof(information);
        if (monitor == nullptr ||
            ::GetMonitorInfoW(monitor, &information) == FALSE) {
            return;
        }
        const int width = bounds.right - bounds.left;
        const int height = bounds.bottom - bounds.top;
        const int workWidth = information.rcWork.right - information.rcWork.left;
        const int workHeight = information.rcWork.bottom - information.rcWork.top;
        const int x = information.rcWork.left +
            std::max(0, (workWidth - width) / 2);
        const int y = information.rcWork.top +
            std::max(0, (workHeight - height) / 2);
        ::SetWindowPos(
            window_,
            nullptr,
            x,
            y,
            0,
            0,
            SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    }

    HINSTANCE instance_{};
    HWND owner_{};
    HWND window_{};
    HotkeyEditorChrome chrome_{};
    HotkeyCaptureSession captureSession_{};

    HotkeyBinding candidate_{DefaultHotkeyBinding()};
    bool hasCandidate_{};
    bool closeAfterKeyRelease_{};
    HotkeySaveCallback saveCallback_;
    std::wstring displayText_{L"请按下快捷键…"};
    std::wstring inlineMessage_;
    HotkeyEditorTone inlineTone_{HotkeyEditorTone::Neutral};
    std::wstring creationError_;
};

HotkeyEditorDialog::HotkeyEditorDialog(
    const HINSTANCE instance,
    const HWND owner)
    : impl_(std::make_unique<Impl>(instance, owner)) {}

HotkeyEditorDialog::~HotkeyEditorDialog() = default;

bool HotkeyEditorDialog::Open(
    const HotkeyBinding& currentBinding,
    HotkeySaveCallback saveCallback,
    std::wstring* errorMessage) {
    return impl_->Open(currentBinding, std::move(saveCallback), errorMessage);
}

void HotkeyEditorDialog::Close() noexcept {
    impl_->Close();
}

HWND HotkeyEditorDialog::WindowHandle() const noexcept {
    return impl_->WindowHandle();
}

bool HotkeyEditorDialog::IsOpen() const noexcept {
    return impl_->IsOpen();
}

}  // namespace qrec
