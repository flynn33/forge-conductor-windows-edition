#pragma once

#include "ForgeConductor/Domain/OperationContext.h"
#include "ForgeConductor/Domain/Result.h"
#include <string>
#include <string_view>

namespace ForgeConductor::Contracts {
// Explicit HTTP(S) access. Mutating verbs may change a remote service; no local file writes.
class IWebAccessService {
public:
    virtual ~IWebAccessService() = default;
    [[nodiscard]] virtual Domain::Result<std::string> execute(
        std::string_view toolName, std::string_view arguments,
        const Domain::OperationContext& context) noexcept = 0;
};
} // namespace ForgeConductor::Contracts
