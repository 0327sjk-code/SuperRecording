#include "common/Logger.h"

#include "common/ProductInfo.h"
#include "common/Win32Helpers.h"

#include <format>
#include <string>

namespace qrec {

Logger::Logger()
    : filePath_(win32::LocalAppDataDirectory() / L"Logs" /
                (std::wstring(product::Name) + L"_" +
                 win32::TimestampForFileName() + L".log")) {
    std::wstring ignoredError;
    static_cast<void>(win32::EnsureDirectory(filePath_.parent_path(), &ignoredError));
    stream_.open(filePath_, std::ios::out | std::ios::app);
}

void Logger::Info(const std::wstring_view message) {
    Write(L"INFO", message);
}

void Logger::Error(const std::wstring_view message) {
    Write(L"ERROR", message);
}

void Logger::Write(const std::wstring_view level, const std::wstring_view message) {
    std::scoped_lock lock(mutex_);
    if (!stream_.is_open()) {
        return;
    }
    SYSTEMTIME local{};
    ::GetLocalTime(&local);
    const std::wstring line = std::format(
        L"{:04}-{:02}-{:02} {:02}:{:02}:{:02} [{}] {}\n",
        local.wYear,
        local.wMonth,
        local.wDay,
        local.wHour,
        local.wMinute,
        local.wSecond,
        level,
        message);
    const int byteCount = ::WideCharToMultiByte(
        CP_UTF8, 0, line.data(), static_cast<int>(line.size()), nullptr, 0, nullptr, nullptr);
    if (byteCount <= 0) {
        return;
    }
    std::string utf8(static_cast<std::size_t>(byteCount), '\0');
    ::WideCharToMultiByte(
        CP_UTF8, 0, line.data(), static_cast<int>(line.size()), utf8.data(), byteCount,
        nullptr, nullptr);
    stream_.write(utf8.data(), static_cast<std::streamsize>(utf8.size()));
    stream_.flush();
}

}  // namespace qrec
