#pragma once

#include "ForgeConductor/Domain/LegacyContinuityModels.h"

#include <cstddef>
#include <optional>
#include <vector>

namespace ForgeConductor::Contracts {

// Narrow read-only bridge to durable agent-session state. The source must
// return only open sessions currently owned by clientId. Application validates
// the returned bound and unique session identities before persisting them.
class ILegacyContinuitySessionSource {
public:
    virtual ~ILegacyContinuitySessionSource() = default;

    [[nodiscard]] virtual Domain::Result<std::vector<Domain::LegacyAgentContinuitySnapshot>>
    listOpenForClient(
        const Domain::ClientId& clientId,
        std::size_t maximumCount,
        const Domain::OperationContext& context) noexcept = 0;

    [[nodiscard]] virtual Domain::Result<bool> isOpen(
        const Domain::SessionId& sessionId,
        const Domain::OperationContext& context) noexcept = 0;

    [[nodiscard]] virtual Domain::Result<std::optional<Domain::LegacyActiveBindingSnapshot>>
    binding(
        const Domain::ClientId& clientId,
        const Domain::OperationContext& context) noexcept = 0;
};

} // namespace ForgeConductor::Contracts
