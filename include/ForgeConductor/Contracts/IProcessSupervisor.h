#pragma once

#include "ForgeConductor/Contracts/AuthorityCapabilities.h"
#include "ForgeConductor/Domain/Domain.h"

#include <cstddef>

namespace ForgeConductor::Contracts {

class IProcessSupervisor {
public:
    static constexpr std::size_t MaximumConcurrentOperations =
        Domain::MaximumConcurrentProcessOperations;

    virtual ~IProcessSupervisor() = default;

    [[nodiscard]] virtual Domain::Result<Domain::ProcessResult> run(
        const Domain::ProcessRequest& request,
        const WorkspaceAuthority& authority,
        const Domain::OperationContext& context) noexcept = 0;

    virtual void cancel(const Domain::OperationId& operationId) noexcept = 0;
    virtual void cancelAll() noexcept = 0;
    virtual void shutdown() noexcept = 0;
};

} // namespace ForgeConductor::Contracts
