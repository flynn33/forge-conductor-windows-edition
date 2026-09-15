#pragma once

#include "ForgeConductor/Contracts/IAgentServices.h"
#include "ForgeConductor/Contracts/IFoundationServices.h"
#include "ForgeConductor/Contracts/IManagedRunServices.h"

#include <memory>

namespace ForgeConductor::Application {

// Persists managed-run identity and terminal state through the existing
// durable agent-run repository. The repository remains the owner of its
// transaction and must outlive this adapter.
class AgentRepositoryManagedRunStore final
    : public Contracts::IManagedRunStore {
public:
    AgentRepositoryManagedRunStore(
        Contracts::IAgentSessionRepository& repository,
        Domain::AgentId managedAgentId,
        Contracts::IHasher& hasher);
    ~AgentRepositoryManagedRunStore() noexcept override;

    AgentRepositoryManagedRunStore(
        const AgentRepositoryManagedRunStore&) = delete;
    AgentRepositoryManagedRunStore& operator=(
        const AgentRepositoryManagedRunStore&) = delete;
    AgentRepositoryManagedRunStore(
        AgentRepositoryManagedRunStore&&) = delete;
    AgentRepositoryManagedRunStore& operator=(
        AgentRepositoryManagedRunStore&&) = delete;

    [[nodiscard]] Domain::Result<std::optional<Domain::ManagedRunRecord>> load(
        const Domain::SessionId& runId,
        const Domain::OperationContext& context) noexcept override;

    [[nodiscard]] Domain::Result<void> save(
        const Domain::ManagedRunRecord& record,
        const Domain::OperationContext& context) noexcept override;

private:
    class Impl;
    std::unique_ptr<Impl> implementation_;
};

} // namespace ForgeConductor::Application
