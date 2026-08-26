#pragma once

#include <string_view>

namespace ForgeConductor::Domain {

[[nodiscard]] bool isValidUtf8(std::string_view value) noexcept;

} // namespace ForgeConductor::Domain

