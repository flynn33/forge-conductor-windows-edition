#include "ForgeConductor/Application/ContinuityCoordinator.h"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace ForgeConductor::Application {
namespace {

constexpr std::size_t MaximumRecoveryProjects = 128U;
constexpr std::size_t MaximumStateMachineSteps = 32U;
constexpr auto RetryDelay = std::chrono::seconds{1};

template <typename T, typename U>
[[nodiscard]] Domain::Result<T> propagate(Domain::Result<U>&& source)
{
    return Domain::Result<T>::failure(std::move(source).error());
}

template <typename T>
[[nodiscard]] Domain::Result<T> failure(
    const std::string_view code,
    std::string message,
    const bool retryable = false)
{
    return Domain::Result<T>::failure(
        Domain::makeError(code, std::move(message), retryable));
}

[[nodiscard]] Domain::Result<void> validateContext(
    const Domain::OperationContext& context,
    Contracts::IClock& clock,
    const std::atomic_bool& shutdownRequested) noexcept
{
    try {
        if (shutdownRequested.load(std::memory_order_acquire)) {
            return Domain::Result<void>::failure(Domain::makeError(
                Domain::ErrorCodes::TransportClosed,
                "The continuity coordinator is shutting down."));
        }
        if (context.isCancellationRequested()) {
            return Domain::Result<void>::failure(Domain::makeError(
                Domain::ErrorCodes::Cancelled,
                "The continuity operation was cancelled."));
        }
        if (context.isExpired(clock.monotonicNow())) {
            return Domain::Result<void>::failure(Domain::makeError(
                Domain::ErrorCodes::DeadlineExceeded,
                "The continuity operation exceeded its deadline."));
        }
        return Domain::Result<void>::success();
    } catch (...) {
        return Domain::Result<void>::failure(Domain::makeError(
            Domain::ErrorCodes::InternalFailure,
            "The continuity operation context could not be validated."));
    }
}

[[nodiscard]] bool hostCanReconcile(const Domain::HostCapabilities& capabilities) noexcept
{
    return capabilities.create && capabilities.bootstrap &&
           (capabilities.idempotency || capabilities.queryByIdempotencyKey);
}

} // namespace

class ContinuityCoordinator::Impl final {
public:
    Impl(
        Contracts::IProjectRegistryRepository& projectRegistry,
        Contracts::IContinuityRepositoryFactory& repositoryFactory,
        Contracts::ISessionHostAdapter& hostAdapter,
        Contracts::IClock& clock) noexcept
        : projectRegistry_{projectRegistry},
          repositoryFactory_{repositoryFactory},
          hostAdapter_{hostAdapter},
          clock_{clock}
    {
    }

    [[nodiscard]] Domain::Result<Domain::CheckpointOutcome> checkpoint(
        const Domain::CheckpointRequest& request,
        const Domain::OperationContext& context) noexcept
    {
        auto valid = validateContext(context, clock_, shutdownRequested_);
        if (!valid) {
            return propagate<Domain::CheckpointOutcome>(std::move(valid));
        }
        auto key = request.idempotencyKey
            ? Domain::Result<Domain::IdempotencyKey>::success(
                  *request.idempotencyKey)
            : Domain::IdempotencyKey::create(
                  request.handoff.operationId.value());
        if (!key) {
            return propagate<Domain::CheckpointOutcome>(std::move(key));
        }
        auto opened = repositoryFactory_.openContinuity(
            request.handoff.project.projectId, context);
        if (!opened) {
            return propagate<Domain::CheckpointOutcome>(std::move(opened));
        }
        auto repository = std::move(opened).value();
        auto created = repository->createOperation(
            request.handoff, key.value(), context);
        if (!created) {
            return propagate<Domain::CheckpointOutcome>(std::move(created));
        }
        auto operation = std::move(created).value();

        if (operation.state == Domain::ContinuityState::RetryWait) {
            if (operation.retryResumeState !=
                    Domain::ContinuityState::CheckpointPreparing ||
                !operation.retryAt || *operation.retryAt > clock_.utcNow()) {
                return failure<Domain::CheckpointOutcome>(
                    Domain::ErrorCodes::DatabaseBusy,
                    "The checkpoint is waiting for its durable retry time.",
                    true);
            }
            auto resumed = repository->compareAndSet(
                operation.operationId,
                Domain::ContinuityState::RetryWait,
                Domain::ContinuityState::CheckpointPreparing,
                std::nullopt,
                std::optional<std::string>{"checkpoint_retry_resumed"},
                context);
            if (!resumed) {
                return propagate<Domain::CheckpointOutcome>(std::move(resumed));
            }
            operation = std::move(resumed).value();
        }
        if (operation.state == Domain::ContinuityState::Idle) {
            auto stored = repository->storeHandoff(request.handoff, context);
            if (!stored) {
                return propagate<Domain::CheckpointOutcome>(std::move(stored));
            }
            auto intent = repository->compareAndSet(
                operation.operationId,
                Domain::ContinuityState::Idle,
                Domain::ContinuityState::CheckpointPreparing,
                std::nullopt,
                std::optional<std::string>{"checkpoint_intent"},
                context);
            if (!intent) {
                return propagate<Domain::CheckpointOutcome>(std::move(intent));
            }
            operation = std::move(intent).value();
        }
        if (operation.state == Domain::ContinuityState::CheckpointPreparing) {
            auto stored = repository->storeHandoff(request.handoff, context);
            if (!stored) {
                static_cast<void>(repository->recordRetry(
                    operation.operationId,
                    Domain::ContinuityState::CheckpointPreparing,
                    stored.error().message,
                    clock_.utcNow() + RetryDelay,
                    context));
                return propagate<Domain::CheckpointOutcome>(std::move(stored));
            }
            auto persisted = repository->compareAndSet(
                operation.operationId,
                Domain::ContinuityState::CheckpointPreparing,
                Domain::ContinuityState::CheckpointPersisted,
                std::nullopt,
                std::optional<std::string>{"canonical_handoff_persisted"},
                context);
            if (!persisted) {
                return propagate<Domain::CheckpointOutcome>(std::move(persisted));
            }
            operation = std::move(persisted).value();
        }
        auto durable = repository->handoff(
            operation.projectId, operation.handoffId, context);
        if (!durable) {
            return propagate<Domain::CheckpointOutcome>(std::move(durable));
        }
        if (!durable.value()) {
            return failure<Domain::CheckpointOutcome>(
                Domain::ErrorCodes::IntegrityFailure,
                "The checkpoint operation has no durable canonical handoff.");
        }
        return Domain::Result<Domain::CheckpointOutcome>::success(
            Domain::CheckpointOutcome{operation, *std::move(durable).value()});
    }

    [[nodiscard]] Domain::Result<std::optional<Domain::ContinuityHandoff>>
    pending(
        const Domain::ProjectId& projectId,
        const Domain::OperationContext& context) noexcept
    {
        auto valid = validateContext(context, clock_, shutdownRequested_);
        if (!valid) {
            return propagate<std::optional<Domain::ContinuityHandoff>>(
                std::move(valid));
        }
        auto opened = repositoryFactory_.openContinuity(projectId, context);
        if (!opened) {
            return propagate<std::optional<Domain::ContinuityHandoff>>(
                std::move(opened));
        }
        auto repository = std::move(opened).value();
        auto active = repository->activeOperation(projectId, context);
        if (!active) {
            return propagate<std::optional<Domain::ContinuityHandoff>>(
                std::move(active));
        }
        if (!active.value()) {
            return Domain::Result<
                std::optional<Domain::ContinuityHandoff>>::success(std::nullopt);
        }
        return repository->handoff(
            projectId, active.value()->handoffId, context);
    }

    [[nodiscard]] Domain::Result<Domain::RolloverOutcome> rollover(
        const Domain::RolloverRequest& request,
        const Domain::OperationContext& context) noexcept
    {
        auto valid = validateContext(context, clock_, shutdownRequested_);
        if (!valid) {
            return propagate<Domain::RolloverOutcome>(std::move(valid));
        }
        auto capabilitiesResult = hostAdapter_.capabilities(context);
        if (!capabilitiesResult) {
            return propagate<Domain::RolloverOutcome>(
                std::move(capabilitiesResult));
        }
        const auto capabilities = std::move(capabilitiesResult).value();
        if (!hostCanReconcile(capabilities)) {
            return failure<Domain::RolloverOutcome>(
                Domain::ErrorCodes::HostCapabilityUnavailable,
                "The session host cannot idempotently create and bootstrap a successor.");
        }
        auto opened = repositoryFactory_.openContinuity(
            request.projectId, context);
        if (!opened) {
            return propagate<Domain::RolloverOutcome>(std::move(opened));
        }
        auto repository = std::move(opened).value();
        auto operationResult = repository->operation(
            request.projectId, request.operationId, context);
        if (!operationResult) {
            return propagate<Domain::RolloverOutcome>(
                std::move(operationResult));
        }
        if (!operationResult.value()) {
            return failure<Domain::RolloverOutcome>(
                Domain::ErrorCodes::RecordNotFound,
                "The requested continuity operation was not found.");
        }
        auto operation = *std::move(operationResult).value();
        std::optional<Domain::HostSession> successor;

        for (std::size_t step = 0U; step < MaximumStateMachineSteps; ++step) {
            valid = validateContext(context, clock_, shutdownRequested_);
            if (!valid) {
                return propagate<Domain::RolloverOutcome>(std::move(valid));
            }
            switch (operation.state) {
            case Domain::ContinuityState::Idle:
            case Domain::ContinuityState::CheckpointPreparing: {
                auto durable = repository->handoff(
                    request.projectId, operation.handoffId, context);
                if (!durable) {
                    return propagate<Domain::RolloverOutcome>(std::move(durable));
                }
                if (!durable.value()) {
                    return failure<Domain::RolloverOutcome>(
                        Domain::ErrorCodes::IntegrityFailure,
                        "The rollover has no durable checkpoint handoff.");
                }
                auto prepared = checkpoint(
                    Domain::CheckpointRequest{
                        *std::move(durable).value(),
                        operation.idempotencyKey},
                    context);
                if (!prepared) {
                    return propagate<Domain::RolloverOutcome>(std::move(prepared));
                }
                operation = std::move(prepared).value().operation;
                break;
            }
            case Domain::ContinuityState::CheckpointPersisted: {
                auto transitioned = repository->compareAndSet(
                    operation.operationId,
                    Domain::ContinuityState::CheckpointPersisted,
                    Domain::ContinuityState::SuccessorCreating,
                    std::nullopt,
                    std::optional<std::string>{"host_create_intent"},
                    context);
                if (!transitioned) {
                    return propagate<Domain::RolloverOutcome>(
                        std::move(transitioned));
                }
                operation = std::move(transitioned).value();
                break;
            }
            case Domain::ContinuityState::SuccessorCreating: {
                auto reconciled = reconcileSession(
                    operation, capabilities, context);
                if (!reconciled) {
                    recordRetryBestEffort(
                        *repository, operation, reconciled.error(), context);
                    return propagate<Domain::RolloverOutcome>(
                        std::move(reconciled));
                }
                successor = std::move(reconciled).value();
                auto transitioned = repository->compareAndSet(
                    operation.operationId,
                    Domain::ContinuityState::SuccessorCreating,
                    Domain::ContinuityState::SuccessorCreated,
                    successor->id,
                    std::optional<std::string>{"host_successor_reconciled"},
                    context);
                if (!transitioned) {
                    return propagate<Domain::RolloverOutcome>(
                        std::move(transitioned));
                }
                operation = std::move(transitioned).value();
                break;
            }
            case Domain::ContinuityState::SuccessorCreated: {
                auto transitioned = repository->compareAndSet(
                    operation.operationId,
                    Domain::ContinuityState::SuccessorCreated,
                    Domain::ContinuityState::BootstrapSending,
                    operation.successorSessionId,
                    std::optional<std::string>{"bootstrap_intent"},
                    context);
                if (!transitioned) {
                    return propagate<Domain::RolloverOutcome>(
                        std::move(transitioned));
                }
                operation = std::move(transitioned).value();
                break;
            }
            case Domain::ContinuityState::BootstrapSending: {
                auto reconciled = reconcileSession(
                    operation, capabilities, context);
                if (!reconciled) {
                    recordRetryBestEffort(
                        *repository, operation, reconciled.error(), context);
                    return propagate<Domain::RolloverOutcome>(
                        std::move(reconciled));
                }
                successor = std::move(reconciled).value();
                if (!operation.successorSessionId ||
                    successor->id != *operation.successorSessionId) {
                    return failure<Domain::RolloverOutcome>(
                        Domain::ErrorCodes::IntegrityFailure,
                        "The reconciled host successor differs from the durable successor.");
                }
                auto durable = repository->handoff(
                    request.projectId, operation.handoffId, context);
                if (!durable) {
                    return propagate<Domain::RolloverOutcome>(std::move(durable));
                }
                if (!durable.value()) {
                    return failure<Domain::RolloverOutcome>(
                        Domain::ErrorCodes::IntegrityFailure,
                        "The bootstrap intent has no durable canonical handoff.");
                }
                auto handoff = *std::move(durable).value();
                auto compatible = Domain::validateBootstrapCompatibility(
                    *successor, handoff);
                if (!compatible) {
                    return propagate<Domain::RolloverOutcome>(
                        std::move(compatible));
                }
                auto bootstrapped = hostAdapter_.bootstrap(
                    *successor, handoff, context);
                if (!bootstrapped) {
                    recordRetryBestEffort(
                        *repository, operation, bootstrapped.error(), context);
                    return propagate<Domain::RolloverOutcome>(
                        std::move(bootstrapped));
                }
                auto acknowledgement = hostAdapter_.awaitAcknowledgement(
                    *successor,
                    handoff.handoffId,
                    handoff.contentSha256,
                    context);
                if (!acknowledgement) {
                    recordRetryBestEffort(
                        *repository, operation, acknowledgement.error(), context);
                    return propagate<Domain::RolloverOutcome>(
                        std::move(acknowledgement));
                }
                auto acknowledged = repository->acknowledge(
                    operation.operationId,
                    acknowledgement.value(),
                    context);
                if (!acknowledged) {
                    return propagate<Domain::RolloverOutcome>(
                        std::move(acknowledged));
                }
                operation = std::move(acknowledged).value();
                break;
            }
            case Domain::ContinuityState::Acknowledged: {
                auto transitioned = repository->compareAndSet(
                    operation.operationId,
                    Domain::ContinuityState::Acknowledged,
                    Domain::ContinuityState::PredecessorSealing,
                    operation.successorSessionId,
                    std::optional<std::string>{"predecessor_seal_intent"},
                    context);
                if (!transitioned) {
                    return propagate<Domain::RolloverOutcome>(
                        std::move(transitioned));
                }
                operation = std::move(transitioned).value();
                break;
            }
            case Domain::ContinuityState::PredecessorSealing: {
                auto transitioned = repository->compareAndSet(
                    operation.operationId,
                    Domain::ContinuityState::PredecessorSealing,
                    Domain::ContinuityState::Completed,
                    operation.successorSessionId,
                    std::optional<std::string>{"active_session_pointer_swapped"},
                    context);
                if (!transitioned) {
                    recordRetryBestEffort(
                        *repository, operation, transitioned.error(), context);
                    return propagate<Domain::RolloverOutcome>(
                        std::move(transitioned));
                }
                operation = std::move(transitioned).value();
                break;
            }
            case Domain::ContinuityState::RetryWait: {
                if (!operation.retryResumeState || !operation.retryAt) {
                    return failure<Domain::RolloverOutcome>(
                        Domain::ErrorCodes::IntegrityFailure,
                        "The retry operation is missing its exact resume state or time.");
                }
                if (*operation.retryAt > clock_.utcNow()) {
                    return failure<Domain::RolloverOutcome>(
                        Domain::ErrorCodes::DatabaseBusy,
                        "The rollover is waiting for its durable retry time.",
                        true);
                }
                auto resumed = repository->compareAndSet(
                    operation.operationId,
                    Domain::ContinuityState::RetryWait,
                    *operation.retryResumeState,
                    operation.successorSessionId,
                    std::optional<std::string>{"retry_resumed"},
                    context);
                if (!resumed) {
                    return propagate<Domain::RolloverOutcome>(std::move(resumed));
                }
                operation = std::move(resumed).value();
                break;
            }
            case Domain::ContinuityState::Cancelling: {
                auto cancelled = repository->compareAndSet(
                    operation.operationId,
                    Domain::ContinuityState::Cancelling,
                    Domain::ContinuityState::Cancelled,
                    operation.successorSessionId,
                    std::optional<std::string>{"cancellation_completed"},
                    context);
                if (!cancelled) {
                    return propagate<Domain::RolloverOutcome>(
                        std::move(cancelled));
                }
                operation = std::move(cancelled).value();
                break;
            }
            case Domain::ContinuityState::Completed:
            case Domain::ContinuityState::Cancelled:
                return Domain::Result<Domain::RolloverOutcome>::success(
                    Domain::RolloverOutcome{
                        operation,
                        std::move(successor),
                        operation.state == Domain::ContinuityState::Completed &&
                            operation.acknowledgedSessionId.has_value(),
                        operation.state == Domain::ContinuityState::Completed});
            case Domain::ContinuityState::FailedRecoverable:
                return failure<Domain::RolloverOutcome>(
                    Domain::ErrorCodes::IntegrityFailure,
                    "A recoverable failure was not atomically advanced to retry wait.");
            }
        }
        return failure<Domain::RolloverOutcome>(
            Domain::ErrorCodes::LimitExceeded,
            "The bounded continuity state-machine step limit was reached.");
    }

    [[nodiscard]] Domain::Result<Domain::HandoffResumeOutcome> resume(
        const Domain::HandoffResumeRequest& request,
        const Domain::OperationContext& context) noexcept
    {
        auto valid = validateContext(context, clock_, shutdownRequested_);
        if (!valid) {
            return propagate<Domain::HandoffResumeOutcome>(std::move(valid));
        }
        auto opened = repositoryFactory_.openContinuity(
            request.projectId, context);
        if (!opened) {
            return propagate<Domain::HandoffResumeOutcome>(std::move(opened));
        }
        auto repository = std::move(opened).value();
        auto durable = repository->handoff(
            request.projectId, request.handoffId, context);
        if (!durable) {
            return propagate<Domain::HandoffResumeOutcome>(std::move(durable));
        }
        if (!durable.value()) {
            return failure<Domain::HandoffResumeOutcome>(
                Domain::ErrorCodes::RecordNotFound,
                "The requested continuity handoff was not found.");
        }
        auto operationResult = repository->operation(
            request.projectId, durable.value()->operationId, context);
        if (!operationResult) {
            return propagate<Domain::HandoffResumeOutcome>(
                std::move(operationResult));
        }
        if (!operationResult.value()) {
            return failure<Domain::HandoffResumeOutcome>(
                Domain::ErrorCodes::IntegrityFailure,
                "The requested handoff has no durable continuity operation.");
        }
        auto operation = *std::move(operationResult).value();
        if (operation.handoffId != request.handoffId ||
            operation.successorSessionId != request.successorSessionId ||
            operation.acknowledgedSessionId != request.successorSessionId) {
            return failure<Domain::HandoffResumeOutcome>(
                Domain::ErrorCodes::Conflict,
                "The resume request does not match the acknowledged successor chain.");
        }
        if (operation.state != Domain::ContinuityState::Acknowledged &&
            operation.state != Domain::ContinuityState::PredecessorSealing &&
            operation.state != Domain::ContinuityState::Completed) {
            return failure<Domain::HandoffResumeOutcome>(
                Domain::ErrorCodes::Conflict,
                "The continuity operation is not acknowledged for resume.");
        }
        if (operation.adapterId != hostAdapter_.identifier()) {
            return failure<Domain::HandoffResumeOutcome>(
                Domain::ErrorCodes::IntegrityFailure,
                "The acknowledged continuity operation belongs to another session-host adapter.");
        }

        auto capabilitiesResult = hostAdapter_.capabilities(context);
        if (!capabilitiesResult) {
            return propagate<Domain::HandoffResumeOutcome>(
                std::move(capabilitiesResult));
        }
        const auto capabilities = std::move(capabilitiesResult).value();
        if (!capabilities.resume ||
            (!capabilities.queryByIdempotencyKey &&
             !(capabilities.create && capabilities.idempotency))) {
            return failure<Domain::HandoffResumeOutcome>(
                Domain::ErrorCodes::HostCapabilityUnavailable,
                "The session host cannot safely reconcile an acknowledged successor for resume.");
        }

        auto sessionResult = reconcileSession(operation, capabilities, context);
        if (!sessionResult) {
            return propagate<Domain::HandoffResumeOutcome>(
                std::move(sessionResult));
        }
        auto session = std::move(sessionResult).value();
        if (session.id != request.successorSessionId) {
            return failure<Domain::HandoffResumeOutcome>(
                Domain::ErrorCodes::IntegrityFailure,
                "The resumed host session does not match the durable successor chain.");
        }
        valid = Domain::validateBootstrapCompatibility(
            session, *durable.value());
        if (!valid) {
            return propagate<Domain::HandoffResumeOutcome>(std::move(valid));
        }
        valid = Domain::validateHandoffAcknowledgement(
            operation,
            *durable.value(),
            Domain::HandoffAcknowledgement{
                request.handoffId,
                session.id,
                hostAdapter_.identifier(),
                durable.value()->contentSha256});
        if (!valid) {
            return propagate<Domain::HandoffResumeOutcome>(std::move(valid));
        }

        // Resume is also the host-readiness proof. A native adapter may have
        // reconstructed its durable ledger after the provider transport was
        // restarted, so replay the exact canonical bootstrap through the
        // adapter before publishing or reaffirming the active-session pointer.
        // The adapter owns effect idempotency and must return the same exact
        // acknowledgement binding.
        auto bootstrapped = hostAdapter_.bootstrap(
            session, *durable.value(), context);
        if (!bootstrapped) {
            return propagate<Domain::HandoffResumeOutcome>(
                std::move(bootstrapped));
        }
        auto acknowledgement = hostAdapter_.awaitAcknowledgement(
            session,
            durable.value()->handoffId,
            durable.value()->contentSha256,
            context);
        if (!acknowledgement) {
            return propagate<Domain::HandoffResumeOutcome>(
                std::move(acknowledgement));
        }
        valid = Domain::validateHandoffAcknowledgement(
            operation,
            *durable.value(),
            acknowledgement.value());
        if (!valid) {
            return propagate<Domain::HandoffResumeOutcome>(std::move(valid));
        }

        // Host reconciliation and the complete durable binding are proven
        // before either transition can seal the predecessor or publish the
        // active-session pointer.
        if (operation.state == Domain::ContinuityState::Acknowledged) {
            auto sealing = repository->compareAndSet(
                operation.operationId,
                Domain::ContinuityState::Acknowledged,
                Domain::ContinuityState::PredecessorSealing,
                operation.successorSessionId,
                std::optional<std::string>{"resume_predecessor_seal_intent"},
                context);
            if (!sealing) {
                return propagate<Domain::HandoffResumeOutcome>(
                    std::move(sealing));
            }
            operation = std::move(sealing).value();
        }
        if (operation.state == Domain::ContinuityState::PredecessorSealing) {
            auto completed = repository->compareAndSet(
                operation.operationId,
                Domain::ContinuityState::PredecessorSealing,
                Domain::ContinuityState::Completed,
                operation.successorSessionId,
                std::optional<std::string>{"resume_active_session_pointer_swapped"},
                context);
            if (!completed) {
                return propagate<Domain::HandoffResumeOutcome>(
                    std::move(completed));
            }
            operation = std::move(completed).value();
        }
        return Domain::Result<Domain::HandoffResumeOutcome>::success(
            Domain::HandoffResumeOutcome{
                operation,
                *std::move(durable).value(),
                std::move(session)});
    }

    [[nodiscard]] Domain::Result<Domain::ContinuityRecoveryReport> recover(
        const Domain::ContinuityRecoveryRequest& request,
        const Domain::OperationContext& context) noexcept
    {
        auto valid = validateContext(context, clock_, shutdownRequested_);
        if (!valid) {
            return propagate<Domain::ContinuityRecoveryReport>(std::move(valid));
        }
        std::vector<Domain::ProjectId> projects;
        if (request.projectId) {
            projects.push_back(*request.projectId);
        } else {
            auto descriptors = projectRegistry_.list(MaximumRecoveryProjects, context);
            if (!descriptors) {
                return propagate<Domain::ContinuityRecoveryReport>(
                    std::move(descriptors));
            }
            projects.reserve(descriptors.value().size());
            for (const auto& descriptor : descriptors.value()) {
                projects.push_back(descriptor.id);
            }
        }
        Domain::ContinuityRecoveryReport report{};
        report.operations.reserve(projects.size());
        for (const auto& projectId : projects) {
            valid = validateContext(context, clock_, shutdownRequested_);
            if (!valid) {
                return propagate<Domain::ContinuityRecoveryReport>(
                    std::move(valid));
            }
            auto opened = repositoryFactory_.openContinuity(projectId, context);
            if (!opened) {
                ++report.failed;
                continue;
            }
            auto repository = std::move(opened).value();
            auto active = repository->activeOperation(projectId, context);
            if (!active) {
                ++report.failed;
                continue;
            }
            if (!active.value()) {
                continue;
            }
            ++report.inspected;
            auto operation = *std::move(active).value();
            if (!request.resumeOperations) {
                if (operation.state != Domain::ContinuityState::Cancelling) {
                    auto cancelling = repository->compareAndSet(
                        operation.operationId,
                        operation.state,
                        Domain::ContinuityState::Cancelling,
                        operation.successorSessionId,
                        std::optional<std::string>{"recovery_cancel_intent"},
                        context);
                    if (!cancelling) {
                        ++report.failed;
                        continue;
                    }
                    operation = std::move(cancelling).value();
                }
                auto cancelled = repository->compareAndSet(
                    operation.operationId,
                    Domain::ContinuityState::Cancelling,
                    Domain::ContinuityState::Cancelled,
                    operation.successorSessionId,
                    std::optional<std::string>{"recovery_cancelled"},
                    context);
                if (!cancelled) {
                    ++report.failed;
                    continue;
                }
                operation = std::move(cancelled).value();
                ++report.cancelled;
                report.operations.push_back(operation);
                continue;
            }
            auto outcome = rollover(
                Domain::RolloverRequest{projectId, operation.operationId},
                context);
            if (!outcome) {
                ++report.failed;
                continue;
            }
            ++report.resumed;
            report.operations.push_back(std::move(outcome).value().operation);
        }
        return Domain::Result<Domain::ContinuityRecoveryReport>::success(
            std::move(report));
    }

    [[nodiscard]] Domain::Result<Domain::HostSession> reconcileSession(
        const Domain::ContinuityOperation& operation,
        const Domain::HostCapabilities& capabilities,
        const Domain::OperationContext& context) noexcept
    {
        Domain::SessionCreationRequest request{
            operation.operationId,
            operation.projectId,
            operation.predecessorSessionId,
            operation.idempotencyKey};
        std::optional<Domain::HostSession> session;
        if (capabilities.queryByIdempotencyKey) {
            auto queried = hostAdapter_.queryByIdempotencyKey(
                operation.projectId, operation.idempotencyKey, context);
            if (!queried) {
                return propagate<Domain::HostSession>(std::move(queried));
            }
            session = std::move(queried).value();
        }
        if (!session) {
            if (!capabilities.create) {
                return failure<Domain::HostSession>(
                    Domain::ErrorCodes::HostCapabilityUnavailable,
                    "The session host reported no durable successor and cannot create one.");
            }
            if (!capabilities.queryByIdempotencyKey &&
                !capabilities.idempotency) {
                return failure<Domain::HostSession>(
                    Domain::ErrorCodes::HostCapabilityUnavailable,
                    "The session host cannot safely create or reconcile the durable successor.");
            }
            auto created = hostAdapter_.createSession(request, context);
            if (!created) {
                return created;
            }
            session = std::move(created).value();
        }
        auto valid = Domain::validateHostSessionBinding(*session, request);
        if (!valid) {
            return propagate<Domain::HostSession>(std::move(valid));
        }
        return Domain::Result<Domain::HostSession>::success(*std::move(session));
    }

    void recordRetryBestEffort(
        Contracts::IContinuityRepository& repository,
        const Domain::ContinuityOperation& operation,
        const Domain::Error& error,
        const Domain::OperationContext& context) noexcept
    {
        if (!Domain::isRetryResumeState(operation.state) ||
            context.isCancellationRequested() ||
            context.isExpired(clock_.monotonicNow())) {
            return;
        }
        static_cast<void>(repository.recordRetry(
            operation.operationId,
            operation.state,
            error.message,
            clock_.utcNow() + RetryDelay,
            context));
    }

    Contracts::IProjectRegistryRepository& projectRegistry_;
    Contracts::IContinuityRepositoryFactory& repositoryFactory_;
    Contracts::ISessionHostAdapter& hostAdapter_;
    Contracts::IClock& clock_;
    std::atomic_bool shutdownRequested_{};
};

ContinuityCoordinator::ContinuityCoordinator(
    Contracts::IProjectRegistryRepository& projectRegistry,
    Contracts::IContinuityRepositoryFactory& repositoryFactory,
    Contracts::ISessionHostAdapter& hostAdapter,
    Contracts::IClock& clock)
    : implementation_{std::make_unique<Impl>(
          projectRegistry, repositoryFactory, hostAdapter, clock)}
{
}

ContinuityCoordinator::~ContinuityCoordinator() noexcept
{
    shutdown();
}

Domain::Result<Domain::CheckpointOutcome> ContinuityCoordinator::checkpoint(
    const Domain::CheckpointRequest& request,
    const Domain::OperationContext& context) noexcept
{
    if (!implementation_) {
        return failure<Domain::CheckpointOutcome>(
            Domain::ErrorCodes::TransportClosed,
            "The continuity coordinator has no implementation.");
    }
    return implementation_->checkpoint(request, context);
}

Domain::Result<Domain::CheckpointOutcome> ContinuityCoordinator::prepareHandoff(
    const Domain::CheckpointRequest& request,
    const Domain::OperationContext& context) noexcept
{
    return checkpoint(request, context);
}

Domain::Result<std::optional<Domain::ContinuityHandoff>>
ContinuityCoordinator::getPendingHandoff(
    const Domain::ProjectId& projectId,
    const Domain::OperationContext& context) noexcept
{
    if (!implementation_) {
        return failure<std::optional<Domain::ContinuityHandoff>>(
            Domain::ErrorCodes::TransportClosed,
            "The continuity coordinator has no implementation.");
    }
    return implementation_->pending(projectId, context);
}

Domain::Result<Domain::ContinuityOperation>
ContinuityCoordinator::acknowledgeHandoff(
    const Domain::ProjectId& projectId,
    const Domain::ContinuityOperationId& operationId,
    const Domain::HandoffAcknowledgement& acknowledgement,
    const Domain::OperationContext& context) noexcept
{
    if (!implementation_) {
        return failure<Domain::ContinuityOperation>(
            Domain::ErrorCodes::TransportClosed,
            "The continuity coordinator has no implementation.");
    }
    auto valid = validateContext(
        context,
        implementation_->clock_,
        implementation_->shutdownRequested_);
    if (!valid) {
        return propagate<Domain::ContinuityOperation>(std::move(valid));
    }
    auto opened = implementation_->repositoryFactory_.openContinuity(
        projectId, context);
    if (!opened) {
        return propagate<Domain::ContinuityOperation>(std::move(opened));
    }
    return opened.value()->acknowledge(operationId, acknowledgement, context);
}

Domain::Result<Domain::HandoffResumeOutcome> ContinuityCoordinator::resume(
    const Domain::HandoffResumeRequest& request,
    const Domain::OperationContext& context) noexcept
{
    if (!implementation_) {
        return failure<Domain::HandoffResumeOutcome>(
            Domain::ErrorCodes::TransportClosed,
            "The continuity coordinator has no implementation.");
    }
    return implementation_->resume(request, context);
}

Domain::Result<Domain::ContinuityStatus> ContinuityCoordinator::status(
    const Domain::ProjectId& projectId,
    const Domain::OperationContext& context) noexcept
{
    if (!implementation_) {
        return failure<Domain::ContinuityStatus>(
            Domain::ErrorCodes::TransportClosed,
            "The continuity coordinator has no implementation.");
    }
    auto valid = validateContext(
        context,
        implementation_->clock_,
        implementation_->shutdownRequested_);
    if (!valid) {
        return propagate<Domain::ContinuityStatus>(std::move(valid));
    }
    auto opened = implementation_->repositoryFactory_.openContinuity(
        projectId, context);
    if (!opened) {
        return propagate<Domain::ContinuityStatus>(std::move(opened));
    }
    return opened.value()->status(projectId, context);
}

Domain::Result<Domain::RolloverOutcome> ContinuityCoordinator::requestRollover(
    const Domain::RolloverRequest& request,
    const Domain::OperationContext& context) noexcept
{
    if (!implementation_) {
        return failure<Domain::RolloverOutcome>(
            Domain::ErrorCodes::TransportClosed,
            "The continuity coordinator has no implementation.");
    }
    return implementation_->rollover(request, context);
}

Domain::Result<Domain::ContinuityRecoveryReport>
ContinuityCoordinator::recoverIncompleteOperations(
    const Domain::ContinuityRecoveryRequest& request,
    const Domain::OperationContext& context) noexcept
{
    if (!implementation_) {
        return failure<Domain::ContinuityRecoveryReport>(
            Domain::ErrorCodes::TransportClosed,
            "The continuity coordinator has no implementation.");
    }
    return implementation_->recover(request, context);
}

Domain::Result<Domain::ContinuityResetReport>
ContinuityCoordinator::resetProjectContinuity(
    const Domain::ContinuityResetRequest& request,
    const Domain::OperationContext& context) noexcept
{
    if (!implementation_) {
        return failure<Domain::ContinuityResetReport>(
            Domain::ErrorCodes::TransportClosed,
            "The continuity coordinator has no implementation.");
    }
    auto valid = validateContext(
        context,
        implementation_->clock_,
        implementation_->shutdownRequested_);
    if (!valid) {
        return propagate<Domain::ContinuityResetReport>(std::move(valid));
    }
    auto opened = implementation_->repositoryFactory_.openContinuity(
        request.projectId, context);
    if (!opened) {
        return propagate<Domain::ContinuityResetReport>(std::move(opened));
    }
    return opened.value()->resetContinuity(request, context);
}

void ContinuityCoordinator::cancel(const Domain::OperationId& operationId) noexcept
{
    if (implementation_) {
        implementation_->hostAdapter_.cancel(operationId);
    }
}

void ContinuityCoordinator::shutdown() noexcept
{
    if (implementation_) {
        implementation_->shutdownRequested_.store(
            true, std::memory_order_release);
    }
}

} // namespace ForgeConductor::Application
