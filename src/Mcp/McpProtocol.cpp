#include "ForgeConductor/Mcp/McpProtocol.h"

#include <algorithm>

namespace ForgeConductor::Mcp {
namespace {

[[nodiscard]] bool isWhitespace(const char value) noexcept
{
    return value == ' ' || value == '\t' || value == '\r' || value == '\n' ||
        value == '\f' || value == '\v';
}

[[nodiscard]] std::string_view trim(std::string_view value) noexcept
{
    while (!value.empty() && isWhitespace(value.front())) {
        value.remove_prefix(1U);
    }
    while (!value.empty() && isWhitespace(value.back())) {
        value.remove_suffix(1U);
    }
    return value;
}

} // namespace

std::string_view McpProtocol::negotiate(const std::string_view requested) noexcept
{
    const auto candidate = trim(requested);
    const auto match = std::find(
        SupportedVersions.begin(), SupportedVersions.end(), candidate);
    return match == SupportedVersions.end() ? SupportedVersions.front() : *match;
}

} // namespace ForgeConductor::Mcp
