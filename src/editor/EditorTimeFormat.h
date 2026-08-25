#pragma once

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <format>
#include <string>

namespace qrec {

inline std::wstring FormatEditorTime(const std::chrono::milliseconds value) {
    const std::int64_t totalMilliseconds = std::max<std::int64_t>(0, value.count());
    const std::int64_t totalSeconds = totalMilliseconds / 1000;
    const std::int64_t hours = totalSeconds / 3600;
    const std::int64_t minutes = (totalSeconds / 60) % 60;
    const std::int64_t seconds = totalSeconds % 60;
    const std::int64_t hundredths = (totalMilliseconds % 1000) / 10;
    if (hours > 0) {
        return std::format(L"{:02}:{:02}:{:02}.{:02}", hours, minutes, seconds, hundredths);
    }
    return std::format(L"{:02}:{:02}.{:02}", minutes, seconds, hundredths);
}

}  // namespace qrec
