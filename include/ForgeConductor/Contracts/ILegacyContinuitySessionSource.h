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

    // Global source-compatible count used by forge_status continuity summary.
    // Implementations fail when the durable count exceeds maximumCount rather
    // than truncating a value that is reported as exact.
    [[nodiscard]] virtual Domain::Result<std::size_t> countOpen(
        std::size_t,
        const Domain::OperationContext&) noexcept
    {
        return Domain::Result<std::size_t>::failure(Domain::makeError(
            Domain::ErrorCodes::HostCapabilityUnavailable,
            "Global open agent-session count is not implemented by this source."));
    }

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
