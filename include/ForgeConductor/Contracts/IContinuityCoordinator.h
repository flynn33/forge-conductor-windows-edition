#pragma once

#include "ForgeConductor/Domain/Domain.h"

#include <memory>
#include <optional>
#include <string>

namespace ForgeConductor::Contracts {

class IContinuityRepository {
public:
    virtual ~IContinuityRepository() = default;

    [[nodiscard]] virtual const Domain::ProjectId& projectId() const noexcept = 0;

    [[nodiscard]] virtual Domain::Result<Domain::ContinuityOperation>
    createOperation(
        const Domain::ContinuityHandoff& handoff,
        const Domain::IdempotencyKey& idempotencyKey,
        const Domain::OperationContext& context) noexcept = 0;

    [[nodiscard]] virtual Domain::Result<void> storeHandoff(
        const Domain::ContinuityHandoff& handoff,
        const Domain::OperationContext& context) noexcept = 0;

    [[nodiscard]] virtual Domain::Result<std::optional<Domain::ContinuityHandoff>> handoff(
        const Domain::ProjectId& projectId,
        const Domain::ContinuityHandoffId& handoffId,
        const Domain::OperationContext& context) noexcept = 0;

    [[nodiscard]] virtual Domain::Result<std::optional<Domain::ContinuityOperation>>
    operation(
        const Domain::ProjectId& projectId,
        const Domain::ContinuityOperationId& operationId,
        const Domain::OperationContext& context) noexcept = 0;

    [[nodiscard]] virtual Domain::Result<std::optional<Domain::ContinuityOperation>>
    activeOperation(
        const Domain::ProjectId& projectId,
        const Domain::OperationContext& context) noexcept = 0;

    [[nodiscard]] virtual Domain::Result<Domain::ContinuityOperation> compareAndSet(
        const Domain::ContinuityOperationId& operationId,
        Domain::ContinuityState expected,
        Domain::ContinuityState next,
        std::optional<Domain::SessionId> successorSessionId,
        std::optional<std::string> evidence,
        const Domain::OperationContext& context) noexcept = 0;

    [[nodiscard]] virtual Domain::Result<Domain::ContinuityOperation> acknowledge(
        const Domain::ContinuityOperationId& operationId,
        const Domain::HandoffAcknowledgement& acknowledgement,
        const Domain::OperationContext& context) noexcept = 0;

    [[nodiscard]] virtual Domain::Result<Domain::ContinuityOperation> recordRetry(
        const Domain::ContinuityOperationId& operationId,
        Domain::ContinuityState resumeState,
        std::string error,
        std::optional<Domain::UtcTimePoint> retryAt,
        const Domain::OperationContext& context) noexcept = 0;

    [[nodiscard]] virtual Domain::Result<std::optional<Domain::SessionId>> activeSession(
        const Domain::ProjectId& projectId,
        const Domain::OperationContext& context) noexcept = 0;

    [[nodiscard]] virtual Domain::Result<std::size_t> transitionCount(
        const Domain::ContinuityOperationId& operationId,
        const Domain::OperationContext& context) noexcept = 0;

    [[nodiscard]] virtual Domain::Result<Domain::ContinuityStatus> status(
        const Domain::ProjectId& projectId,
        const Domain::OperationContext& context) noexcept = 0;

    [[nodiscard]] virtual Domain::Result<Domain::ContinuityResetReport> resetContinuity(
        const Domain::ContinuityResetRequest& request,
        const Domain::OperationContext& context) noexcept = 0;

    virtual void close() noexcept = 0;
};

class IContinuityRepositoryFactory {
public:
    virtual ~IContinuityRepositoryFactory() = default;

    [[nodiscard]] virtual Domain::Result<std::shared_ptr<IContinuityRepository>>
    openContinuity(
        const Domain::ProjectId& projectId,
        const Domain::OperationContext& context) noexcept = 0;

    [[nodiscard]] virtual Domain::Result<void> close(
        const Domain::ProjectId& projectId,
        const Domain::OperationContext& context) noexcept = 0;

    [[nodiscard]] virtual std::size_t openCount() const noexcept = 0;

    virtual void shutdown() noexcept = 0;
};

class IContinuityCoordinator {
public:
    virtual ~IContinuityCoordinator() = default;

    [[nodiscard]] virtual Domain::Result<Domain::CheckpointOutcome> checkpoint(
        const Domain::CheckpointRequest& request,
        const Domain::OperationContext& context) noexcept = 0;

    [[nodiscard]] virtual Domain::Result<Domain::CheckpointOutcome> prepareHandoff(
        const Domain::CheckpointRequest& request,
        const Domain::OperationContext& context) noexcept = 0;

    [[nodiscard]] virtual Domain::Result<std::optional<Domain::ContinuityHandoff>>
    getPendingHandoff(
        const Domain::ProjectId& projectId,
        const Domain::OperationContext& context) noexcept = 0;

    [[nodiscard]] virtual Domain::Result<Domain::ContinuityOperation> acknowledgeHandoff(
        const Domain::ProjectId& projectId,
        const Domain::ContinuityOperationId& operationId,
        const Domain::HandoffAcknowledgement& acknowledgement,
        const Domain::OperationContext& context) noexcept = 0;

    [[nodiscard]] virtual Domain::Result<Domain::HandoffResumeOutcome> resume(
        const Domain::HandoffResumeRequest& request,
        const Domain::OperationContext& context) noexcept = 0;

    [[nodiscard]] virtual Domain::Result<Domain::ContinuityStatus> status(
        const Domain::ProjectId& projectId,
        const Domain::OperationContext& context) noexcept = 0;

    [[nodiscard]] virtual Domain::Result<Domain::RolloverOutcome> requestRollover(
        const Domain::RolloverRequest& request,
        const Domain::OperationContext& context) noexcept = 0;

    [[nodiscard]] virtual Domain::Result<Domain::ContinuityRecoveryReport>
    recoverIncompleteOperations(
        const Domain::ContinuityRecoveryRequest& request,
        const Domain::OperationContext& context) noexcept = 0;

    [[nodiscard]] virtual Domain::Result<Domain::ContinuityResetReport>
    resetProjectContinuity(
        const Domain::ContinuityResetRequest& request,
        const Domain::OperationContext& context) noexcept = 0;

    virtual void cancel(const Domain::OperationId& operationId) noexcept = 0;
    virtual void shutdown() noexcept = 0;
};

} // namespace ForgeConductor::Contracts
