#pragma once

#include <filesystem>
#include <fstream>
#include <mutex>
#include <string_view>

namespace qrec {

class Logger final {
public:
    Logger();
    void Info(std::wstring_view message);
    void Error(std::wstring_view message);
    [[nodiscard]] const std::filesystem::path& FilePath() const noexcept { return filePath_; }

private:
    void Write(std::wstring_view level, std::wstring_view message);

    std::filesystem::path filePath_;
    std::ofstream stream_;
    std::mutex mutex_;
};

}  // namespace qrec
