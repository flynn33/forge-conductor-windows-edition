#include "ForgeConductor/Application/AgentRepositoryManagedRunStore.h"

#include <utility>

namespace ForgeConductor::Application {
namespace {

[[nodiscard]] Domain::SessionStatus sessionStatus(
    const Domain::ManagedRunState state) noexcept
{
    switch (state) {
    case Domain::ManagedRunState::Running:
    case Domain::ManagedRunState::Cancelling:
        return Domain::SessionStatus::Running;
    case Domain::ManagedRunState::Completed:
        return Domain::SessionStatus::Completed;
    case Domain::ManagedRunState::Failed:
        return Domain::SessionStatus::Failed;
    case Domain::ManagedRunState::Cancelled:
        return Domain::SessionStatus::Closed;
    }
    return Domain::SessionStatus::Failed;
}

[[nodiscard]] Domain::ManagedRunState managedState(
    const Domain::SessionStatus state) noexcept
{
    switch (state) {
    case Domain::SessionStatus::Completed:
        return Domain::ManagedRunState::Completed;
    case Domain::SessionStatus::Failed:
        return Domain::ManagedRunState::Failed;
    case Domain::SessionStatus::Closed:
        return Domain::ManagedRunState::Cancelled;
    default:
        return Domain::ManagedRunState::Running;
    }
}

[[nodiscard]] std::optional<std::string> summary(
    const Domain::ManagedRunRecord& record)
{
    if (record.lastError) {
        return Domain::truncateAgentSummaryUtf8(record.lastError->message);
    }
    if (record.outputText) {
        return Domain::truncateAgentSummaryUtf8(*record.outputText);
    }
    return std::nullopt;
}

} // namespace

class AgentRepositoryManagedRunStore::Impl final {
public:
    Impl(
        Contracts::IAgentSessionRepository& repository,
        Domain::AgentId managedAgentId)
        : repository_{repository}, managedAgentId_{std::move(managedAgentId)}
    {
    }

    [[nodiscard]] Domain::Result<std::optional<Domain::ManagedRunRecord>> load(
        const Domain::SessionId& runId,
        const Domain::OperationContext& context) noexcept
    {
        auto loaded = repository_.getRun(runId, context);
        if (!loaded) {
            return Domain::Result<
                std::optional<Domain::ManagedRunRecord>>::failure(
                std::move(loaded).error());
        }
        if (!loaded.value()) {
            return Domain::Result<
                std::optional<Domain::ManagedRunRecord>>::success(std::nullopt);
        }
        const auto& run = *loaded.value();
        if (!run.projectId || !run.session.clientId ||
            !run.goal || run.session.agentId != managedAgentId_) {
            return Domain::Result<
                std::optional<Domain::ManagedRunRecord>>::failure(
                Domain::makeError(
                    Domain::ErrorCodes::IntegrityFailure,
                    "The durable run is not a complete managed-run record."));
        }
        Domain::ManagedRunRecord record{
            run.session.id,
            *run.projectId,
            *run.session.clientId,
            *run.goal,
            managedState(run.session.status),
            std::nullopt,
            0U,
            0U,
            std::nullopt,
            run.session.summary,
            run.session.status == Domain::SessionStatus::Failed &&
                    run.session.summary
                ? std::optional<Domain::Error>{Domain::makeError(
                      Domain::ErrorCodes::InternalFailure,
                      *run.session.summary)}
                : std::nullopt,
            run.session.createdAt,
            run.session.updatedAt};
        return Domain::Result<
            std::optional<Domain::ManagedRunRecord>>::success(
            std::move(record));
    }

    [[nodiscard]] Domain::Result<void> save(
        const Domain::ManagedRunRecord& record,
        const Domain::OperationContext& context) noexcept
    {
        try {
            auto loaded = repository_.getRun(record.runId, context);
            if (!loaded) {
                return Domain::Result<void>::failure(
                    std::move(loaded).error());
            }
            Domain::AgentSession session{
                record.runId,
                managedAgentId_,
                record.clientId,
                sessionStatus(record.state),
                summary(record),
                record.createdAt,
                record.updatedAt};
            if (!loaded.value()) {
                Domain::AgentRunStartMutation mutation{
                    Domain::AgentRunRecord{
                        session,
                        record.projectId,
                        record.task,
                        std::nullopt,
                        {},
                        {},
                        std::nullopt},
                    std::nullopt,
                    "Superseded by a newer Manager-owned run."};
                auto saved = repository_.startRun(mutation, context);
                if (!saved) {
                    return Domain::Result<void>::failure(
                        std::move(saved).error());
                }
                return Domain::Result<void>::success();
            }
            if (loaded.value()->session.agentId != managedAgentId_ ||
                loaded.value()->projectId !=
                    std::optional<Domain::ProjectId>{record.projectId} ||
                loaded.value()->session.clientId !=
                    std::optional<Domain::ClientId>{record.clientId} ||
                loaded.value()->goal !=
                    std::optional<std::string>{record.task}) {
                return Domain::Result<void>::failure(Domain::makeError(
                    Domain::ErrorCodes::Conflict,
                    "The durable run identity does not match this managed run."));
            }
            return repository_.save(session, context);
        } catch (...) {
            return Domain::Result<void>::failure(Domain::makeError(
                Domain::ErrorCodes::InternalFailure,
                "The managed-run record could not be persisted safely."));
        }
    }

private:
    Contracts::IAgentSessionRepository& repository_;
    Domain::AgentId managedAgentId_;
};

AgentRepositoryManagedRunStore::AgentRepositoryManagedRunStore(
    Contracts::IAgentSessionRepository& repository,
    Domain::AgentId managedAgentId)
    : implementation_{std::make_unique<Impl>(
          repository,
          std::move(managedAgentId))}
{
}

AgentRepositoryManagedRunStore::~AgentRepositoryManagedRunStore() noexcept =
    default;

Domain::Result<std::optional<Domain::ManagedRunRecord>>
AgentRepositoryManagedRunStore::load(
    const Domain::SessionId& runId,
    const Domain::OperationContext& context) noexcept
{
    return implementation_->load(runId, context);
}

Domain::Result<void> AgentRepositoryManagedRunStore::save(
    const Domain::ManagedRunRecord& record,
    const Domain::OperationContext& context) noexcept
{
    return implementation_->save(record, context);
}

} // namespace ForgeConductor::Application
