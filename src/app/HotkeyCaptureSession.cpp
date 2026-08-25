#include "app/HotkeyCaptureSession.h"

#include <algorithm>
#include <array>
#include <utility>

namespace qrec {

HotkeyCaptureSession::~HotkeyCaptureSession() {
    Stop();
}

HotkeyCaptureStartResult HotkeyCaptureSession::Start(
    HotkeyCaptureCallbacks callbacks) {
    if (activeSession_ != nullptr && activeSession_ != this) {
        return {HotkeyCaptureStartError::AlreadyInUse, ERROR_BUSY};
    }

    Stop();
    callbacks_ = std::move(callbacks);
    pressedKeys_.fill(false);
    SeedModifierState();
    state_ = HotkeyCaptureState::Capturing;
    activeSession_ = this;

    keyboardHook_ = ::SetWindowsHookExW(
        WH_KEYBOARD_LL,
        &HotkeyCaptureSession::LowLevelKeyboardProc,
        ::GetModuleHandleW(nullptr),
        0);
    if (keyboardHook_ == nullptr) {
        const DWORD nativeError = ::GetLastError();
        Stop();
        return {HotkeyCaptureStartError::HookInstallationFailed, nativeError};
    }

    if (callbacks_.modifiersChanged) {
        callbacks_.modifiersChanged(CurrentModifiers());
    }
    return {};
}

void HotkeyCaptureSession::Stop() noexcept {
    state_ = HotkeyCaptureState::Idle;
    if (keyboardHook_ != nullptr) {
        ::UnhookWindowsHookEx(keyboardHook_);
        keyboardHook_ = nullptr;
    }
    if (activeSession_ == this) {
        activeSession_ = nullptr;
    }
    pressedKeys_.fill(false);
    callbacks_ = {};
    pendingCompletion_ = HotkeyCaptureCompletion::CandidateAccepted;
}

HotkeyCaptureState HotkeyCaptureSession::State() const noexcept {
    return state_;
}

bool HotkeyCaptureSession::IsActive() const noexcept {
    return state_ != HotkeyCaptureState::Idle && keyboardHook_ != nullptr;
}

bool HotkeyCaptureSession::IsCapturing() const noexcept {
    return state_ == HotkeyCaptureState::Capturing && keyboardHook_ != nullptr;
}

bool HotkeyCaptureSession::IsDraining() const noexcept {
    return state_ == HotkeyCaptureState::Draining && keyboardHook_ != nullptr;
}

UINT HotkeyCaptureSession::CurrentModifiers() const noexcept {
    UINT modifiers = 0U;
    if (pressedKeys_[VK_CONTROL] || pressedKeys_[VK_LCONTROL] ||
        pressedKeys_[VK_RCONTROL]) {
        modifiers |= MOD_CONTROL;
    }
    if (pressedKeys_[VK_MENU] || pressedKeys_[VK_LMENU] ||
        pressedKeys_[VK_RMENU]) {
        modifiers |= MOD_ALT;
    }
    if (pressedKeys_[VK_SHIFT] || pressedKeys_[VK_LSHIFT] ||
        pressedKeys_[VK_RSHIFT]) {
        modifiers |= MOD_SHIFT;
    }
    if (pressedKeys_[VK_LWIN] || pressedKeys_[VK_RWIN]) {
        modifiers |= MOD_WIN;
    }
    return modifiers;
}

bool HotkeyCaptureSession::ProcessKeyboardEvent(
    const HotkeyCaptureEvent& event) {
    if (!IsActive()) {
        return false;
    }

    const bool keyDown =
        event.message == WM_KEYDOWN || event.message == WM_SYSKEYDOWN;
    const bool keyUp =
        event.message == WM_KEYUP || event.message == WM_SYSKEYUP;
    if (keyDown || keyUp) {
        if (state_ == HotkeyCaptureState::Draining) {
            HandleDrainingEvent(event.virtualKey, keyDown, keyUp);
        } else if (state_ == HotkeyCaptureState::Capturing) {
            HandleCapturingEvent(event.virtualKey, keyDown, keyUp);
        }
    }

    // The active capture transaction owns all input, even if the event is not
    // a key transition understood above. This prevents partial Win/Alt chords
    // from leaking to Windows or the previously focused application.
    return true;
}

LRESULT CALLBACK HotkeyCaptureSession::LowLevelKeyboardProc(
    const int code,
    const WPARAM wParam,
    const LPARAM lParam) noexcept {
    HotkeyCaptureSession* const session = activeSession_;
    if (code < 0 || session == nullptr || session->keyboardHook_ == nullptr) {
        return ::CallNextHookEx(nullptr, code, wParam, lParam);
    }

    const auto* const nativeEvent =
        reinterpret_cast<const KBDLLHOOKSTRUCT*>(lParam);
    if (nativeEvent == nullptr) {
        return 1;
    }

    try {
        const HotkeyCaptureEvent event{
            wParam,
            static_cast<UINT>(nativeEvent->vkCode),
            nativeEvent->flags,
            nativeEvent->dwExtraInfo};
        if (session->ProcessKeyboardEvent(event)) {
            return 1;
        }
    } catch (...) {
        // Never allow a client callback exception to escape a Win32 hook.
        return 1;
    }
    return ::CallNextHookEx(nullptr, code, wParam, lParam);
}

void HotkeyCaptureSession::SeedModifierState() noexcept {
    constexpr std::array modifierKeys{
        VK_LCONTROL,
        VK_RCONTROL,
        VK_LMENU,
        VK_RMENU,
        VK_LSHIFT,
        VK_RSHIFT,
        VK_LWIN,
        VK_RWIN};
    for (const int virtualKey : modifierKeys) {
        pressedKeys_[static_cast<std::size_t>(virtualKey)] =
            (::GetAsyncKeyState(virtualKey) & 0x8000) != 0;
    }
}

void HotkeyCaptureSession::UpdateKeyState(
    const UINT virtualKey,
    const bool keyDown) noexcept {
    if (virtualKey < pressedKeys_.size()) {
        pressedKeys_[virtualKey] = keyDown;
    }
}

void HotkeyCaptureSession::HandleCapturingEvent(
    const UINT virtualKey,
    const bool keyDown,
    const bool keyUp) {
    if (virtualKey == VK_ESCAPE) {
        UpdateKeyState(virtualKey, keyDown);
        if (keyDown) {
            BeginDrain(HotkeyCaptureCompletion::Cancelled);
            if (callbacks_.cancelRequested) {
                callbacks_.cancelRequested();
            }
        }
        return;
    }

    if (IsModifierVirtualKey(virtualKey)) {
        const UINT previousModifiers = CurrentModifiers();
        UpdateKeyState(virtualKey, keyDown);
        const UINT currentModifiers = CurrentModifiers();
        if (previousModifiers != currentModifiers && callbacks_.modifiersChanged) {
            callbacks_.modifiersChanged(currentModifiers);
        }
        return;
    }

    if (keyUp) {
        UpdateKeyState(virtualKey, false);
        return;
    }
    if (!keyDown) {
        return;
    }

    const bool wasAlreadyPressed =
        virtualKey < pressedKeys_.size() && pressedKeys_[virtualKey];
    UpdateKeyState(virtualKey, true);
    if (wasAlreadyPressed) {
        return;
    }

    const HotkeyBinding attempted{CurrentModifiers(), virtualKey};
    const std::wstring validationError = HotkeyValidationError(attempted);
    if (!validationError.empty()) {
        if (callbacks_.candidateRejected) {
            callbacks_.candidateRejected(attempted, validationError);
        }
        return;
    }

    BeginDrain(HotkeyCaptureCompletion::CandidateAccepted);
    if (callbacks_.candidateAccepted) {
        callbacks_.candidateAccepted(attempted);
    }
}

void HotkeyCaptureSession::HandleDrainingEvent(
    const UINT virtualKey,
    const bool keyDown,
    const bool keyUp) {
    UpdateKeyState(virtualKey, keyDown);
    if (keyUp && !AnyCapturedKeyPressed()) {
        CompleteDrain();
    }
}

void HotkeyCaptureSession::BeginDrain(
    const HotkeyCaptureCompletion completion) {
    pendingCompletion_ = completion;
    state_ = HotkeyCaptureState::Draining;
}

void HotkeyCaptureSession::CompleteDrain() {
    const HotkeyCaptureCompletion completion = pendingCompletion_;
    auto callback = std::move(callbacks_.keysReleased);
    Stop();
    if (callback) {
        callback(completion);
    }
}

bool HotkeyCaptureSession::AnyCapturedKeyPressed() const noexcept {
    return std::ranges::any_of(pressedKeys_, [](const bool pressed) {
        return pressed;
    });
}

bool HotkeyCaptureSession::IsModifierVirtualKey(
    const UINT virtualKey) noexcept {
    switch (virtualKey) {
    case VK_CONTROL:
    case VK_LCONTROL:
    case VK_RCONTROL:
    case VK_MENU:
    case VK_LMENU:
    case VK_RMENU:
    case VK_SHIFT:
    case VK_LSHIFT:
    case VK_RSHIFT:
    case VK_LWIN:
    case VK_RWIN:
        return true;
    default:
        return false;
    }
}

}  // namespace qrec
