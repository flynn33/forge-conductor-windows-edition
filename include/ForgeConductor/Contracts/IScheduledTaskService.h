#pragma once

#include "ForgeConductor/Contracts/AuthorityCapabilities.h"
#include "ForgeConductor/Domain/OperationContext.h"
#include "ForgeConductor/Domain/Result.h"
#include <string>
#include <string_view>

namespace ForgeConductor::Contracts {
class IScheduledTaskService {
public:
    virtual ~IScheduledTaskService() = default;
    [[nodiscard]] virtual Domain::Result<std::string> execute(std::string_view name,
        std::string_view arguments, const WorkspaceAuthority& authority,
        const Domain::OperationContext& context) noexcept = 0;
    [[nodiscard]] virtual Domain::Result<void> initialize(const Domain::OperationContext&) noexcept = 0;
    [[nodiscard]] virtual Domain::Result<void> tick(const Domain::OperationContext&) noexcept = 0;
    [[nodiscard]] virtual Domain::Result<void> start() noexcept = 0;
    virtual void shutdown() noexcept = 0;
};
} // namespace ForgeConductor::Contracts
