#pragma once

#include "ForgeConductor/Contracts/IFoundationServices.h"
#include "ForgeConductor/Contracts/IProjectMemoryService.h"

#include <memory>

namespace ForgeConductor::Application {

// The injected repositories and redactor must outlive this service. The service
// owns no platform or database resource directly; repository operation pins are
// retained for the complete synchronous call.
class ProjectMemoryService final : public Contracts::IProjectMemoryService {
public:
    ProjectMemoryService(
        Contracts::IProjectRegistryRepository& registry,
        Contracts::IProjectMemoryRepositoryFactory& repositoryFactory,
        Contracts::IRedactor& redactor,
        Domain::ProjectMemoryLimits limits);
    ~ProjectMemoryService() noexcept override;

    ProjectMemoryService(const ProjectMemoryService&) = delete;
    ProjectMemoryService& operator=(const ProjectMemoryService&) = delete;
    ProjectMemoryService(ProjectMemoryService&&) = delete;
    ProjectMemoryService& operator=(ProjectMemoryService&&) = delete;

    [[nodiscard]] Domain::Result<Domain::ProjectInitialization> initialize(
        const Domain::InitializeProjectRequest& request,
        const Domain::OperationContext& context) noexcept override;

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

    [[nodiscard]] Domain::Result<void> closeProject(
        const Domain::ProjectId& projectId,
        const Domain::OperationContext& context) noexcept override;

    [[nodiscard]] Domain::Result<Domain::ResetReport> resetProjectMemory(
        const Domain::ProjectId& projectId,
        const Domain::DestructiveConfirmation& confirmation,
        const Domain::OperationContext& context) noexcept override;

    [[nodiscard]] Domain::Result<Domain::ResetReport> resetAllProjectMemory(
        const Domain::DestructiveConfirmation& confirmation,
        const Domain::OperationContext& context) noexcept override;

    void shutdown() noexcept override;

private:
    class Impl;
    std::unique_ptr<Impl> implementation_;
};

} // namespace ForgeConductor::Application
