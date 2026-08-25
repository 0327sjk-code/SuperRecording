#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "common/HotkeyBinding.h"

#include <windows.h>

#include <array>
#include <functional>
#include <string>

namespace qrec {

enum class HotkeyCaptureState : unsigned char {
    Idle,
    Capturing,
    Draining,
};

enum class HotkeyCaptureCompletion : unsigned char {
    CandidateAccepted,
    Cancelled,
};

enum class HotkeyCaptureStartError : unsigned char {
    None,
    AlreadyInUse,
    HookInstallationFailed,
};

struct HotkeyCaptureStartResult final {
    HotkeyCaptureStartError error{HotkeyCaptureStartError::None};
    DWORD nativeError{};

    [[nodiscard]] bool Succeeded() const noexcept {
        return error == HotkeyCaptureStartError::None;
    }
};

// A normalized low-level keyboard event. Injected events are intentionally
// accepted: the capture UI must behave identically for physical, accessibility,
// on-screen-keyboard, and automation input.
struct HotkeyCaptureEvent final {
    WPARAM message{};
    UINT virtualKey{};
    DWORD flags{};
    ULONG_PTR extraInfo{};

    [[nodiscard]] bool IsInjected() const noexcept {
        return (flags & (LLKHF_INJECTED | LLKHF_LOWER_IL_INJECTED)) != 0U;
    }
};

struct HotkeyCaptureCallbacks final {
    // Fired on capture start and whenever the effective modifier mask changes.
    std::function<void(UINT modifiers)> modifiersChanged;
    // Fired immediately after a valid main key enters the drain phase.
    std::function<void(const HotkeyBinding& binding)> candidateAccepted;
    // Keeps the session in capture mode; the dialog owns presentation of error.
    std::function<void(
        const HotkeyBinding& binding,
        const std::wstring& validationError)> candidateRejected;
    // Fired once on Esc key-down, after the session has entered the drain phase.
    std::function<void()> cancelRequested;
    // Fired after every captured key has been released and the hook is removed.
    std::function<void(HotkeyCaptureCompletion completion)> keysReleased;
};

// Owns one complete global-keyboard capture transaction. Windows permits only
// one active instance in this process; Start reports contention without
// disturbing the existing owner.
class HotkeyCaptureSession final {
public:
    HotkeyCaptureSession() = default;
    ~HotkeyCaptureSession();

    HotkeyCaptureSession(const HotkeyCaptureSession&) = delete;
    HotkeyCaptureSession& operator=(const HotkeyCaptureSession&) = delete;
    HotkeyCaptureSession(HotkeyCaptureSession&&) = delete;
    HotkeyCaptureSession& operator=(HotkeyCaptureSession&&) = delete;

    [[nodiscard]] HotkeyCaptureStartResult Start(HotkeyCaptureCallbacks callbacks);
    void Stop() noexcept;

    [[nodiscard]] HotkeyCaptureState State() const noexcept;
    [[nodiscard]] bool IsActive() const noexcept;
    [[nodiscard]] bool IsCapturing() const noexcept;
    [[nodiscard]] bool IsDraining() const noexcept;
    [[nodiscard]] UINT CurrentModifiers() const noexcept;

    // Returns true when the caller must suppress the event. While active, every
    // keyboard event is suppressed, including unsupported and injected input.
    // This public boundary also permits deterministic event injection in tests.
    [[nodiscard]] bool ProcessKeyboardEvent(const HotkeyCaptureEvent& event);

private:
    static LRESULT CALLBACK LowLevelKeyboardProc(
        int code,
        WPARAM wParam,
        LPARAM lParam) noexcept;

    void SeedModifierState() noexcept;
    void UpdateKeyState(UINT virtualKey, bool keyDown) noexcept;
    void HandleCapturingEvent(UINT virtualKey, bool keyDown, bool keyUp);
    void HandleDrainingEvent(UINT virtualKey, bool keyDown, bool keyUp);
    void BeginDrain(HotkeyCaptureCompletion completion);
    void CompleteDrain();
    [[nodiscard]] bool AnyCapturedKeyPressed() const noexcept;
    [[nodiscard]] static bool IsModifierVirtualKey(UINT virtualKey) noexcept;

    HHOOK keyboardHook_{};
    std::array<bool, 256> pressedKeys_{};
    HotkeyCaptureCallbacks callbacks_{};
    HotkeyCaptureState state_{HotkeyCaptureState::Idle};
    HotkeyCaptureCompletion pendingCompletion_{
        HotkeyCaptureCompletion::CandidateAccepted};

    inline static HotkeyCaptureSession* activeSession_{};
};

}  // namespace qrec
