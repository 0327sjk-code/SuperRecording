#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>

namespace qrec::ui {

enum class MotionEasing : unsigned char {
    EaseOutQuart,
    EaseOutQuint,
};

namespace motion_detail {

// -1 = not queried yet, 0 = disabled, 1 = enabled. The setting is process-wide
// for the current interactive user, so every control can share one cached value.
inline std::atomic<int> clientAreaAnimationsEnabled{-1};

}  // namespace motion_detail

[[nodiscard]] inline bool RefreshClientAreaAnimationsEnabled() noexcept {
    BOOL enabled = TRUE;
    if (::SystemParametersInfoW(
            SPI_GETCLIENTAREAANIMATION,
            0,
            &enabled,
            0) == FALSE) {
        enabled = TRUE;
    }
    const bool animationsEnabled = enabled != FALSE;
    motion_detail::clientAreaAnimationsEnabled.store(
        animationsEnabled ? 1 : 0,
        std::memory_order_release);
    return animationsEnabled;
}

[[nodiscard]] inline bool ClientAreaAnimationsEnabled() noexcept {
    const int cached = motion_detail::clientAreaAnimationsEnabled.load(
        std::memory_order_acquire);
    if (cached >= 0) {
        return cached != 0;
    }
    return RefreshClientAreaAnimationsEnabled();
}

[[nodiscard]] inline float ApplyMotionEasing(
    const float progress,
    const MotionEasing easing) noexcept {
    const float clamped = std::clamp(progress, 0.0F, 1.0F);
    const float inverse = 1.0F - clamped;
    if (easing == MotionEasing::EaseOutQuint) {
        return 1.0F - inverse * inverse * inverse * inverse * inverse;
    }
    return 1.0F - inverse * inverse * inverse * inverse;
}

class MotionState final {
public:
    using Clock = std::chrono::steady_clock;

    explicit MotionState(const float initialValue = 0.0F) noexcept
        : value_(std::clamp(initialValue, 0.0F, 1.0F)),
          startValue_(value_),
          targetValue_(value_) {}

    void JumpTo(const float target) noexcept {
        value_ = std::clamp(target, 0.0F, 1.0F);
        startValue_ = value_;
        targetValue_ = value_;
        active_ = false;
    }

    [[nodiscard]] bool SetTarget(
        const float target,
        const std::chrono::milliseconds duration,
        const MotionEasing easing,
        const bool animate,
        const Clock::time_point now = Clock::now()) noexcept {
        (void)Advance(now);
        const float clampedTarget = std::clamp(target, 0.0F, 1.0F);
        if (!animate || duration <= std::chrono::milliseconds::zero()) {
            const bool changed = active_ || std::abs(value_ - clampedTarget) > kEpsilon;
            JumpTo(clampedTarget);
            return changed;
        }
        if (std::abs(targetValue_ - clampedTarget) <= kEpsilon) {
            return false;
        }
        startValue_ = value_;
        targetValue_ = clampedTarget;
        startedAt_ = now;
        duration_ = duration;
        easing_ = easing;
        active_ = std::abs(startValue_ - targetValue_) > kEpsilon;
        if (!active_) {
            value_ = targetValue_;
        }
        return true;
    }

    [[nodiscard]] bool Advance(const Clock::time_point now = Clock::now()) noexcept {
        if (!active_) {
            return false;
        }
        const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(now - startedAt_);
        const auto total = std::chrono::duration_cast<std::chrono::microseconds>(duration_);
        if (elapsed >= total || total.count() <= 0) {
            JumpTo(targetValue_);
            return false;
        }
        const float progress = static_cast<float>(elapsed.count()) /
            static_cast<float>(total.count());
        const float eased = ApplyMotionEasing(progress, easing_);
        value_ = startValue_ + (targetValue_ - startValue_) * eased;
        return true;
    }

    [[nodiscard]] float Value() const noexcept { return value_; }
    [[nodiscard]] float Target() const noexcept { return targetValue_; }
    [[nodiscard]] bool IsActive() const noexcept { return active_; }

private:
    static constexpr float kEpsilon = 0.0005F;

    float value_{};
    float startValue_{};
    float targetValue_{};
    Clock::time_point startedAt_{};
    std::chrono::milliseconds duration_{};
    MotionEasing easing_{MotionEasing::EaseOutQuart};
    bool active_{};
};

[[nodiscard]] inline COLORREF InterpolateColor(
    const COLORREF from,
    const COLORREF to,
    const float amount) noexcept {
    const float clamped = std::clamp(amount, 0.0F, 1.0F);
    const auto channel = [clamped](const BYTE first, const BYTE second) noexcept -> BYTE {
        const float value = static_cast<float>(first) +
            (static_cast<float>(second) - static_cast<float>(first)) * clamped;
        return static_cast<BYTE>(std::clamp(std::lround(value), 0L, 255L));
    };
    return RGB(
        channel(GetRValue(from), GetRValue(to)),
        channel(GetGValue(from), GetGValue(to)),
        channel(GetBValue(from), GetBValue(to)));
}

[[nodiscard]] inline BYTE MotionAlpha(
    const float amount,
    const BYTE maximum = 255) noexcept {
    const float scaled = std::clamp(amount, 0.0F, 1.0F) * static_cast<float>(maximum);
    return static_cast<BYTE>(std::clamp(std::lround(scaled), 0L, 255L));
}

}  // namespace qrec::ui
