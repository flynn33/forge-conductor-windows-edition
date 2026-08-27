#pragma once

#include "ForgeConductor/Domain/LegacyContinuityModels.h"

#include <string_view>

namespace ForgeConductor::Contracts {

class ILegacyContextContinuityService {
public:
    virtual ~ILegacyContextContinuityService() = default;

    [[nodiscard]] virtual Domain::Result<Domain::LegacyContinuityPersistOutcome>
    checkpoint(
        const Domain::LegacyContinuityWriteRequest& request,
        const Domain::ClientId& clientId,
        Domain::LegacyHandoffSource source,
        const Domain::OperationContext& context) noexcept = 0;

    [[nodiscard]] virtual Domain::Result<Domain::LegacyContinuityPersistOutcome>
    handoff(
        const Domain::LegacyContinuityWriteRequest& request,
        const Domain::ClientId& clientId,
        Domain::LegacyHandoffSource source,
        const Domain::OperationContext& context) noexcept = 0;

    [[nodiscard]] virtual Domain::Result<Domain::LegacyContinuityPersistOutcome>
    automaticPersist(
        const Domain::LegacyContinuityAutomaticRequest& request,
        const Domain::ClientId& clientId,
        const Domain::OperationContext& context) noexcept = 0;

    [[nodiscard]] virtual Domain::Result<Domain::LegacyContinuityPersistOutcome>
    budgetHandoff(
        const Domain::ClientId& clientId,
        std::string_view reason,
        const Domain::OperationContext& context) noexcept = 0;

    [[nodiscard]] virtual Domain::Result<Domain::LegacyContinuityGetOutcome> get(
        const Domain::LegacyContinuityGetRequest& request,
        const Domain::OperationContext& context) noexcept = 0;

    [[nodiscard]] virtual Domain::Result<Domain::LegacyContinuityListOutcome> list(
        const Domain::LegacyContinuityListRequest& request,
        const Domain::OperationContext& context) noexcept = 0;

    [[nodiscard]] virtual Domain::Result<Domain::LegacyContinuityStatusSummary>
    statusSummary(const Domain::OperationContext&) noexcept
    {
        return Domain::Result<Domain::LegacyContinuityStatusSummary>::failure(
            Domain::makeError(
                Domain::ErrorCodes::HostCapabilityUnavailable,
                "Legacy continuity status is not implemented by this service."));
    }

    [[nodiscard]] virtual Domain::Result<Domain::LegacyContinuityProjectionRepairOutcome>
    repairProjections(const Domain::OperationContext& context) noexcept = 0;

    [[nodiscard]] virtual Domain::Result<Domain::LegacyContinuityResetOutcome>
    reset(
        const Domain::DestructiveConfirmation& confirmation,
        const Domain::OperationContext& context) noexcept = 0;

    virtual void shutdown() noexcept = 0;
};

} // namespace ForgeConductor::Contracts
