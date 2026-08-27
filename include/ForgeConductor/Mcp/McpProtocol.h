#pragma once

#include <array>
#include <string_view>

namespace ForgeConductor::Mcp {

class McpProtocol final {
public:
    static constexpr std::array<std::string_view, 4U> SupportedVersions{
        "2025-11-25",
        "2025-06-18",
        "2025-03-26",
        "2024-11-05"};

    [[nodiscard]] static std::string_view negotiate(
        std::string_view requested) noexcept;
};

} // namespace ForgeConductor::Mcp
