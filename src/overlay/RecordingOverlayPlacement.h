#pragma once

#include <windows.h>

#include "../common/Types.h"

namespace qrec::overlay::placement {

[[nodiscard]] UINT DpiAt(POINT screenPoint) noexcept;
[[nodiscard]] RECT AutomaticBounds(const IntRect& recordingRegion, SIZE windowSize, UINT dpi) noexcept;
[[nodiscard]] RECT ClampToWorkArea(RECT windowBounds) noexcept;

}  // namespace qrec::overlay::placement
