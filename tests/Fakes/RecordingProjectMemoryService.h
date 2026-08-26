#pragma once

#include "ForgeConductor/Contracts/IProjectMemoryService.h"
#include "DeterministicResult.h"

#include <array>
#include <cstddef>
#include <optional>

namespace ForgeConductor::Tests::Fakes {

enum class ProjectMemoryCall : std::size_t {
    Initialize,
    Remember,
    RememberBatch,
    Search,
    Get,
    Update,
    Forget,
    ListRecent,
    Link,
    Export,
    Import,
    Status,
    CloseProject,
    ResetProjectMemory,
    ResetAllProjectMemory,
    Count
};

class RecordingProjectMemoryService final
    : public Contracts::IProjectMemoryService {
public:
    DeterministicResult<Domain::ProjectInitialization> initializeResult;
    DeterministicResult<Domain::MemoryWriteOutcome> rememberResult;
    DeterministicResult<Domain::MemoryBatchOutcome> rememberBatchResult;
    DeterministicResult<Domain::MemoryPage> searchResult;
    DeterministicResult<Domain::MemoryRecords> getResult;
    DeterministicResult<Domain::ProjectMemoryRecord> updateResult;
    DeterministicResult<Domain::ForgetOutcome> forgetResult;
    DeterministicResult<Domain::MemoryPage> listRecentResult;
    DeterministicResult<Domain::LinkOutcome> linkResult;
    DeterministicResult<Domain::ProjectMemoryExport> exportResult;
    DeterministicResult<Domain::ProjectMemoryImport> importResult;
    DeterministicResult<Domain::ProjectMemoryStatus> statusResult;
    DeterministicResult<void> closeProjectResult;
    DeterministicResult<Domain::ResetReport> resetProjectMemoryResult;
    DeterministicResult<Domain::ResetReport> resetAllProjectMemoryResult;

    [[nodiscard]] Domain::Result<Domain::ProjectInitialization> initialize(
        const Domain::InitializeProjectRequest& request,
        const Domain::OperationContext& context) noexcept override
    {
        const auto* projectId = request.requestedProjectId
            ? &request.requestedProjectId.value()
            : nullptr;
        return complete(
            ProjectMemoryCall::Initialize,
            projectId,
            context,
            initializeResult);
    }

    [[nodiscard]] Domain::Result<Domain::MemoryWriteOutcome> remember(
        const Domain::RememberProjectMemoryRequest& request,
        const Domain::OperationContext& context) noexcept override
    {
        return complete(
            ProjectMemoryCall::Remember,
            &request.projectId,
            context,
            rememberResult);
    }

    [[nodiscard]] Domain::Result<Domain::MemoryBatchOutcome> rememberBatch(
        const Domain::RememberProjectMemoryBatchRequest& request,
        const Domain::OperationContext& context) noexcept override
    {
        return complete(
            ProjectMemoryCall::RememberBatch,
            &request.projectId,
            context,
            rememberBatchResult);
    }

    [[nodiscard]] Domain::Result<Domain::MemoryPage> search(
        const Domain::SearchProjectMemoryRequest& request,
        const Domain::OperationContext& context) noexcept override
    {
        return complete(
            ProjectMemoryCall::Search,
            &request.projectId,
            context,
            searchResult);
    }

    [[nodiscard]] Domain::Result<Domain::MemoryRecords> get(
        const Domain::GetProjectMemoryRequest& request,
        const Domain::OperationContext& context) noexcept override
    {
        return complete(
            ProjectMemoryCall::Get,
            &request.projectId,
            context,
            getResult);
    }

    [[nodiscard]] Domain::Result<Domain::ProjectMemoryRecord> update(
        const Domain::UpdateProjectMemoryRequest& request,
        const Domain::OperationContext& context) noexcept override
    {
        return complete(
            ProjectMemoryCall::Update,
            &request.projectId,
            context,
            updateResult);
    }

    [[nodiscard]] Domain::Result<Domain::ForgetOutcome> forget(
        const Domain::ForgetProjectMemoryRequest& request,
        const Domain::OperationContext& context) noexcept override
    {
        return complete(
            ProjectMemoryCall::Forget,
            &request.projectId,
            context,
            forgetResult);
    }

    [[nodiscard]] Domain::Result<Domain::MemoryPage> listRecent(
        const Domain::ListRecentProjectMemoryRequest& request,
        const Domain::OperationContext& context) noexcept override
    {
        return complete(
            ProjectMemoryCall::ListRecent,
            &request.projectId,
            context,
            listRecentResult);
    }

    [[nodiscard]] Domain::Result<Domain::LinkOutcome> link(
        const Domain::LinkProjectMemoryRequest& request,
        const Domain::OperationContext& context) noexcept override
    {
        return complete(
            ProjectMemoryCall::Link,
            &request.projectId,
            context,
            linkResult);
    }

    [[nodiscard]] Domain::Result<Domain::ProjectMemoryExport> exportMemory(
        const Domain::ExportProjectMemoryRequest& request,
        const Contracts::WorkspaceAuthority& writeAuthority,
        const Contracts::AuthorizedToolCall& authorization,
        const Domain::OperationContext& context) noexcept override
    {
        try {
            lastExportAuthority_.emplace(writeAuthority);
            lastExportAuthorization_.emplace(authorization);
            if (writeAuthority.projectId() != request.projectId ||
                writeAuthority.intent() != Domain::FileAccess::Write ||
                !authorization.matches(writeAuthority, context) ||
                !authorization.matchesProject(request.projectId) ||
                authorization.toolName() != "project_memory.export" ||
                authorization.effect() != Domain::ToolEffect::Write) {
                return Domain::Result<Domain::ProjectMemoryExport>::failure(
                    Domain::makeError(
                        Domain::ErrorCodes::Unauthorized,
                        "The project-memory export capability is mismatched."));
            }
            return complete(
                ProjectMemoryCall::Export,
                &request.projectId,
                context,
                exportResult);
        } catch (...) {
            return Domain::Result<Domain::ProjectMemoryExport>::failure(
                Domain::makeError(
                    Domain::ErrorCodes::InternalFailure,
                    "The export authority or capability could not be recorded."));
        }
    }

    [[nodiscard]] Domain::Result<Domain::ProjectMemoryImport> importMemory(
        const Domain::ImportProjectMemoryRequest& request,
        const Domain::OperationContext& context) noexcept override
    {
        return complete(
            ProjectMemoryCall::Import,
            &request.projectId,
            context,
            importResult);
    }

    [[nodiscard]] Domain::Result<Domain::ProjectMemoryStatus> status(
        const Domain::ProjectMemoryStatusRequest& request,
        const Domain::OperationContext& context) noexcept override
    {
        return complete(
            ProjectMemoryCall::Status,
            &request.projectId,
            context,
            statusResult);
    }

    [[nodiscard]] Domain::Result<void> closeProject(
        const Domain::ProjectId& projectId,
        const Domain::OperationContext& context) noexcept override
    {
        return complete(
            ProjectMemoryCall::CloseProject,
            &projectId,
            context,
            closeProjectResult);
    }

    [[nodiscard]] Domain::Result<Domain::ResetReport> resetProjectMemory(
        const Domain::ProjectId& projectId,
        const Domain::DestructiveConfirmation& confirmation,
        const Domain::OperationContext& context) noexcept override
    {
        try {
            lastResetConfirmation_ = confirmation;
            return complete(
                ProjectMemoryCall::ResetProjectMemory,
                &projectId,
                context,
                resetProjectMemoryResult);
        } catch (...) {
            return Domain::Result<Domain::ResetReport>::failure(
                Domain::makeError(
                    Domain::ErrorCodes::InternalFailure,
                    "The project-memory reset confirmation could not be recorded."));
        }
    }

    [[nodiscard]] Domain::Result<Domain::ResetReport> resetAllProjectMemory(
        const Domain::DestructiveConfirmation& confirmation,
        const Domain::OperationContext& context) noexcept override
    {
        try {
            lastResetConfirmation_ = confirmation;
            return complete(
                ProjectMemoryCall::ResetAllProjectMemory,
                nullptr,
                context,
                resetAllProjectMemoryResult);
        } catch (...) {
            return Domain::Result<Domain::ResetReport>::failure(
                Domain::makeError(
                    Domain::ErrorCodes::InternalFailure,
                    "The all-project-memory reset confirmation could not be recorded."));
        }
    }

    void shutdown() noexcept override
    {
        shutdown_ = true;
    }

    void setNow(const Domain::MonotonicTimePoint now) noexcept
    {
        now_ = now;
    }

    [[nodiscard]] std::size_t callCount(
        const ProjectMemoryCall call) const noexcept
    {
        return calls_[static_cast<std::size_t>(call)];
    }

    [[nodiscard]] const std::optional<Domain::ProjectId>&
    lastProjectId() const noexcept
    {
        return lastProjectId_;
    }

    [[nodiscard]] const std::optional<Domain::OperationId>&
    lastOperationId() const noexcept
    {
        return lastOperationId_;
    }
    [[nodiscard]] const std::optional<Contracts::WorkspaceAuthority>&
    lastExportAuthority() const noexcept
    {
        return lastExportAuthority_;
    }

    [[nodiscard]] const std::optional<Contracts::AuthorizedToolCall>&
    lastExportAuthorization() const noexcept
    {
        return lastExportAuthorization_;
    }

    [[nodiscard]] const std::optional<Domain::DestructiveConfirmation>&
    lastResetConfirmation() const noexcept
    {
        return lastResetConfirmation_;
    }

private:
    template <typename T>
    [[nodiscard]] Domain::Result<T> complete(
        const ProjectMemoryCall call,
        const Domain::ProjectId* const projectId,
        const Domain::OperationContext& context,
        const DeterministicResult<T>& scriptedResult) noexcept
    {
        try {
            ++calls_[static_cast<std::size_t>(call)];
            lastOperationId_ = context.operationId;
            if (projectId != nullptr) {
                lastProjectId_ = *projectId;
            } else {
                lastProjectId_.reset();
            }
            if (shutdown_ || context.isCancellationRequested()) {
                return Domain::Result<T>::failure(Domain::makeError(
                    Domain::ErrorCodes::Cancelled,
                    "The deterministic project-memory operation was cancelled."));
            }
            if (context.isExpired(now_)) {
                return Domain::Result<T>::failure(Domain::makeError(
                    Domain::ErrorCodes::DeadlineExceeded,
                    "The deterministic project-memory deadline expired."));
            }
            return scriptedResult.get();
        } catch (...) {
            return Domain::Result<T>::failure(Domain::makeError(
                Domain::ErrorCodes::InternalFailure,
                "The deterministic project-memory call could not be recorded."));
        }
    }

    std::array<
        std::size_t,
        static_cast<std::size_t>(ProjectMemoryCall::Count)> calls_{};
    std::optional<Domain::ProjectId> lastProjectId_;
    std::optional<Domain::OperationId> lastOperationId_;
    std::optional<Contracts::WorkspaceAuthority> lastExportAuthority_;
    std::optional<Contracts::AuthorizedToolCall> lastExportAuthorization_;
    std::optional<Domain::DestructiveConfirmation> lastResetConfirmation_;
    Domain::MonotonicTimePoint now_{};
    bool shutdown_{};
};

} // namespace ForgeConductor::Tests::Fakes
