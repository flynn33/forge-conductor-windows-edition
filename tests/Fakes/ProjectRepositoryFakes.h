#pragma once

#include "BoundedFakeSupport.h"
#include "ContinuityRepositoryFake.h"
#include "DeterministicResult.h"
#include "FoundationFakes.h"
#include "ForgeConductor/Contracts/ILegacyMemoryService.h"
#include "ForgeConductor/Contracts/IProjectMemoryService.h"

#include <algorithm>
#include <cstddef>
#include <memory>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace ForgeConductor::Tests::Fakes {

struct ProjectRegistryDetachRequest final {
    Domain::ProjectId projectId;
    Domain::PathText alias;
};

using ProjectRegistryRequest = std::variant<
    Domain::InitializeProjectRequest,
    Domain::ProjectId,
    std::size_t,
    ProjectRegistryDetachRequest>;

class ProjectRegistryRepositoryFake final
    : public Contracts::IProjectRegistryRepository {
public:
    explicit ProjectRegistryRepositoryFake(
        const std::size_t maximumDescriptors,
        const Domain::MonotonicTimePoint now = {}) noexcept
        : maximumDescriptors_{maximumDescriptors}, gate_{now}
    {
    }

    [[nodiscard]] Domain::Result<void> seedInitialization(
        Domain::ProjectInitialization initialization) noexcept
    {
        try {
            initialization_.emplace(std::move(initialization));
            return Domain::Result<void>::success();
        } catch (...) {
            return fakeInternalFailure<void>();
        }
    }

    [[nodiscard]] Domain::Result<void> seedDescriptor(
        Domain::ProjectMemoryDescriptor descriptor) noexcept
    {
        try {
            const auto existing = std::find_if(
                descriptors_.begin(),
                descriptors_.end(),
                [&](const Domain::ProjectMemoryDescriptor& candidate) {
                    return candidate.id == descriptor.id;
                });
            if (existing != descriptors_.end()) {
                *existing = std::move(descriptor);
                return Domain::Result<void>::success();
            }
            if (descriptors_.size() >= maximumDescriptors_) {
                return Domain::Result<void>::failure(Domain::makeError(
                    Domain::ErrorCodes::LimitExceeded,
                    "The deterministic project registry is at capacity."));
            }
            descriptors_.push_back(std::move(descriptor));
            return Domain::Result<void>::success();
        } catch (...) {
            return fakeInternalFailure<void>();
        }
    }

    [[nodiscard]] Domain::Result<Domain::ProjectInitialization> initialize(
        const Domain::InitializeProjectRequest& request,
        const Domain::OperationContext& context) noexcept override
    {
        try {
            lastRequest_.emplace(request);
            auto accepted = gate_.enter(context);
            if (!accepted) {
                return propagateFakeGateFailure<Domain::ProjectInitialization>(
                    std::move(accepted));
            }
            if (!initialization_) {
                return Domain::Result<Domain::ProjectInitialization>::failure(
                    Domain::makeError(
                        Domain::ErrorCodes::ProjectNotFound,
                        "No deterministic project initialization was seeded."));
            }
            if (request.requestedProjectId &&
                request.requestedProjectId.value() != initialization_->project.id) {
                return Domain::Result<Domain::ProjectInitialization>::failure(
                    Domain::makeError(
                        Domain::ErrorCodes::ProjectScopeMismatch,
                        "The requested project does not match the seeded project."));
            }
            return Domain::Result<Domain::ProjectInitialization>::success(
                initialization_.value());
        } catch (...) {
            return fakeInternalFailure<Domain::ProjectInitialization>();
        }
    }

    [[nodiscard]] Domain::Result<Domain::ProjectMemoryDescriptor> descriptor(
        const Domain::ProjectId& projectId,
        const Domain::OperationContext& context) noexcept override
    {
        try {
            lastRequest_.emplace(projectId);
            auto accepted = gate_.enter(context);
            if (!accepted) {
                return propagateFakeGateFailure<Domain::ProjectMemoryDescriptor>(
                    std::move(accepted));
            }
            const auto match = std::find_if(
                descriptors_.begin(),
                descriptors_.end(),
                [&](const Domain::ProjectMemoryDescriptor& candidate) {
                    return candidate.id == projectId;
                });
            if (match == descriptors_.end()) {
                return Domain::Result<Domain::ProjectMemoryDescriptor>::failure(
                    Domain::makeError(
                        Domain::ErrorCodes::ProjectNotFound,
                        "The deterministic project descriptor was not found."));
            }
            return Domain::Result<Domain::ProjectMemoryDescriptor>::success(*match);
        } catch (...) {
            return fakeInternalFailure<Domain::ProjectMemoryDescriptor>();
        }
    }

    [[nodiscard]] Domain::Result<std::vector<Domain::ProjectMemoryDescriptor>> list(
        const std::size_t maximumCount,
        const Domain::OperationContext& context) noexcept override
    {
        try {
            lastRequest_.emplace(maximumCount);
            auto accepted = gate_.enter(context);
            if (!accepted) {
                return propagateFakeGateFailure<
                    std::vector<Domain::ProjectMemoryDescriptor>>(std::move(accepted));
            }
            const auto count = std::min(maximumCount, descriptors_.size());
            return Domain::Result<std::vector<Domain::ProjectMemoryDescriptor>>::success(
                std::vector<Domain::ProjectMemoryDescriptor>{
                    descriptors_.begin(),
                    descriptors_.begin() + static_cast<std::ptrdiff_t>(count)});
        } catch (...) {
            return fakeInternalFailure<std::vector<Domain::ProjectMemoryDescriptor>>();
        }
    }

    [[nodiscard]] Domain::Result<void> detachAlias(
        const Domain::ProjectId& projectId,
        const Domain::PathText& alias,
        const Domain::OperationContext& context) noexcept override
    {
        try {
            lastRequest_.emplace(ProjectRegistryDetachRequest{projectId, alias});
            auto accepted = gate_.enter(context);
            if (!accepted) {
                return accepted;
            }
            const auto match = std::find_if(
                descriptors_.begin(),
                descriptors_.end(),
                [&](const Domain::ProjectMemoryDescriptor& candidate) {
                    return candidate.id == projectId;
                });
            if (match == descriptors_.end()) {
                return Domain::Result<void>::failure(Domain::makeError(
                    Domain::ErrorCodes::ProjectNotFound,
                    "The deterministic project descriptor was not found."));
            }
            match->aliases.erase(
                std::remove(match->aliases.begin(), match->aliases.end(), alias),
                match->aliases.end());
            return Domain::Result<void>::success();
        } catch (...) {
            return fakeInternalFailure<void>();
        }
    }

    void shutdown() noexcept { gate_.shutdown(); }
    void setNow(const Domain::MonotonicTimePoint now) noexcept { gate_.setNow(now); }

    [[nodiscard]] std::size_t descriptorCount() const noexcept
    {
        return descriptors_.size();
    }

    [[nodiscard]] const std::optional<ProjectRegistryRequest>&
    lastRequest() const noexcept
    {
        return lastRequest_;
    }

private:
    const std::size_t maximumDescriptors_;
    std::vector<Domain::ProjectMemoryDescriptor> descriptors_;
    std::optional<Domain::ProjectInitialization> initialization_;
    std::optional<ProjectRegistryRequest> lastRequest_;
    BoundedFakeOperationGate gate_;
};

enum class ProjectMemoryRepositoryCall {
    None,
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
    QuickCheck
};

using ProjectMemoryRepositoryRequest = std::variant<
    Domain::RememberProjectMemoryRequest,
    Domain::RememberProjectMemoryBatchRequest,
    Domain::SearchProjectMemoryRequest,
    Domain::GetProjectMemoryRequest,
    Domain::UpdateProjectMemoryRequest,
    Domain::ForgetProjectMemoryRequest,
    Domain::ListRecentProjectMemoryRequest,
    Domain::LinkProjectMemoryRequest,
    Domain::ExportProjectMemoryRequest,
    Domain::ImportProjectMemoryRequest,
    Domain::ProjectMemoryStatusRequest>;

class ProjectMemoryRepositoryFake final
    : public Contracts::IProjectRepository {
public:
    explicit ProjectMemoryRepositoryFake(
        Domain::ProjectId projectId,
        const Domain::MonotonicTimePoint now = {})
        : projectId_{std::move(projectId)}, continuity_{projectId_, now}, gate_{now}
    {
    }

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
    DeterministicResult<void> quickCheckResult;
    DeterministicResult<Domain::ResetReport> resetResult;

    [[nodiscard]] const Domain::ProjectId& projectId() const noexcept override
    {
        return projectId_;
    }

    [[nodiscard]] Domain::Result<Domain::MemoryWriteOutcome> remember(
        const Domain::RememberProjectMemoryRequest& request,
        const Domain::OperationContext& context) noexcept override
    {
        return complete(
            ProjectMemoryRepositoryCall::Remember,
            request,
            context,
            rememberResult);
    }

    [[nodiscard]] Domain::Result<Domain::MemoryBatchOutcome> rememberBatch(
        const Domain::RememberProjectMemoryBatchRequest& request,
        const Domain::OperationContext& context) noexcept override
    {
        return complete(
            ProjectMemoryRepositoryCall::RememberBatch,
            request,
            context,
            rememberBatchResult);
    }

    [[nodiscard]] Domain::Result<Domain::MemoryPage> search(
        const Domain::SearchProjectMemoryRequest& request,
        const Domain::OperationContext& context) noexcept override
    {
        return complete(
            ProjectMemoryRepositoryCall::Search,
            request,
            context,
            searchResult);
    }

    [[nodiscard]] Domain::Result<Domain::MemoryRecords> get(
        const Domain::GetProjectMemoryRequest& request,
        const Domain::OperationContext& context) noexcept override
    {
        return complete(
            ProjectMemoryRepositoryCall::Get,
            request,
            context,
            getResult);
    }

    [[nodiscard]] Domain::Result<Domain::ProjectMemoryRecord> update(
        const Domain::UpdateProjectMemoryRequest& request,
        const Domain::OperationContext& context) noexcept override
    {
        return complete(
            ProjectMemoryRepositoryCall::Update,
            request,
            context,
            updateResult);
    }

    [[nodiscard]] Domain::Result<Domain::ForgetOutcome> forget(
        const Domain::ForgetProjectMemoryRequest& request,
        const Domain::OperationContext& context) noexcept override
    {
        return complete(
            ProjectMemoryRepositoryCall::Forget,
            request,
            context,
            forgetResult);
    }

    [[nodiscard]] Domain::Result<Domain::MemoryPage> listRecent(
        const Domain::ListRecentProjectMemoryRequest& request,
        const Domain::OperationContext& context) noexcept override
    {
        return complete(
            ProjectMemoryRepositoryCall::ListRecent,
            request,
            context,
            listRecentResult);
    }

    [[nodiscard]] Domain::Result<Domain::LinkOutcome> link(
        const Domain::LinkProjectMemoryRequest& request,
        const Domain::OperationContext& context) noexcept override
    {
        return complete(
            ProjectMemoryRepositoryCall::Link,
            request,
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
            if (request.projectId != projectId_ ||
                writeAuthority.projectId() != projectId_ ||
                writeAuthority.intent() != Domain::FileAccess::Write ||
                !authorization.matches(writeAuthority, context) ||
                !authorization.matchesProject(projectId_) ||
                authorization.toolName() != "project_memory.export" ||
                authorization.effect() != Domain::ToolEffect::Write) {
                return Domain::Result<Domain::ProjectMemoryExport>::failure(
                    Domain::makeError(
                        Domain::ErrorCodes::Unauthorized,
                        "The export authority or tool capability is mismatched."));
            }
            return complete(
                ProjectMemoryRepositoryCall::Export,
                request,
                context,
                exportResult);
        } catch (...) {
            return fakeInternalFailure<Domain::ProjectMemoryExport>();
        }
    }

    [[nodiscard]] Domain::Result<Domain::ProjectMemoryImport> importMemory(
        const Domain::ImportProjectMemoryRequest& request,
        const Domain::OperationContext& context) noexcept override
    {
        return complete(
            ProjectMemoryRepositoryCall::Import,
            request,
            context,
            importResult);
    }

    [[nodiscard]] Domain::Result<Domain::ProjectMemoryStatus> status(
        const Domain::ProjectMemoryStatusRequest& request,
        const Domain::OperationContext& context) noexcept override
    {
        return complete(
            ProjectMemoryRepositoryCall::Status,
            request,
            context,
            statusResult);
    }

    [[nodiscard]] Domain::Result<void> quickCheck(
        const Domain::OperationContext& context) noexcept override
    {
        try {
            lastCall_ = ProjectMemoryRepositoryCall::QuickCheck;
            lastRequest_.reset();
            auto accepted = gate_.enter(context);
            if (!accepted) {
                return accepted;
            }
            return quickCheckResult.get();
        } catch (...) {
            return fakeInternalFailure<void>();
        }
    }

    [[nodiscard]] Domain::Result<Domain::ResetReport> resetMemory(
        const Domain::DestructiveConfirmation& confirmation,
        const Domain::OperationContext& context) noexcept override
    {
        try {
            lastResetConfirmation_.emplace(confirmation);
            auto accepted = gate_.enter(context);
            if (!accepted) {
                return propagateFakeGateFailure<Domain::ResetReport>(
                    std::move(accepted));
            }
            return resetResult.get();
        } catch (...) {
            return fakeInternalFailure<Domain::ResetReport>();
        }
    }

    [[nodiscard]] Domain::Result<Domain::ContinuityOperation> createOperation(
        const Domain::ContinuityHandoff& handoff,
        const Domain::IdempotencyKey& idempotencyKey,
        const Domain::OperationContext& context) noexcept override
    {
        return continuity_.createOperation(handoff, idempotencyKey, context);
    }

    [[nodiscard]] Domain::Result<void> storeHandoff(
        const Domain::ContinuityHandoff& handoff,
        const Domain::OperationContext& context) noexcept override
    {
        return continuity_.storeHandoff(handoff, context);
    }

    [[nodiscard]] Domain::Result<std::optional<Domain::ContinuityHandoff>> handoff(
        const Domain::ProjectId& projectId,
        const Domain::ContinuityHandoffId& handoffId,
        const Domain::OperationContext& context) noexcept override
    {
        return continuity_.handoff(projectId, handoffId, context);
    }

    [[nodiscard]] Domain::Result<std::optional<Domain::ContinuityOperation>>
    operation(
        const Domain::ProjectId& projectId,
        const Domain::ContinuityOperationId& operationId,
        const Domain::OperationContext& context) noexcept override
    {
        return continuity_.operation(projectId, operationId, context);
    }

    [[nodiscard]] Domain::Result<std::optional<Domain::ContinuityOperation>>
    activeOperation(
        const Domain::ProjectId& projectId,
        const Domain::OperationContext& context) noexcept override
    {
        return continuity_.activeOperation(projectId, context);
    }

    [[nodiscard]] Domain::Result<Domain::ContinuityOperation> compareAndSet(
        const Domain::ContinuityOperationId& operationId,
        const Domain::ContinuityState expected,
        const Domain::ContinuityState next,
        std::optional<Domain::SessionId> successorSessionId,
        std::optional<std::string> evidence,
        const Domain::OperationContext& context) noexcept override
    {
        return continuity_.compareAndSet(
            operationId,
            expected,
            next,
            std::move(successorSessionId),
            std::move(evidence),
            context);
    }

    [[nodiscard]] Domain::Result<Domain::ContinuityOperation> acknowledge(
        const Domain::ContinuityOperationId& operationId,
        const Domain::HandoffAcknowledgement& acknowledgement,
        const Domain::OperationContext& context) noexcept override
    {
        return continuity_.acknowledge(operationId, acknowledgement, context);
    }

    [[nodiscard]] Domain::Result<Domain::ContinuityOperation> recordRetry(
        const Domain::ContinuityOperationId& operationId,
        const Domain::ContinuityState resumeState,
        std::string error,
        std::optional<Domain::UtcTimePoint> retryAt,
        const Domain::OperationContext& context) noexcept override
    {
        return continuity_.recordRetry(
            operationId,
            resumeState,
            std::move(error),
            retryAt,
            context);
    }

    [[nodiscard]] Domain::Result<std::optional<Domain::SessionId>> activeSession(
        const Domain::ProjectId& projectId,
        const Domain::OperationContext& context) noexcept override
    {
        return continuity_.activeSession(projectId, context);
    }

    [[nodiscard]] Domain::Result<std::size_t> transitionCount(
        const Domain::ContinuityOperationId& operationId,
        const Domain::OperationContext& context) noexcept override
    {
        return continuity_.transitionCount(operationId, context);
    }

    [[nodiscard]] Domain::Result<Domain::ContinuityStatus> status(
        const Domain::ProjectId& projectId,
        const Domain::OperationContext& context) noexcept override
    {
        return continuity_.status(projectId, context);
    }

    [[nodiscard]] Domain::Result<Domain::ContinuityResetReport> resetContinuity(
        const Domain::ContinuityResetRequest& request,
        const Domain::OperationContext& context) noexcept override
    {
        return continuity_.resetContinuity(request, context);
    }

    void close() noexcept override
    {
        continuity_.close();
        gate_.close();
        ++closeCount_;
    }

    void setNow(const Domain::MonotonicTimePoint now) noexcept
    {
        continuity_.setNow(now);
        gate_.setNow(now);
    }

    [[nodiscard]] ProjectMemoryRepositoryCall lastCall() const noexcept
    {
        return lastCall_;
    }

    [[nodiscard]] const std::optional<ProjectMemoryRepositoryRequest>&
    lastRequest() const noexcept
    {
        return lastRequest_;
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

    [[nodiscard]] const ContinuityRepositoryFake& continuity() const noexcept
    {
        return continuity_;
    }

    [[nodiscard]] ContinuityRepositoryFake& continuity() noexcept
    {
        return continuity_;
    }

    [[nodiscard]] std::size_t closeCount() const noexcept { return closeCount_; }

private:
    template <typename Request, typename T>
    [[nodiscard]] Domain::Result<T> complete(
        const ProjectMemoryRepositoryCall call,
        const Request& request,
        const Domain::OperationContext& context,
        const DeterministicResult<T>& result) noexcept
    {
        try {
            lastCall_ = call;
            lastRequest_.emplace(request);
            auto accepted = gate_.enter(context);
            if (!accepted) {
                return propagateFakeGateFailure<T>(std::move(accepted));
            }
            if (request.projectId != projectId_) {
                return Domain::Result<T>::failure(Domain::makeError(
                    Domain::ErrorCodes::ProjectScopeMismatch,
                    "The request belongs to another project repository."));
            }
            return result.get();
        } catch (...) {
            return fakeInternalFailure<T>();
        }
    }

    Domain::ProjectId projectId_;
    ContinuityRepositoryFake continuity_;
    BoundedFakeOperationGate gate_;
    ProjectMemoryRepositoryCall lastCall_{ProjectMemoryRepositoryCall::None};
    std::optional<ProjectMemoryRepositoryRequest> lastRequest_;
    std::optional<Contracts::WorkspaceAuthority> lastExportAuthority_;
    std::optional<Contracts::AuthorizedToolCall> lastExportAuthorization_;
    std::optional<Domain::DestructiveConfirmation> lastResetConfirmation_;
    std::size_t closeCount_{};
};

class ProjectMemoryRepositoryFactoryFake final
    : public Contracts::IProjectMemoryRepositoryFactory {
public:
    ProjectMemoryRepositoryFactoryFake(
        const std::size_t maximumKnownRepositories,
        const std::size_t maximumOpenRepositories,
        const Domain::MonotonicTimePoint now = {}) noexcept
        : maximumKnownRepositories_{maximumKnownRepositories},
          maximumOpenRepositories_{maximumOpenRepositories},
          gate_{now}
    {
    }

    [[nodiscard]] Domain::Result<void> addRepository(
        std::shared_ptr<Contracts::IProjectMemoryRepository> repository) noexcept
    {
        try {
            if (!repository) {
                return Domain::Result<void>::failure(Domain::makeError(
                    Domain::ErrorCodes::InvalidRequest,
                    "A deterministic repository is required."));
            }
            const auto duplicate = std::find_if(
                repositories_.begin(),
                repositories_.end(),
                [&](const auto& candidate) {
                    return candidate->projectId() == repository->projectId();
                });
            if (duplicate != repositories_.end()) {
                return Domain::Result<void>::failure(Domain::makeError(
                    Domain::ErrorCodes::Conflict,
                    "The deterministic repository is already registered."));
            }
            if (repositories_.size() >= maximumKnownRepositories_) {
                return Domain::Result<void>::failure(Domain::makeError(
                    Domain::ErrorCodes::LimitExceeded,
                    "The deterministic repository catalog is at capacity."));
            }
            repositories_.push_back(std::move(repository));
            return Domain::Result<void>::success();
        } catch (...) {
            return fakeInternalFailure<void>();
        }
    }

    [[nodiscard]] Domain::Result<std::shared_ptr<Contracts::IProjectMemoryRepository>> open(
        const Domain::ProjectId& projectId,
        const Domain::OperationContext& context) noexcept override
    {
        try {
            lastProjectId_ = projectId;
            auto accepted = gate_.enter(context);
            if (!accepted) {
                return propagateFakeGateFailure<
                    std::shared_ptr<Contracts::IProjectMemoryRepository>>(
                    std::move(accepted));
            }
            const auto repository = std::find_if(
                repositories_.begin(),
                repositories_.end(),
                [&](const auto& candidate) {
                    return candidate->projectId() == projectId;
                });
            if (repository == repositories_.end()) {
                return Domain::Result<
                    std::shared_ptr<Contracts::IProjectMemoryRepository>>::failure(
                    Domain::makeError(
                        Domain::ErrorCodes::ProjectNotFound,
                        "The deterministic repository is not registered."));
            }
            const auto alreadyOpen = std::find(
                openProjectIds_.begin(), openProjectIds_.end(), projectId);
            if (alreadyOpen == openProjectIds_.end()) {
                if (openProjectIds_.size() >= maximumOpenRepositories_) {
                    return Domain::Result<
                        std::shared_ptr<Contracts::IProjectMemoryRepository>>::failure(
                        Domain::makeError(
                            Domain::ErrorCodes::LimitExceeded,
                            "The deterministic open-repository bound was reached."));
                }
                openProjectIds_.push_back(projectId);
            }
            return Domain::Result<
                std::shared_ptr<Contracts::IProjectMemoryRepository>>::success(*repository);
        } catch (...) {
            return fakeInternalFailure<
                std::shared_ptr<Contracts::IProjectMemoryRepository>>();
        }
    }

    [[nodiscard]] Domain::Result<void> close(
        const Domain::ProjectId& projectId,
        const Domain::OperationContext& context) noexcept override
    {
        try {
            lastProjectId_ = projectId;
            auto accepted = gate_.enter(context);
            if (!accepted) {
                return accepted;
            }
            openProjectIds_.erase(
                std::remove(openProjectIds_.begin(), openProjectIds_.end(), projectId),
                openProjectIds_.end());
            return Domain::Result<void>::success();
        } catch (...) {
            return fakeInternalFailure<void>();
        }
    }

    void shutdown() noexcept override
    {
        gate_.shutdown();
        openProjectIds_.clear();
    }

    [[nodiscard]] std::size_t openCount() const noexcept override
    {
        return openProjectIds_.size();
    }

    void setNow(const Domain::MonotonicTimePoint now) noexcept { gate_.setNow(now); }
    [[nodiscard]] std::size_t knownCount() const noexcept { return repositories_.size(); }

    [[nodiscard]] const std::optional<Domain::ProjectId>&
    lastProjectId() const noexcept
    {
        return lastProjectId_;
    }

private:
    const std::size_t maximumKnownRepositories_;
    const std::size_t maximumOpenRepositories_;
    std::vector<std::shared_ptr<Contracts::IProjectMemoryRepository>> repositories_;
    std::vector<Domain::ProjectId> openProjectIds_;
    std::optional<Domain::ProjectId> lastProjectId_;
    BoundedFakeOperationGate gate_;
};

enum class LegacyMemoryCall { None, Set, Get, List, Remove, Search, Purge };

class LegacyMemoryServiceFake final : public Contracts::ILegacyMemoryService {
public:
    LegacyMemoryServiceFake(
        const std::size_t maximumNotes,
        Domain::DestructiveConfirmation expectedPurgeConfirmation,
        const Domain::MonotonicTimePoint now = {},
        std::shared_ptr<const Contracts::IUnicodeCanonicalizer>
            unicodeCanonicalizer =
                std::make_shared<UnicodeCanonicalizerFake>())
        : maximumNotes_{maximumNotes},
          expectedPurgeConfirmation_{std::move(expectedPurgeConfirmation)},
          unicodeCanonicalizer_{
              unicodeCanonicalizer
                  ? std::move(unicodeCanonicalizer)
                  : std::make_shared<UnicodeCanonicalizerFake>()},
          gate_{now}
    {
    }

    [[nodiscard]] Domain::Result<Domain::LegacyMemorySetOutcome> set(
        const Domain::LegacyMemorySetRequest& request,
        const Domain::OperationContext& context) noexcept override
    {
        try {
            lastCall_ = LegacyMemoryCall::Set;
            auto accepted = gate_.enter(context);
            if (!accepted) {
                return propagateFakeGateFailure<Domain::LegacyMemorySetOutcome>(
                    std::move(accepted));
            }
            auto normalized = normalizeSetRequest(request);
            if (!normalized) {
                return Domain::Result<Domain::LegacyMemorySetOutcome>::failure(
                    std::move(normalized).error());
            }
            lastUpsert_ = normalized.value();
            Domain::MemoryNote note{
                normalized.value().key,
                normalized.value().body,
                normalized.value().tags,
                Domain::UtcTimePoint{},
                Domain::UtcTimePoint{}};
            const auto existing = std::find_if(
                notes_.begin(), notes_.end(), [&](const Domain::MemoryNote& candidate) {
                    return candidate.key == note.key;
                });
            if (existing != notes_.end()) {
                *existing = note;
            } else {
                if (notes_.size() >= maximumNotes_) {
                    return Domain::Result<Domain::LegacyMemorySetOutcome>::failure(
                        Domain::makeError(
                            Domain::ErrorCodes::LimitExceeded,
                            "The deterministic legacy-memory bound was reached."));
                }
                notes_.push_back(note);
            }
            return Domain::Result<Domain::LegacyMemorySetOutcome>::success(
                Domain::LegacyMemorySetOutcome{std::move(note), true});
        } catch (...) {
            return fakeInternalFailure<Domain::LegacyMemorySetOutcome>();
        }
    }

    [[nodiscard]] Domain::Result<Domain::LegacyMemoryGetOutcome> get(
        const Domain::LegacyMemoryGetRequest& request,
        const Domain::OperationContext& context) noexcept override
    {
        try {
            lastCall_ = LegacyMemoryCall::Get;
            auto accepted = gate_.enter(context);
            if (!accepted) {
                return propagateFakeGateFailure<Domain::LegacyMemoryGetOutcome>(
                    std::move(accepted));
            }
            auto key = Domain::normalizeLegacyMemoryGetRequest(request);
            if (!key) {
                return Domain::Result<Domain::LegacyMemoryGetOutcome>::failure(
                    std::move(key).error());
            }
            lastText_ = key.value();
            const auto match = std::find_if(
                notes_.begin(), notes_.end(), [&](const Domain::MemoryNote& candidate) {
                    return candidate.key == key.value();
                });
            return Domain::Result<Domain::LegacyMemoryGetOutcome>::success(
                Domain::LegacyMemoryGetOutcome{
                    key.value(),
                    match == notes_.end()
                        ? std::optional<Domain::MemoryNote>{}
                        : std::optional<Domain::MemoryNote>{*match}});
        } catch (...) {
            return fakeInternalFailure<Domain::LegacyMemoryGetOutcome>();
        }
    }

    [[nodiscard]] Domain::Result<Domain::LegacyMemoryListOutcome> list(
        const Domain::LegacyMemoryListRequest& request,
        const Domain::OperationContext& context) noexcept override
    {
        try {
            lastCall_ = LegacyMemoryCall::List;
            auto accepted = gate_.enter(context);
            if (!accepted) {
                return propagateFakeGateFailure<Domain::LegacyMemoryListOutcome>(
                    std::move(accepted));
            }
            auto query = Domain::normalizeLegacyMemoryListRequest(request);
            if (!query) {
                return Domain::Result<Domain::LegacyMemoryListOutcome>::failure(
                    std::move(query).error());
            }
            lastLimit_ = query.value().limit;
            lastIncludeSystem_ = query.value().includeSystem;
            std::size_t visibleTotal{};
            std::size_t selectedCandidates{};
            std::vector<Domain::LegacyMemoryNoteProjection> output;
            output.reserve(std::min(query.value().limit, notes_.size()));
            for (const auto& note : notes_) {
                if (!query.value().includeSystem &&
                    isSqlHiddenSystemKey(note.key)) {
                    continue;
                }
                ++visibleTotal;
                if ((query.value().prefix &&
                     !asciiPrefix(note.key, *query.value().prefix)) ||
                    selectedCandidates >= query.value().limit) {
                    continue;
                }
                ++selectedCandidates;
                if (query.value().tag) {
                    auto contains = containsCanonicalTag(
                        note.tags, *query.value().tag);
                    if (!contains) {
                        return Domain::Result<
                            Domain::LegacyMemoryListOutcome>::failure(
                                std::move(contains).error());
                    }
                    if (!contains.value()) {
                        continue;
                    }
                }
                output.push_back(project(note, query.value().includeBody));
            }
            return Domain::Result<Domain::LegacyMemoryListOutcome>::success(
                Domain::LegacyMemoryListOutcome{std::move(output), visibleTotal});
        } catch (...) {
            return fakeInternalFailure<Domain::LegacyMemoryListOutcome>();
        }
    }

    [[nodiscard]] Domain::Result<Domain::LegacyMemoryDeleteOutcome> remove(
        const Domain::LegacyMemoryRemoveRequest& request,
        const Domain::OperationContext& context) noexcept override
    {
        try {
            lastCall_ = LegacyMemoryCall::Remove;
            auto accepted = gate_.enter(context);
            if (!accepted) {
                return propagateFakeGateFailure<Domain::LegacyMemoryDeleteOutcome>(
                    std::move(accepted));
            }
            auto key = Domain::normalizeLegacyMemoryRemoveRequest(request);
            if (!key) {
                return Domain::Result<Domain::LegacyMemoryDeleteOutcome>::failure(
                    std::move(key).error());
            }
            lastText_ = key.value();
            const auto original = notes_.size();
            notes_.erase(
                std::remove_if(
                    notes_.begin(), notes_.end(), [&](const Domain::MemoryNote& note) {
                        return note.key == key.value();
                    }),
                notes_.end());
            const bool deleted = notes_.size() != original;
            return Domain::Result<Domain::LegacyMemoryDeleteOutcome>::success(
                Domain::LegacyMemoryDeleteOutcome{
                    key.value(),
                    deleted,
                    deleted,
                    Domain::isSystemMemoryKey(key.value())});
        } catch (...) {
            return fakeInternalFailure<Domain::LegacyMemoryDeleteOutcome>();
        }
    }

    [[nodiscard]] Domain::Result<Domain::LegacyMemorySearchOutcome> search(
        const Domain::LegacyMemorySearchRequest& request,
        const Domain::OperationContext& context) noexcept override
    {
        try {
            lastCall_ = LegacyMemoryCall::Search;
            auto accepted = gate_.enter(context);
            if (!accepted) {
                return propagateFakeGateFailure<Domain::LegacyMemorySearchOutcome>(
                    std::move(accepted));
            }
            auto query = Domain::normalizeLegacyMemorySearchRequest(request);
            if (!query) {
                return Domain::Result<Domain::LegacyMemorySearchOutcome>::failure(
                    std::move(query).error());
            }
            lastText_ = query.value().query;
            lastLimit_ = query.value().limit;
            std::vector<Domain::LegacyMemoryNoteProjection> output;
            output.reserve(std::min(query.value().limit, notes_.size()));
            for (const auto& note : notes_) {
                if (output.size() >= query.value().limit) {
                    break;
                }
                if (!query.value().includeSystem &&
                    isSqlHiddenSystemKey(note.key)) {
                    continue;
                }
                bool matches = asciiContains(note.key, query.value().query) ||
                               asciiContains(note.body, query.value().query);
                for (const auto& tag : note.tags) {
                    matches = matches || asciiContains(tag, query.value().query);
                }
                if (matches) {
                    output.push_back(project(note, query.value().includeBody));
                }
            }
            return Domain::Result<Domain::LegacyMemorySearchOutcome>::success(
                Domain::LegacyMemorySearchOutcome{
                    query.value().query, std::move(output)});
        } catch (...) {
            return fakeInternalFailure<Domain::LegacyMemorySearchOutcome>();
        }
    }

    [[nodiscard]] Domain::Result<Domain::LegacyMemoryPurgeOutcome> purge(
        const Domain::DestructiveConfirmation& confirmation,
        const Domain::OperationContext& context) noexcept override
    {
        try {
            lastCall_ = LegacyMemoryCall::Purge;
            lastPurgeConfirmation_ = confirmation;
            auto accepted = gate_.enter(context);
            if (!accepted) {
                return propagateFakeGateFailure<Domain::LegacyMemoryPurgeOutcome>(
                    std::move(accepted));
            }
            auto valid = Domain::validateDestructiveConfirmation(
                confirmation,
                expectedPurgeConfirmation_.action,
                expectedPurgeConfirmation_.scope,
                expectedPurgeConfirmation_.token);
            if (!valid) {
                return Domain::Result<Domain::LegacyMemoryPurgeOutcome>::failure(
                    std::move(valid).error());
            }
            const auto removed = notes_.size();
            notes_.clear();
            return Domain::Result<Domain::LegacyMemoryPurgeOutcome>::success(
                Domain::LegacyMemoryPurgeOutcome{removed, true});
        } catch (...) {
            return fakeInternalFailure<Domain::LegacyMemoryPurgeOutcome>();
        }
    }

    [[nodiscard]] Domain::Result<void> quickCheck(
        const Domain::OperationContext& context) noexcept override
    {
        return gate_.enter(context);
    }

    void shutdown() noexcept override { gate_.shutdown(); }
    void setNow(const Domain::MonotonicTimePoint now) noexcept { gate_.setNow(now); }
    [[nodiscard]] std::size_t noteCount() const noexcept { return notes_.size(); }
    [[nodiscard]] LegacyMemoryCall lastCall() const noexcept { return lastCall_; }

    [[nodiscard]] const std::optional<Domain::DestructiveConfirmation>&
    lastPurgeConfirmation() const noexcept
    {
        return lastPurgeConfirmation_;
    }

private:
    [[nodiscard]] Domain::Result<Domain::LegacyMemoryUpsert>
    normalizeSetRequest(const Domain::LegacyMemorySetRequest& request) const
    {
        auto key = Domain::normalizeLegacyMemoryKey(request.key);
        if (!key) {
            return Domain::Result<Domain::LegacyMemoryUpsert>::failure(
                std::move(key).error());
        }
        auto body = Domain::normalizeLegacyMemoryBody(request.body);
        if (!body) {
            return Domain::Result<Domain::LegacyMemoryUpsert>::failure(
                std::move(body).error());
        }
        auto prepared = Domain::prepareLegacyMemoryTags(request.tags);
        if (!prepared) {
            return Domain::Result<Domain::LegacyMemoryUpsert>::failure(
                std::move(prepared).error());
        }

        try {
            std::map<Contracts::NfcUtf8Key, std::string> unique;
            for (auto& tag : prepared.value()) {
                auto canonical = unicodeCanonicalizer_->nfcKey(tag);
                if (!canonical) {
                    return Domain::Result<
                        Domain::LegacyMemoryUpsert>::failure(
                            std::move(canonical).error());
                }
                unique.try_emplace(
                    std::move(canonical).value(), std::move(tag));
            }
            std::vector<std::string> tags;
            tags.reserve(unique.size());
            for (auto& [canonical, original] : unique) {
                static_cast<void>(canonical);
                tags.push_back(std::move(original));
            }
            return Domain::Result<Domain::LegacyMemoryUpsert>::success(
                Domain::LegacyMemoryUpsert{
                    std::move(key).value(),
                    std::move(body).value(),
                    std::move(tags)});
        } catch (...) {
            return fakeInternalFailure<Domain::LegacyMemoryUpsert>();
        }
    }

    [[nodiscard]] Domain::Result<bool> containsCanonicalTag(
        const std::vector<std::string>& tags,
        const std::string_view candidate) const
    {
        auto candidateKey = unicodeCanonicalizer_->nfcKey(candidate);
        if (!candidateKey) {
            return Domain::Result<bool>::failure(
                std::move(candidateKey).error());
        }
        for (const auto& tag : tags) {
            auto tagKey = unicodeCanonicalizer_->nfcKey(tag);
            if (!tagKey) {
                return Domain::Result<bool>::failure(
                    std::move(tagKey).error());
            }
            if (tagKey.value() == candidateKey.value()) {
                return Domain::Result<bool>::success(true);
            }
        }
        return Domain::Result<bool>::success(false);
    }

    [[nodiscard]] static unsigned char foldAscii(unsigned char value) noexcept
    {
        if (value >= static_cast<unsigned char>('A') &&
            value <= static_cast<unsigned char>('Z')) {
            return static_cast<unsigned char>(value + ('a' - 'A'));
        }
        return value;
    }

    [[nodiscard]] static bool asciiPrefix(
        const std::string_view value,
        const std::string_view prefix) noexcept
    {
        if (prefix.size() > value.size()) {
            return false;
        }
        for (std::size_t index{}; index < prefix.size(); ++index) {
            if (foldAscii(static_cast<unsigned char>(value[index])) !=
                foldAscii(static_cast<unsigned char>(prefix[index]))) {
                return false;
            }
        }
        return true;
    }

    [[nodiscard]] static bool isSqlHiddenSystemKey(
        const std::string_view key) noexcept
    {
        return asciiPrefix(key, "agent_run/") ||
               asciiPrefix(key, "agent_active/") ||
               asciiPrefix(key, "continuity/");
    }

    [[nodiscard]] static bool asciiContains(
        const std::string_view value,
        const std::string_view query) noexcept
    {
        if (query.empty()) {
            return true;
        }
        if (query.size() > value.size()) {
            return false;
        }
        for (std::size_t start{}; start + query.size() <= value.size(); ++start) {
            if (asciiPrefix(value.substr(start), query)) {
                return true;
            }
        }
        return false;
    }

    [[nodiscard]] static Domain::LegacyMemoryNoteProjection project(
        const Domain::MemoryNote& note,
        const bool includeBody)
    {
        return Domain::LegacyMemoryNoteProjection{
            note.key,
            includeBody ? std::optional<std::string>{note.body} : std::nullopt,
            note.body.size(),
            note.tags,
            note.createdAt,
            note.updatedAt};
    }

    const std::size_t maximumNotes_;
    Domain::DestructiveConfirmation expectedPurgeConfirmation_;
    std::shared_ptr<const Contracts::IUnicodeCanonicalizer>
        unicodeCanonicalizer_;
    std::vector<Domain::MemoryNote> notes_;
    BoundedFakeOperationGate gate_;
    LegacyMemoryCall lastCall_{LegacyMemoryCall::None};
    std::optional<Domain::LegacyMemoryUpsert> lastUpsert_;
    std::optional<std::string> lastText_;
    std::optional<std::size_t> lastLimit_;
    std::optional<bool> lastIncludeSystem_;
    std::optional<Domain::DestructiveConfirmation> lastPurgeConfirmation_;
};

} // namespace ForgeConductor::Tests::Fakes
