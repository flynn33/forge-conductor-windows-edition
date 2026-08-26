#pragma once

#include "ForgeConductor/Domain/Result.h"

#include <string>
#include <string_view>

namespace ForgeConductor::Infrastructure::Windows::Detail {

[[nodiscard]] Domain::Result<std::wstring> strictUtf8ToUtf16(
    std::string_view value) noexcept;
[[nodiscard]] Domain::Result<std::string> strictUtf16ToUtf8(
    std::wstring_view value) noexcept;

} // namespace ForgeConductor::Infrastructure::Windows::Detail
