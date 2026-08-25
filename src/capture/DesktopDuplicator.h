#pragma once

#include "../common/Types.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace qrec::capture {

struct DesktopFrame final {
    std::vector<std::uint8_t> bgra;
    std::uint32_t width{};
    std::uint32_t height{};
    std::uint32_t stride{};
};

enum class FrameAcquireStatus : std::uint8_t {
    FrameReady,
    NoDesktopChange,
    AccessLost,
    Failed,
};

enum class DesktopDuplicatorError : std::uint8_t {
    None,
    InvalidRegion,
    DisplayNotFound,
    CrossDisplayRegion,
    UnsupportedRotation,
    GraphicsInitialization,
    DuplicationInitialization,
    FrameAcquisition,
};

struct DesktopDuplicatorOptions final {
    IntRect region{};
    bool includeCursor{true};
};

class DesktopDuplicator final {
public:
    DesktopDuplicator();
    ~DesktopDuplicator();

    DesktopDuplicator(const DesktopDuplicator&) = delete;
    DesktopDuplicator& operator=(const DesktopDuplicator&) = delete;

    [[nodiscard]] bool Initialize(
        const DesktopDuplicatorOptions& options,
        std::wstring& errorMessage,
        long& nativeError,
        DesktopDuplicatorError* errorCategory = nullptr) noexcept;

    [[nodiscard]] FrameAcquireStatus AcquireFrame(
        DesktopFrame& destination,
        std::uint32_t timeoutMilliseconds,
        std::wstring& errorMessage,
        long& nativeError) noexcept;

    void Reset() noexcept;

    [[nodiscard]] bool IsInitialized() const noexcept;
    [[nodiscard]] IntRect Region() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace qrec::capture
