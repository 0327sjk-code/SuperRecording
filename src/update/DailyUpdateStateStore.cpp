#include "update/DailyUpdateStateStore.h"
#include "update/DailyUpdatePolicy.h"

#include <windows.h>
#include <string>
#include <system_error>
#include <utility>

namespace qrec::update {
namespace {
constexpr wchar_t Section[] = L"AutomaticUpdate";
constexpr wchar_t CompletedDateKey[] = L"CompletedLocalDate";
}

DailyUpdateStateStore::DailyUpdateStateStore(std::filesystem::path path)
    : path_(std::move(path)) {}

std::uint32_t DailyUpdateStateStore::LoadCompletedDate() const noexcept {
    const auto date = ::GetPrivateProfileIntW(Section, CompletedDateKey, 0, path_.c_str());
    return DailyUpdatePolicy::IsValidDate(date) ? date : 0;
}

bool DailyUpdateStateStore::SaveCompletedDate(const std::uint32_t date) const noexcept {
    if (!DailyUpdatePolicy::IsValidDate(date)) return false;
    try {
        std::error_code error;
        std::filesystem::create_directories(path_.parent_path(), error);
        if (error) return false;
        const auto value = std::to_wstring(date);
        if (!::WritePrivateProfileStringW(Section, CompletedDateKey, value.c_str(), path_.c_str()))
            return false;
        // Flush before handing off to the next process, avoiding a restart/check loop.
        // Win32 documents zero as the normal return value for this flush form.
        static_cast<void>(::WritePrivateProfileStringW(nullptr, nullptr, nullptr, path_.c_str()));
        return LoadCompletedDate() == date;
    } catch (...) { return false; }
}
}  // namespace qrec::update
