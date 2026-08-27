#pragma once

#include "ForgeConductor/Contracts/IAgentServices.h"
#include "ForgeConductor/Contracts/IDiagnosticsServices.h"
#include "ForgeConductor/Contracts/IFileSystemServices.h"
#include "ForgeConductor/Contracts/IFoundationServices.h"
#include "ForgeConductor/Contracts/ILegacyContinuitySessionSource.h"
#include "ForgeConductor/Persistence/Windows/WindowsCentralDatabase.h"

#include <memory>

namespace ForgeConductor::Persistence::Windows {

enum class AgentSessionTransactionKind {
    Start,
    Reattach,
    Complete,
    RepairProjection,
    CloseStale
};

enum class AgentSessionTransactionCheckpoint {
    BeforeCommit,
    AfterCommit
};

// Test seam for process-crash fixtures. Production factories never accept an
// observer and no environment or process-wide switch can enable callbacks.
class IAgentSessionTransactionObserver {
public:
    virtual ~IAgentSessionTransactionObserver() noexcept = default;

    virtual void onAgentSessionTransactionCheckpoint(
        AgentSessionTransactionKind kind,
        AgentSessionTransactionCheckpoint checkpoint) noexcept = 0;
};

class WindowsAgentSessionRepository final
    : public Contracts::IAgentSessionRepository,
      public Contracts::ILegacyContinuitySessionSource {
public:
    [[nodiscard]] static Domain::Result<
        std::shared_ptr<WindowsAgentSessionRepository>> open(
        std::shared_ptr<Contracts::IApplicationPaths> applicationPaths,
        std::shared_ptr<Contracts::IRuntimeDiagnostics> runtimeDiagnostics,
        std::shared_ptr<Contracts::IClock> clock,
        const Domain::OperationContext& context) noexcept;

    // The observer exists solely for deterministic native process-crash tests.
    [[nodiscard]] static Domain::Result<
        std::shared_ptr<WindowsAgentSessionRepository>> create(
        std::unique_ptr<WindowsCentralDatabase> database,
        std::shared_ptr<Contracts::IClock> clock,
        IAgentSessionTransactionObserver* transactionObserver = nullptr) noexcept;

    [[nodiscard]] static Domain::Result<
        std::shared_ptr<WindowsAgentSessionRepository>> attach(
        std::shared_ptr<WindowsCentralDatabase> database,
        std::shared_ptr<Contracts::IClock> clock) noexcept;

    ~WindowsAgentSessionRepository() noexcept override;

    WindowsAgentSessionRepository(const WindowsAgentSessionRepository&) = delete;
    WindowsAgentSessionRepository& operator=(
        const WindowsAgentSessionRepository&) = delete;
    WindowsAgentSessionRepository(WindowsAgentSessionRepository&&) = delete;
    WindowsAgentSessionRepository& operator=(
        WindowsAgentSessionRepository&&) = delete;

    [[nodiscard]] Domain::Result<void> save(
        const Domain::AgentSession& session,
        const Domain::OperationContext& context) noexcept override;
    [[nodiscard]] Domain::Result<std::optional<Domain::AgentSession>> get(
        const Domain::SessionId& sessionId,
        const Domain::OperationContext& context) noexcept override;
    [[nodiscard]] Domain::Result<std::vector<Domain::AgentSession>> list(
        const std::optional<Domain::AgentId>& agentId,
        const std::optional<Domain::SessionStatus>& status,
        std::size_t maximumCount,
        const Domain::OperationContext& context) noexcept override;
    [[nodiscard]] Domain::Result<Domain::AgentRunStartPersistenceOutcome> startRun(
        const Domain::AgentRunStartMutation& mutation,
        const Domain::OperationContext& context) noexcept override;
    [[nodiscard]] Domain::Result<std::optional<Domain::AgentRunRecord>> getRun(
        const Domain::SessionId& sessionId,
        const Domain::OperationContext& context) noexcept override;
    [[nodiscard]] Domain::Result<Domain::AgentRunReattachOutcome> reattachRun(
        const Domain::AgentRunReattachMutation& mutation,
        const Domain::OperationContext& context) noexcept override;
    [[nodiscard]] Domain::Result<Domain::AgentRunCompletePersistenceOutcome>
    completeRun(
        const Domain::AgentRunCompleteMutation& mutation,
        const Domain::OperationContext& context) noexcept override;
    [[nodiscard]] Domain::Result<bool> touchRun(
        const Domain::SessionId& sessionId,
        Domain::UtcTimePoint touchedAt,
        const Domain::OperationContext& context) noexcept override;
    [[nodiscard]] Domain::Result<std::optional<Domain::AgentRunRecord>>
    latestOpenRun(
        const Domain::ClientId& clientId,
        const Domain::OperationContext& context) noexcept override;
    [[nodiscard]] Domain::Result<Domain::AgentRunRecoveryOutcome> recoverRun(
        const Domain::AgentRunRecoveryRequest& request,
        const Domain::OperationContext& context) noexcept override;
    [[nodiscard]] Domain::Result<Domain::AgentProjectionRepairOutcome>
    repairProjection(
        const Domain::AgentProjectionRepairRequest& request,
        const Domain::OperationContext& context) noexcept override;
    [[nodiscard]] Domain::Result<Domain::AgentStaleCloseOutcome> closeStale(
        const Domain::AgentStaleCloseRequest& request,
        const Domain::OperationContext& context) noexcept override;
    [[nodiscard]] Domain::Result<
        std::vector<Domain::LegacyAgentContinuitySnapshot>>
    listOpenForClient(
        const Domain::ClientId& clientId,
        std::size_t maximumCount,
        const Domain::OperationContext& context) noexcept override;
    [[nodiscard]] Domain::Result<std::size_t> countOpen(
        std::size_t maximumCount,
        const Domain::OperationContext& context) noexcept override;
    [[nodiscard]] Domain::Result<bool> isOpen(
        const Domain::SessionId& sessionId,
        const Domain::OperationContext& context) noexcept override;
    [[nodiscard]] Domain::Result<
        std::optional<Domain::LegacyActiveBindingSnapshot>>
    binding(
        const Domain::ClientId& clientId,
        const Domain::OperationContext& context) noexcept override;
    [[nodiscard]] Domain::Result<void> quickCheck(
        const Domain::OperationContext& context) noexcept override;

    void close() noexcept override;

private:
    struct Impl;

    explicit WindowsAgentSessionRepository(
        std::unique_ptr<Impl> implementation) noexcept;

    std::unique_ptr<Impl> implementation_;
};

} // namespace ForgeConductor::Persistence::Windows
