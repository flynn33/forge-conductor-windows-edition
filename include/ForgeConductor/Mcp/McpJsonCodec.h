#pragma once

#include "ForgeConductor/Domain/Result.h"

#include <cstddef>
#include <string>
#include <string_view>

namespace ForgeConductor::Mcp {

// Canonicalizes one bounded JSON value. Objects are emitted with recursively
// sorted keys, no insignificant whitespace, and no trailing newline.
class McpJsonCodec final {
public:
    static constexpr std::size_t MaximumDocumentBytes = 1024U * 1024U;
    static constexpr std::size_t MaximumNestingDepth = 64U;

    [[nodiscard]] Domain::Result<std::string> canonicalize(
        std::string_view utf8Json) const noexcept;
};

} // namespace ForgeConductor::Mcp
