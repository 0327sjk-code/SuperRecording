#pragma once

#include "update/UpdateTypes.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace qrec::update::detail {

struct ParsedHttpsUrl final {
    std::wstring host;
    std::wstring objectName;
    std::uint16_t port{};
};

[[nodiscard]] bool HasInvalidHeaderCharacters(
    std::wstring_view value) noexcept;

[[nodiscard]] std::optional<ParsedHttpsUrl> ParseHttpsUrl(
    std::wstring_view url,
    UpdateFailure* failure);

// requestHandle is a WinHTTP HINTERNET kept opaque in this private header.
[[nodiscard]] bool ValidateFinalRequestUrl(
    void* requestHandle,
    UpdateFailure* failure);

}  // namespace qrec::update::detail
