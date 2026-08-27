#pragma once

#include "ForgeConductor/Contracts/IContinuityProjectionStore.h"
#include "ForgeConductor/Contracts/IFoundationServices.h"
#include "ForgeConductor/Contracts/ILegacyContextContinuityService.h"
#include "ForgeConductor/Contracts/ILegacyContinuityRepository.h"
#include "ForgeConductor/Contracts/ILegacyContinuitySessionSource.h"

#include <memory>

namespace ForgeConductor::Application {

// All injected dependencies are composition-owned and must outlive this
// service. shutdown drains admitted synchronous work, then closes only the
// continuity repository/projection admissions. The agent-session source is
// borrowed and remains owned by the agent composition root.
class LegacyContextContinuityService final
    : public Contracts::ILegacyContextContinuityService {
public:
    LegacyContextContinuityService(
        Contracts::ILegacyContinuityRepository& repository,
        Contracts::IContinuityProjectionStore& projections,
        Contracts::ILegacyContinuitySessionSource& sessions,
        Contracts::IClock& clock,
        Contracts::IUuidGenerator& uuidGenerator);
    ~LegacyContextContinuityService() noexcept override;

    LegacyContextContinuityService(const LegacyContextContinuityService&) = delete;
    LegacyContextContinuityService& operator=(const LegacyContextContinuityService&) = delete;
    LegacyContextContinuityService(LegacyContextContinuityService&&) = delete;
    LegacyContextContinuityService& operator=(LegacyContextContinuityService&&) = delete;

    [[nodiscard]] Domain::Result<Domain::LegacyContinuityPersistOutcome>
    checkpoint(
        const Domain::LegacyContinuityWriteRequest& request,
        const Domain::ClientId& clientId,
        Domain::LegacyHandoffSource source,
        const Domain::OperationContext& context) noexcept override;

    [[nodiscard]] Domain::Result<Domain::LegacyContinuityPersistOutcome>
    handoff(
        const Domain::LegacyContinuityWriteRequest& request,
        const Domain::ClientId& clientId,
        Domain::LegacyHandoffSource source,
        const Domain::OperationContext& context) noexcept override;

    [[nodiscard]] Domain::Result<Domain::LegacyContinuityPersistOutcome>
    automaticPersist(
        const Domain::LegacyContinuityAutomaticRequest& request,
        const Domain::ClientId& clientId,
        const Domain::OperationContext& context) noexcept override;

    [[nodiscard]] Domain::Result<Domain::LegacyContinuityPersistOutcome>
    budgetHandoff(
        const Domain::ClientId& clientId,
        std::string_view reason,
        const Domain::OperationContext& context) noexcept override;

    [[nodiscard]] Domain::Result<Domain::LegacyContinuityGetOutcome> get(
        const Domain::LegacyContinuityGetRequest& request,
        const Domain::OperationContext& context) noexcept override;

    [[nodiscard]] Domain::Result<Domain::LegacyContinuityListOutcome> list(
        const Domain::LegacyContinuityListRequest& request,
        const Domain::OperationContext& context) noexcept override;

    [[nodiscard]] Domain::Result<Domain::LegacyContinuityStatusSummary>
    statusSummary(
        const Domain::OperationContext& context) noexcept override;

    [[nodiscard]] Domain::Result<Domain::LegacyContinuityProjectionRepairOutcome>
    repairProjections(
        const Domain::OperationContext& context) noexcept override;

    [[nodiscard]] Domain::Result<Domain::LegacyContinuityResetOutcome> reset(
        const Domain::DestructiveConfirmation& confirmation,
        const Domain::OperationContext& context) noexcept override;

    void shutdown() noexcept override;

private:
    class Impl;
    std::unique_ptr<Impl> implementation_;
};

} // namespace ForgeConductor::Application
