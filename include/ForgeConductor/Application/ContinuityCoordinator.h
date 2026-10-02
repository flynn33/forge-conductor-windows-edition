#pragma once

#include "ForgeConductor/Contracts/IContinuityCoordinator.h"
#include "ForgeConductor/Contracts/IFoundationServices.h"
#include "ForgeConductor/Contracts/IProjectMemoryService.h"
#include "ForgeConductor/Contracts/ISessionHostAdapter.h"

#include <memory>

namespace ForgeConductor::Application {

// Drives the durable project continuity ledger. The injected registry,
// repository factory, host adapter, and clock must outlive the coordinator.
// Host effects are never executed while a database transaction or cache lock
// is held; every effect is preceded by a committed intent state.
class ContinuityCoordinator final : public Contracts::IContinuityCoordinator {
public:
    ContinuityCoordinator(
        Contracts::IProjectRegistryRepository& projectRegistry,
        Contracts::IContinuityRepositoryFactory& repositoryFactory,
        Contracts::ISessionHostAdapter& hostAdapter,
        Contracts::IClock& clock);
    ~ContinuityCoordinator() noexcept override;

    ContinuityCoordinator(const ContinuityCoordinator&) = delete;
    ContinuityCoordinator& operator=(const ContinuityCoordinator&) = delete;
    ContinuityCoordinator(ContinuityCoordinator&&) = delete;
    ContinuityCoordinator& operator=(ContinuityCoordinator&&) = delete;

    [[nodiscard]] Domain::Result<Domain::CheckpointOutcome> checkpoint(
        const Domain::CheckpointRequest& request,
        const Domain::OperationContext& context) noexcept override;

    [[nodiscard]] Domain::Result<Domain::CheckpointOutcome> prepareHandoff(
        const Domain::CheckpointRequest& request,
        const Domain::OperationContext& context) noexcept override;

    [[nodiscard]] Domain::Result<std::optional<Domain::ContinuityHandoff>>
    getPendingHandoff(
        const Domain::ProjectId& projectId,
        const Domain::OperationContext& context) noexcept override;

    [[nodiscard]] Domain::Result<Domain::ContinuityOperation> acknowledgeHandoff(
        const Domain::ProjectId& projectId,
        const Domain::ContinuityOperationId& operationId,
        const Domain::HandoffAcknowledgement& acknowledgement,
        const Domain::OperationContext& context) noexcept override;

    [[nodiscard]] Domain::Result<Domain::HandoffResumeOutcome> resume(
        const Domain::HandoffResumeRequest& request,
        const Domain::OperationContext& context) noexcept override;

    [[nodiscard]] Domain::Result<Domain::ContinuityStatus> status(
        const Domain::ProjectId& projectId,
        const Domain::OperationContext& context) noexcept override;

    [[nodiscard]] Domain::Result<Domain::RolloverOutcome> requestRollover(
        const Domain::RolloverRequest& request,
        const Domain::OperationContext& context) noexcept override;

    [[nodiscard]] Domain::Result<Domain::ContinuityRecoveryReport>
    recoverIncompleteOperations(
        const Domain::ContinuityRecoveryRequest& request,
        const Domain::OperationContext& context) noexcept override;

    [[nodiscard]] Domain::Result<Domain::ContinuityResetReport>
    resetProjectContinuity(
        const Domain::ContinuityResetRequest& request,
        const Domain::OperationContext& context) noexcept override;

    void cancel(const Domain::OperationId& operationId) noexcept override;
    void shutdown() noexcept override;

private:
    class Impl;
    std::unique_ptr<Impl> implementation_;
};

} // namespace ForgeConductor::Application
