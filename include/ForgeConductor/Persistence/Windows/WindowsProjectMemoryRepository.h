#pragma once

#include "ForgeConductor/Contracts/IProjectMemoryArtifactStore.h"
#include "ForgeConductor/Contracts/IProjectMemoryService.h"
#include "ForgeConductor/Persistence/Windows/WindowsProjectDatabase.h"

#include <cstddef>
#include <memory>
#include <optional>
#include <string>

namespace ForgeConductor::Persistence::Windows {

struct WindowsProjectMemoryRepositoryOptions final {
    WindowsProjectDatabaseOptions database;
    Domain::ProjectMemoryLimits limits;
};

// Owns exactly one WindowsProjectDatabase and implements the project-scoped
// memory repository contract on that database's sole serialized connection.
class WindowsProjectMemoryRepository final
    : public Contracts::IProjectRepository {
public:
    [[nodiscard]] static Domain::Result<
        std::shared_ptr<WindowsProjectMemoryRepository>> open(
        const Domain::ProjectId& projectId,
        std::shared_ptr<Contracts::IApplicationPaths> applicationPaths,
        std::shared_ptr<Contracts::IProjectMemoryArtifactStore> artifactStore,
        std::shared_ptr<Contracts::IRuntimeDiagnostics> runtimeDiagnostics,
        std::shared_ptr<Contracts::IRedactor> redactor,
        std::shared_ptr<Contracts::IHasher> hasher,
        std::shared_ptr<Contracts::IUuidGenerator> uuidGenerator,
        std::shared_ptr<Contracts::IClock> clock,
        WindowsProjectMemoryRepositoryOptions options,
        const Domain::OperationContext& context) noexcept;

    // Infrastructure composition convenience retained for direct native tests.
    // Production composition injects the store explicitly through the opener.
    [[nodiscard]] static Domain::Result<
        std::shared_ptr<WindowsProjectMemoryRepository>> open(
        const Domain::ProjectId& projectId,
        std::shared_ptr<Contracts::IApplicationPaths> applicationPaths,
        std::shared_ptr<Contracts::IRuntimeDiagnostics> runtimeDiagnostics,
        std::shared_ptr<Contracts::IRedactor> redactor,
        std::shared_ptr<Contracts::IHasher> hasher,
        std::shared_ptr<Contracts::IUuidGenerator> uuidGenerator,
        std::shared_ptr<Contracts::IClock> clock,
        WindowsProjectMemoryRepositoryOptions options,
        const Domain::OperationContext& context) noexcept;

    [[nodiscard]] static Domain::Result<
        std::shared_ptr<WindowsProjectMemoryRepository>> create(
        std::unique_ptr<WindowsProjectDatabase> database,
        std::shared_ptr<Contracts::IProjectMemoryArtifactStore> artifactStore,
        std::shared_ptr<Contracts::IRedactor> redactor,
        std::shared_ptr<Contracts::IHasher> hasher,
        std::shared_ptr<Contracts::IUuidGenerator> uuidGenerator,
        std::shared_ptr<Contracts::IClock> clock,
        WindowsProjectMemoryRepositoryOptions options) noexcept;

    ~WindowsProjectMemoryRepository() noexcept override;

    WindowsProjectMemoryRepository(const WindowsProjectMemoryRepository&) = delete;
    WindowsProjectMemoryRepository& operator=(const WindowsProjectMemoryRepository&) = delete;
    WindowsProjectMemoryRepository(WindowsProjectMemoryRepository&&) = delete;
    WindowsProjectMemoryRepository& operator=(WindowsProjectMemoryRepository&&) = delete;

    [[nodiscard]] const Domain::ProjectId& projectId() const noexcept override;

    [[nodiscard]] Domain::Result<Domain::MemoryWriteOutcome> remember(
        const Domain::RememberProjectMemoryRequest& request,
        const Domain::OperationContext& context) noexcept override;
    [[nodiscard]] Domain::Result<Domain::MemoryBatchOutcome> rememberBatch(
        const Domain::RememberProjectMemoryBatchRequest& request,
        const Domain::OperationContext& context) noexcept override;
    [[nodiscard]] Domain::Result<Domain::MemoryPage> search(
        const Domain::SearchProjectMemoryRequest& request,
        const Domain::OperationContext& context) noexcept override;
    [[nodiscard]] Domain::Result<Domain::MemoryRecords> get(
        const Domain::GetProjectMemoryRequest& request,
        const Domain::OperationContext& context) noexcept override;
    [[nodiscard]] Domain::Result<Domain::ProjectMemoryRecord> update(
        const Domain::UpdateProjectMemoryRequest& request,
        const Domain::OperationContext& context) noexcept override;
    [[nodiscard]] Domain::Result<Domain::ForgetOutcome> forget(
        const Domain::ForgetProjectMemoryRequest& request,
        const Domain::OperationContext& context) noexcept override;
    [[nodiscard]] Domain::Result<Domain::MemoryPage> listRecent(
        const Domain::ListRecentProjectMemoryRequest& request,
        const Domain::OperationContext& context) noexcept override;
    [[nodiscard]] Domain::Result<Domain::LinkOutcome> link(
        const Domain::LinkProjectMemoryRequest& request,
        const Domain::OperationContext& context) noexcept override;
    [[nodiscard]] Domain::Result<Domain::ProjectMemoryExport> exportMemory(
        const Domain::ExportProjectMemoryRequest& request,
        const Contracts::WorkspaceAuthority& writeAuthority,
        const Contracts::AuthorizedToolCall& authorization,
        const Domain::OperationContext& context) noexcept override;
    [[nodiscard]] Domain::Result<Domain::ProjectMemoryImport> importMemory(
        const Domain::ImportProjectMemoryRequest& request,
        const Domain::OperationContext& context) noexcept override;
    [[nodiscard]] Domain::Result<Domain::ProjectMemoryStatus> status(
        const Domain::ProjectMemoryStatusRequest& request,
        const Domain::OperationContext& context) noexcept override;
    [[nodiscard]] Domain::Result<void> quickCheck(
        const Domain::OperationContext& context) noexcept override;
    [[nodiscard]] Domain::Result<Domain::ResetReport> resetMemory(
        const Domain::DestructiveConfirmation& confirmation,
        const Domain::OperationContext& context) noexcept override;

    [[nodiscard]] Domain::Result<Domain::ContinuityOperation> createOperation(
        const Domain::ContinuityHandoff& handoff,
        const Domain::IdempotencyKey& idempotencyKey,
        const Domain::OperationContext& context) noexcept override;
    [[nodiscard]] Domain::Result<void> storeHandoff(
        const Domain::ContinuityHandoff& handoff,
        const Domain::OperationContext& context) noexcept override;
    [[nodiscard]] Domain::Result<std::optional<Domain::ContinuityHandoff>> handoff(
        const Domain::ProjectId& projectId,
        const Domain::ContinuityHandoffId& handoffId,
        const Domain::OperationContext& context) noexcept override;
    [[nodiscard]] Domain::Result<std::optional<Domain::ContinuityOperation>> operation(
        const Domain::ProjectId& projectId,
        const Domain::ContinuityOperationId& operationId,
        const Domain::OperationContext& context) noexcept override;
    [[nodiscard]] Domain::Result<std::optional<Domain::ContinuityOperation>>
    activeOperation(
        const Domain::ProjectId& projectId,
        const Domain::OperationContext& context) noexcept override;
    [[nodiscard]] Domain::Result<Domain::ContinuityOperation> compareAndSet(
        const Domain::ContinuityOperationId& operationId,
        Domain::ContinuityState expected,
        Domain::ContinuityState next,
        std::optional<Domain::SessionId> successorSessionId,
        std::optional<std::string> evidence,
        const Domain::OperationContext& context) noexcept override;
    [[nodiscard]] Domain::Result<Domain::ContinuityOperation> acknowledge(
        const Domain::ContinuityOperationId& operationId,
        const Domain::HandoffAcknowledgement& acknowledgement,
        const Domain::OperationContext& context) noexcept override;
    [[nodiscard]] Domain::Result<Domain::ContinuityOperation> recordRetry(
        const Domain::ContinuityOperationId& operationId,
        Domain::ContinuityState resumeState,
        std::string error,
        std::optional<Domain::UtcTimePoint> retryAt,
        const Domain::OperationContext& context) noexcept override;
    [[nodiscard]] Domain::Result<std::optional<Domain::SessionId>> activeSession(
        const Domain::ProjectId& projectId,
        const Domain::OperationContext& context) noexcept override;
    [[nodiscard]] Domain::Result<std::size_t> transitionCount(
        const Domain::ContinuityOperationId& operationId,
        const Domain::OperationContext& context) noexcept override;
    [[nodiscard]] Domain::Result<Domain::ContinuityStatus> status(
        const Domain::ProjectId& projectId,
        const Domain::OperationContext& context) noexcept override;
    [[nodiscard]] Domain::Result<Domain::ContinuityResetReport> resetContinuity(
        const Domain::ContinuityResetRequest& request,
        const Domain::OperationContext& context) noexcept override;

    void close() noexcept override;

private:
    struct Impl;

    explicit WindowsProjectMemoryRepository(std::unique_ptr<Impl> implementation) noexcept;

    std::unique_ptr<Impl> implementation_;
};

} // namespace ForgeConductor::Persistence::Windows
