#pragma once

#include "ForgeConductor/Domain/OperationContext.h"
#include "ForgeConductor/Domain/Result.h"

#include <cstdint>
#include <optional>
#include <string>

namespace ForgeConductor::Contracts {

struct GitHubReadRequest final {
    std::string repository;
    std::string operation;
    std::optional<std::uint64_t> id;
    std::optional<std::string> ref;
    std::uint32_t page{1U};
    std::uint32_t perPage{30U};
};

// Specific repository inspection operations; no caller-supplied URL or HTTP method.
class IGitHubReadService {
public:
    virtual ~IGitHubReadService() = default;
    [[nodiscard]] virtual Domain::Result<std::string> read(
        const GitHubReadRequest& request,
        const Domain::OperationContext& context) noexcept = 0;
};

} // namespace ForgeConductor::Contracts
