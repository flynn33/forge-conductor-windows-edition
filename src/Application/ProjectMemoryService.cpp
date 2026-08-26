#include "ForgeConductor/Application/ProjectMemoryService.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cctype>
#include <limits>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ForgeConductor::Application {
namespace {

constexpr std::size_t MaximumRegistryProjects = 1'024U;
constexpr std::string_view ResetProjectAction = "reset_project_memory";
constexpr std::string_view ResetProjectTokenPrefix = "RESET PROJECT MEMORY ";
constexpr std::string_view ResetAllAction = "reset_all_project_memory";
constexpr std::string_view ResetAllScope = "all-projects";
constexpr std::string_view ResetAllToken = "RESET ALL PROJECT MEMORY";

template <typename T>
[[nodiscard]] Domain::Result<T> internalFailure(const std::string_view message)
{
    return Domain::Result<T>::failure(
        Domain::makeError(Domain::ErrorCodes::InternalFailure, std::string{message}));
}

template <typename T>
[[nodiscard]] Domain::Result<T> scopeMismatch()
{
    return Domain::Result<T>::failure(Domain::makeError(
        Domain::ErrorCodes::ProjectScopeMismatch,
        "A project-memory dependency returned data for a different project."));
}

template <typename T, typename U>
[[nodiscard]] Domain::Result<T> propagateFailure(Domain::Result<U>&& source)
{
    return Domain::Result<T>::failure(std::move(source).error());
}

[[nodiscard]] Domain::Result<void> validateContext(
    const Domain::OperationContext& context) noexcept
{
    try {
        const auto now = std::chrono::steady_clock::now();
        if (context.isCancellationRequested()) {
            return Domain::Result<void>::failure(Domain::makeError(
                Domain::ErrorCodes::Cancelled,
                "The project-memory operation was cancelled before admission."));
        }
        if (context.isExpired(now)) {
            return Domain::Result<void>::failure(Domain::makeError(
                Domain::ErrorCodes::DeadlineExceeded,
                "The project-memory operation deadline expired before admission."));
        }
        if (context.deadline > now + Domain::MaximumProjectMemoryDeadline) {
            return Domain::Result<void>::failure(Domain::makeError(
                Domain::ErrorCodes::InvalidRequest,
                "The project-memory operation deadline exceeds 60000 milliseconds."));
        }
        return Domain::Result<void>::success();
    } catch (...) {
        return Domain::Result<void>::failure(Domain::makeError(
            Domain::ErrorCodes::InternalFailure,
            "The project-memory operation context could not be validated."));
    }
}

[[nodiscard]] std::string trimAscii(std::string value)
{
    const auto whitespace = [](const unsigned char character) {
        return std::isspace(character) != 0;
    };
    const auto first = std::find_if_not(value.begin(), value.end(), whitespace);
    const auto last = std::find_if_not(value.rbegin(), value.rend(), whitespace).base();
    if (first >= last) {
        return {};
    }
    return std::string{first, last};
}

[[nodiscard]] std::string normalizeIdentifier(std::string value)
{
    value = trimAscii(std::move(value));
    std::transform(
        value.begin(),
        value.end(),
        value.begin(),
        [](const unsigned char character) {
            return static_cast<char>(std::tolower(character));
        });
    return value;
}

[[nodiscard]] bool matchesProject(
    const Domain::MemoryWriteOutcome& outcome,
    const Domain::ProjectId& projectId) noexcept
{
    return outcome.projectId == projectId;
}

[[nodiscard]] bool matchesProject(
    const Domain::MemoryBatchOutcome& outcome,
    const Domain::ProjectId& projectId) noexcept
{
    if (outcome.projectId != projectId) {
        return false;
    }
    return std::all_of(
        outcome.results.begin(),
        outcome.results.end(),
        [&](const Domain::MemoryWriteOutcome& item) {
            return matchesProject(item, projectId);
        });
}

[[nodiscard]] bool matchesProject(
    const Domain::ProjectMemoryRecord& record,
    const Domain::ProjectId& projectId) noexcept
{
    return record.projectId == projectId;
}

[[nodiscard]] bool matchesProject(
    const Domain::MemoryPage& page,
    const Domain::ProjectId& projectId) noexcept
{
    if (page.projectId != projectId) {
        return false;
    }
    return std::all_of(
        page.records.begin(),
        page.records.end(),
        [&](const Domain::MemorySearchHit& hit) {
            return matchesProject(hit.record, projectId);
        });
}

[[nodiscard]] bool matchesProject(
    const Domain::MemoryRecords& records,
    const Domain::ProjectId& projectId) noexcept
{
    if (records.projectId != projectId) {
        return false;
    }
    return std::all_of(
        records.records.begin(),
        records.records.end(),
        [&](const Domain::ProjectMemoryRecord& record) {
            return matchesProject(record, projectId);
        });
}

[[nodiscard]] bool matchesProject(
    const Domain::ForgetOutcome& outcome,
    const Domain::ProjectId& projectId) noexcept
{
    return outcome.projectId == projectId;
}

[[nodiscard]] bool matchesProject(
    const Domain::LinkOutcome& outcome,
    const Domain::ProjectId& projectId) noexcept
{
    return outcome.projectId == projectId;
}

[[nodiscard]] bool matchesProject(
    const Domain::ProjectMemoryExport& outcome,
    const Domain::ProjectId& projectId) noexcept
{
    return outcome.projectId == projectId;
}

[[nodiscard]] bool matchesProject(
    const Domain::ProjectMemoryImport& outcome,
    const Domain::ProjectId& projectId) noexcept
{
    if (outcome.projectId != projectId) {
        return false;
    }
    return std::all_of(
        outcome.imported.begin(),
        outcome.imported.end(),
        [&](const Domain::MemoryWriteOutcome& item) {
            return matchesProject(item, projectId);
        });
}

[[nodiscard]] bool matchesProject(
    const Domain::ProjectMemoryStatus& status,
    const Domain::ProjectId& projectId) noexcept
{
    return status.projectId == projectId;
}

template <typename T>
[[nodiscard]] Domain::Result<T> enforceProjectScope(
    Domain::Result<T>&& result,
    const Domain::ProjectId& projectId)
{
    if (!result) {
        return Domain::Result<T>::failure(std::move(result).error());
    }
    auto value = std::move(result).value();
    if (!matchesProject(value, projectId)) {
        return scopeMismatch<T>();
    }
    return Domain::Result<T>::success(std::move(value));
}

[[nodiscard]] bool addWithoutOverflow(
    std::size_t& aggregate,
    const std::size_t value) noexcept
{
    if (value > std::numeric_limits<std::size_t>::max() - aggregate) {
        return false;
    }
    aggregate += value;
    return true;
}

[[nodiscard]] Domain::Error annotateCommittedProjects(
    Domain::Error error,
    const std::size_t committedProjects)
{
    if (committedProjects != 0U) {
        error.message += " " + std::to_string(committedProjects) +
                         " earlier project reset(s) remain committed; the "
                         "all-project reset is not cross-project atomic.";
    }
    return error;
}

} // namespace

class ProjectMemoryService::Impl final {
public:
    Impl(
        Contracts::IProjectRegistryRepository& registry,
        Contracts::IProjectMemoryRepositoryFactory& repositoryFactory,
        Contracts::IRedactor& redactor,
        Domain::ProjectMemoryLimits limits) noexcept
        : registry_{registry},
          repositoryFactory_{repositoryFactory},
          redactor_{redactor},
          limits_{std::move(limits)}
    {
    }

    [[nodiscard]] Domain::Result<Domain::ProjectInitialization> initialize(
        const Domain::InitializeProjectRequest& request,
        const Domain::OperationContext& context) noexcept
    {
        return execute<Domain::ProjectInitialization>(context, [&]() {
            auto sanitized = redactInitializeRequest(request);
            if (!sanitized) {
                return propagateFailure<Domain::ProjectInitialization>(
                    std::move(sanitized));
            }

            auto currentContext = validateContext(context);
            if (!currentContext) {
                return propagateFailure<Domain::ProjectInitialization>(
                    std::move(currentContext));
            }

            auto initialized = registry_.initialize(sanitized.value(), context);
            if (!initialized) {
                return initialized;
            }
            auto value = std::move(initialized).value();
            if (sanitized.value().requestedProjectId &&
                sanitized.value().requestedProjectId.value() != value.project.id) {
                return scopeMismatch<Domain::ProjectInitialization>();
            }

            auto scoped = openScoped(value.project.id, context);
            if (!scoped) {
                return propagateFailure<Domain::ProjectInitialization>(
                    std::move(scoped));
            }
            auto repository = std::move(scoped).value();
            value.project = repository.descriptor;

            auto repositoryStatus = enforceProjectScope(
                repository.repository->status(
                    Domain::ProjectMemoryStatusRequest{value.project.id}, context),
                value.project.id);
            if (!repositoryStatus) {
                return propagateFailure<Domain::ProjectInitialization>(
                    std::move(repositoryStatus));
            }
            const auto& status = repositoryStatus.value();
            if (!status.integrityOk) {
                return Domain::Result<Domain::ProjectInitialization>::failure(
                    Domain::makeError(
                        Domain::ErrorCodes::IntegrityFailure,
                        "The initialized project-memory repository did not pass its "
                        "integrity check."));
            }
            if (status.schemaVersion != Domain::ProjectMemorySchemaVersion ||
                status.capabilityVersion != Domain::ProjectMemoryCapabilityVersion) {
                return Domain::Result<Domain::ProjectInitialization>::failure(
                    Domain::makeError(
                        Domain::ErrorCodes::UnsupportedVersion,
                        "The initialized project-memory repository has an unsupported "
                        "schema or capability version."));
            }

            value.schemaVersion = status.schemaVersion;
            value.capabilityVersion = status.capabilityVersion;
            value.limits = limits_;
            value.lexicalSearchAvailable = true;
            value.fullTextSearchAvailable = status.fullTextSearchAvailable;
            value.migrationCurrent = true;
            return Domain::Result<Domain::ProjectInitialization>::success(
                std::move(value));
        });
    }

    [[nodiscard]] Domain::Result<Domain::MemoryWriteOutcome> remember(
        const Domain::RememberProjectMemoryRequest& request,
        const Domain::OperationContext& context) noexcept
    {
        return execute<Domain::MemoryWriteOutcome>(context, [&]() {
            auto redacted = redactWrite(request.write);
            if (!redacted) {
                return propagateFailure<Domain::MemoryWriteOutcome>(
                    std::move(redacted));
            }
            auto validated = Domain::validateProjectMemoryWrite(
                std::move(redacted).value(), limits_);
            if (!validated) {
                return propagateFailure<Domain::MemoryWriteOutcome>(
                    std::move(validated));
            }
            const Domain::RememberProjectMemoryRequest normalized{
                request.projectId, std::move(validated).value()};
            auto scoped = openScoped(request.projectId, context);
            if (!scoped) {
                return propagateFailure<Domain::MemoryWriteOutcome>(
                    std::move(scoped));
            }
            auto repository = std::move(scoped).value();
            return enforceProjectScope(
                repository.repository->remember(normalized, context),
                request.projectId);
        });
    }

    [[nodiscard]] Domain::Result<Domain::MemoryBatchOutcome> rememberBatch(
        const Domain::RememberProjectMemoryBatchRequest& request,
        const Domain::OperationContext& context) noexcept
    {
        return execute<Domain::MemoryBatchOutcome>(context, [&]() {
            if (request.writes.empty() || request.writes.size() > limits_.maximumBatchCount) {
                return Domain::Result<Domain::MemoryBatchOutcome>::failure(
                    Domain::makeError(
                        Domain::ErrorCodes::PayloadTooLarge,
                        "Project memory batch count is outside its configured bound."));
            }

            std::vector<Domain::ProjectMemoryWrite> redacted;
            redacted.reserve(request.writes.size());
            for (const auto& write : request.writes) {
                auto item = redactWrite(write);
                if (!item) {
                    return propagateFailure<Domain::MemoryBatchOutcome>(
                        std::move(item));
                }
                redacted.push_back(std::move(item).value());
            }

            auto validated = Domain::validateProjectMemoryBatch(
                std::move(redacted), limits_);
            if (!validated) {
                return propagateFailure<Domain::MemoryBatchOutcome>(
                    std::move(validated));
            }
            const Domain::RememberProjectMemoryBatchRequest normalized{
                request.projectId, std::move(validated).value()};
            auto scoped = openScoped(request.projectId, context);
            if (!scoped) {
                return propagateFailure<Domain::MemoryBatchOutcome>(
                    std::move(scoped));
            }
            auto repository = std::move(scoped).value();
            return enforceProjectScope(
                repository.repository->rememberBatch(normalized, context),
                request.projectId);
        });
    }

    [[nodiscard]] Domain::Result<Domain::MemoryPage> search(
        const Domain::SearchProjectMemoryRequest& request,
        const Domain::OperationContext& context) noexcept
    {
        return execute<Domain::MemoryPage>(context, [&]() {
            auto validated = Domain::validateSearchProjectMemoryRequest(
                request, limits_);
            if (!validated) {
                return propagateFailure<Domain::MemoryPage>(std::move(validated));
            }
            auto scoped = openScoped(request.projectId, context);
            if (!scoped) {
                return propagateFailure<Domain::MemoryPage>(std::move(scoped));
            }
            auto repository = std::move(scoped).value();
            return enforceProjectScope(
                repository.repository->search(validated.value(), context),
                request.projectId);
        });
    }

    [[nodiscard]] Domain::Result<Domain::MemoryRecords> get(
        const Domain::GetProjectMemoryRequest& request,
        const Domain::OperationContext& context) noexcept
    {
        return execute<Domain::MemoryRecords>(context, [&]() {
            auto validated = Domain::validateGetProjectMemoryRequest(request, limits_);
            if (!validated) {
                return propagateFailure<Domain::MemoryRecords>(
                    std::move(validated));
            }
            auto scoped = openScoped(request.projectId, context);
            if (!scoped) {
                return propagateFailure<Domain::MemoryRecords>(std::move(scoped));
            }
            auto repository = std::move(scoped).value();
            return enforceProjectScope(
                repository.repository->get(request, context), request.projectId);
        });
    }

    [[nodiscard]] Domain::Result<Domain::ProjectMemoryRecord> update(
        const Domain::UpdateProjectMemoryRequest& request,
        const Domain::OperationContext& context) noexcept
    {
        return execute<Domain::ProjectMemoryRecord>(context, [&]() {
            auto redacted = redactUpdateRequest(request);
            if (!redacted) {
                return propagateFailure<Domain::ProjectMemoryRecord>(
                    std::move(redacted));
            }
            auto validated = Domain::validateUpdateProjectMemoryRequest(
                std::move(redacted).value(), limits_);
            if (!validated) {
                return propagateFailure<Domain::ProjectMemoryRecord>(
                    std::move(validated));
            }
            auto scoped = openScoped(request.projectId, context);
            if (!scoped) {
                return propagateFailure<Domain::ProjectMemoryRecord>(
                    std::move(scoped));
            }
            auto repository = std::move(scoped).value();
            return enforceProjectScope(
                repository.repository->update(validated.value(), context),
                request.projectId);
        });
    }

    [[nodiscard]] Domain::Result<Domain::ForgetOutcome> forget(
        const Domain::ForgetProjectMemoryRequest& request,
        const Domain::OperationContext& context) noexcept
    {
        return execute<Domain::ForgetOutcome>(context, [&]() {
            auto scoped = openScoped(request.projectId, context);
            if (!scoped) {
                return propagateFailure<Domain::ForgetOutcome>(std::move(scoped));
            }
            auto repository = std::move(scoped).value();
            return enforceProjectScope(
                repository.repository->forget(request, context), request.projectId);
        });
    }

    [[nodiscard]] Domain::Result<Domain::MemoryPage> listRecent(
        const Domain::ListRecentProjectMemoryRequest& request,
        const Domain::OperationContext& context) noexcept
    {
        return execute<Domain::MemoryPage>(context, [&]() {
            auto validated = Domain::validateListRecentProjectMemoryRequest(
                request, limits_);
            if (!validated) {
                return propagateFailure<Domain::MemoryPage>(std::move(validated));
            }
            auto scoped = openScoped(request.projectId, context);
            if (!scoped) {
                return propagateFailure<Domain::MemoryPage>(std::move(scoped));
            }
            auto repository = std::move(scoped).value();
            return enforceProjectScope(
                repository.repository->listRecent(validated.value(), context),
                request.projectId);
        });
    }

    [[nodiscard]] Domain::Result<Domain::LinkOutcome> link(
        const Domain::LinkProjectMemoryRequest& request,
        const Domain::OperationContext& context) noexcept
    {
        return execute<Domain::LinkOutcome>(context, [&]() {
            auto redacted = redactLinkRequest(request);
            if (!redacted) {
                return propagateFailure<Domain::LinkOutcome>(std::move(redacted));
            }
            auto validated = Domain::validateLinkProjectMemoryRequest(
                std::move(redacted).value());
            if (!validated) {
                return propagateFailure<Domain::LinkOutcome>(std::move(validated));
            }
            auto scoped = openScoped(request.projectId, context);
            if (!scoped) {
                return propagateFailure<Domain::LinkOutcome>(std::move(scoped));
            }
            auto repository = std::move(scoped).value();
            return enforceProjectScope(
                repository.repository->link(validated.value(), context),
                request.projectId);
        });
    }

    [[nodiscard]] Domain::Result<Domain::ProjectMemoryExport> exportMemory(
        const Domain::ExportProjectMemoryRequest& request,
        const Contracts::WorkspaceAuthority& writeAuthority,
        const Contracts::AuthorizedToolCall& authorization,
        const Domain::OperationContext& context) noexcept
    {
        return execute<Domain::ProjectMemoryExport>(context, [&]() {
            auto scoped = openScoped(request.projectId, context);
            if (!scoped) {
                return propagateFailure<Domain::ProjectMemoryExport>(
                    std::move(scoped));
            }
            auto repository = std::move(scoped).value();
            return enforceProjectScope(
                repository.repository->exportMemory(
                    request, writeAuthority, authorization, context),
                request.projectId);
        });
    }

    [[nodiscard]] Domain::Result<Domain::ProjectMemoryImport> importMemory(
        const Domain::ImportProjectMemoryRequest& request,
        const Domain::OperationContext& context) noexcept
    {
        return execute<Domain::ProjectMemoryImport>(context, [&]() {
            auto scoped = openScoped(request.projectId, context);
            if (!scoped) {
                return propagateFailure<Domain::ProjectMemoryImport>(
                    std::move(scoped));
            }
            auto repository = std::move(scoped).value();
            return enforceProjectScope(
                repository.repository->importMemory(request, context),
                request.projectId);
        });
    }

    [[nodiscard]] Domain::Result<Domain::ProjectMemoryStatus> status(
        const Domain::ProjectMemoryStatusRequest& request,
        const Domain::OperationContext& context) noexcept
    {
        return execute<Domain::ProjectMemoryStatus>(context, [&]() {
            auto validated = Domain::validateProjectMemoryStatusRequest(request);
            if (!validated) {
                return propagateFailure<Domain::ProjectMemoryStatus>(
                    std::move(validated));
            }
            auto scoped = openScoped(request.projectId, context);
            if (!scoped) {
                return propagateFailure<Domain::ProjectMemoryStatus>(
                    std::move(scoped));
            }
            auto repository = std::move(scoped).value();
            auto result = enforceProjectScope(
                repository.repository->status(request, context), request.projectId);
            if (!result) {
                return result;
            }
            auto value = std::move(result).value();
            value.openRepositories = repositoryFactory_.openCount();
            value.limits = limits_;
            return Domain::Result<Domain::ProjectMemoryStatus>::success(
                std::move(value));
        });
    }

    [[nodiscard]] Domain::Result<void> closeProject(
        const Domain::ProjectId& projectId,
        const Domain::OperationContext& context) noexcept
    {
        return execute<void>(context, [&]() {
            auto descriptor = registeredDescriptor(projectId, context);
            if (!descriptor) {
                return propagateFailure<void>(std::move(descriptor));
            }
            return repositoryFactory_.close(projectId, context);
        });
    }

    [[nodiscard]] Domain::Result<Domain::ResetReport> resetProjectMemory(
        const Domain::ProjectId& projectId,
        const Domain::DestructiveConfirmation& confirmation,
        const Domain::OperationContext& context) noexcept
    {
        return execute<Domain::ResetReport>(context, [&]() {
            const auto expectedToken =
                std::string{ResetProjectTokenPrefix} + projectId.value();
            auto validConfirmation = Domain::validateDestructiveConfirmation(
                confirmation,
                ResetProjectAction,
                projectId.value(),
                expectedToken);
            if (!validConfirmation) {
                return propagateFailure<Domain::ResetReport>(
                    std::move(validConfirmation));
            }
            return resetOneProject(projectId, confirmation, context);
        });
    }

    [[nodiscard]] Domain::Result<Domain::ResetReport> resetAllProjectMemory(
        const Domain::DestructiveConfirmation& confirmation,
        const Domain::OperationContext& context) noexcept
    {
        return execute<Domain::ResetReport>(context, [&]() {
            auto validConfirmation = Domain::validateDestructiveConfirmation(
                confirmation,
                ResetAllAction,
                ResetAllScope,
                ResetAllToken);
            if (!validConfirmation) {
                return propagateFailure<Domain::ResetReport>(
                    std::move(validConfirmation));
            }

            auto listed = registry_.list(MaximumRegistryProjects, context);
            if (!listed) {
                return propagateFailure<Domain::ResetReport>(std::move(listed));
            }
            auto projects = std::move(listed).value();
            if (projects.size() > MaximumRegistryProjects) {
                return Domain::Result<Domain::ResetReport>::failure(
                    Domain::makeError(
                        Domain::ErrorCodes::LimitExceeded,
                        "The project registry exceeded the bounded all-project reset "
                        "capacity."));
            }
            std::sort(
                projects.begin(),
                projects.end(),
                [](const Domain::ProjectMemoryDescriptor& left,
                   const Domain::ProjectMemoryDescriptor& right) {
                    return left.id.value() < right.id.value();
                });
            for (std::size_t index = 1U; index < projects.size(); ++index) {
                if (projects[index - 1U].id == projects[index].id) {
                    return scopeMismatch<Domain::ResetReport>();
                }
            }

            Domain::ResetReport aggregate;
            aggregate.action = std::string{ResetAllAction};
            aggregate.scope = std::string{ResetAllScope};
            aggregate.verified = true;
            std::size_t completed{};

            for (const auto& project : projects) {
                auto currentContext = validateContext(context);
                if (!currentContext) {
                    return Domain::Result<Domain::ResetReport>::failure(
                        annotateCommittedProjects(
                            std::move(currentContext).error(), completed));
                }

                const Domain::DestructiveConfirmation projectConfirmation{
                    std::string{ResetProjectAction},
                    project.id.value(),
                    std::string{ResetProjectTokenPrefix} + project.id.value()};
                auto reset = resetOneProject(
                    project.id, projectConfirmation, context);
                if (!reset) {
                    return Domain::Result<Domain::ResetReport>::failure(
                        annotateCommittedProjects(std::move(reset).error(), completed));
                }
                const auto& report = reset.value();
                ++completed;
                if (!addWithoutOverflow(
                        aggregate.projectsAffected, report.projectsAffected) ||
                    !addWithoutOverflow(
                        aggregate.recordsRemoved, report.recordsRemoved) ||
                    !addWithoutOverflow(aggregate.linksRemoved, report.linksRemoved) ||
                    !addWithoutOverflow(aggregate.eventsRemoved, report.eventsRemoved)) {
                    return Domain::Result<Domain::ResetReport>::failure(
                        annotateCommittedProjects(
                            Domain::makeError(
                                Domain::ErrorCodes::LimitExceeded,
                                "The all-project reset aggregate count overflowed."),
                            completed));
                }
            }

            return Domain::Result<Domain::ResetReport>::success(
                std::move(aggregate));
        });
    }

    void shutdown() noexcept
    {
        bool shutdownLeader{};
        bool factoryShutdownCalled{};
        try {
            std::unique_lock lock{lifecycleMutex_};
            if (shutdownComplete_.load(std::memory_order_acquire)) {
                return;
            }
            if (!accepting_) {
                lifecycleChanged_.wait(lock, [&]() {
                    return shutdownComplete_.load(std::memory_order_acquire);
                });
                return;
            }

            accepting_ = false;
            shutdownLeader = true;
            lifecycleChanged_.wait(
                lock, [&]() { return activeOperations_ == 0U; });
            lock.unlock();
            repositoryFactory_.shutdown();
            factoryShutdownCalled = true;
            shutdownComplete_.store(true, std::memory_order_release);
            lifecycleChanged_.notify_all();
        } catch (...) {
            if (shutdownLeader) {
                if (!factoryShutdownCalled) {
                    repositoryFactory_.shutdown();
                }
                shutdownComplete_.store(true, std::memory_order_release);
                lifecycleChanged_.notify_all();
            }
        }
    }

private:
    class Admission final {
    public:
        explicit Admission(Impl& owner) noexcept : owner_{&owner} {}

        Admission(const Admission&) = delete;
        Admission& operator=(const Admission&) = delete;

        Admission(Admission&& other) noexcept
            : owner_{std::exchange(other.owner_, nullptr)}
        {
        }

        Admission& operator=(Admission&& other) noexcept
        {
            if (this != &other) {
                release();
                owner_ = std::exchange(other.owner_, nullptr);
            }
            return *this;
        }

        ~Admission() noexcept { release(); }

    private:
        void release() noexcept
        {
            if (owner_ != nullptr) {
                owner_->releaseOperation();
                owner_ = nullptr;
            }
        }

        Impl* owner_{};
    };

    struct ScopedRepository final {
        Domain::ProjectMemoryDescriptor descriptor;
        std::shared_ptr<Contracts::IProjectMemoryRepository> repository;
    };

    [[nodiscard]] Domain::Result<Admission> admit(
        const Domain::OperationContext& context) noexcept
    {
        try {
            auto validContext = validateContext(context);
            if (!validContext) {
                return propagateFailure<Admission>(std::move(validContext));
            }

            std::lock_guard lock{lifecycleMutex_};
            validContext = validateContext(context);
            if (!validContext) {
                return propagateFailure<Admission>(std::move(validContext));
            }
            if (!accepting_) {
                return Domain::Result<Admission>::failure(Domain::makeError(
                    Domain::ErrorCodes::Cancelled,
                    "The project-memory service is shutting down."));
            }
            if (activeOperations_ == std::numeric_limits<std::size_t>::max()) {
                return Domain::Result<Admission>::failure(Domain::makeError(
                    Domain::ErrorCodes::LimitExceeded,
                    "The project-memory service operation count is exhausted."));
            }
            ++activeOperations_;
            return Domain::Result<Admission>::success(Admission{*this});
        } catch (...) {
            return internalFailure<Admission>(
                "The project-memory operation could not be admitted.");
        }
    }

    void releaseOperation() noexcept
    {
        std::lock_guard lock{lifecycleMutex_};
        if (activeOperations_ != 0U) {
            --activeOperations_;
        }
        if (!accepting_ && activeOperations_ == 0U) {
            lifecycleChanged_.notify_all();
        }
    }

    template <typename T, typename Function>
    [[nodiscard]] Domain::Result<T> execute(
        const Domain::OperationContext& context,
        Function&& operation) noexcept
    {
        try {
            auto admitted = admit(context);
            if (!admitted) {
                return propagateFailure<T>(std::move(admitted));
            }
            [[maybe_unused]] auto admission = std::move(admitted).value();
            return std::forward<Function>(operation)();
        } catch (...) {
            return internalFailure<T>(
                "The project-memory application boundary failed internally.");
        }
    }

    [[nodiscard]] Domain::Result<void> redactText(std::string& value)
    {
        auto redacted = redactor_.redact(value);
        if (!redacted) {
            return Domain::Result<void>::failure(std::move(redacted).error());
        }
        value = std::move(redacted).value();
        return Domain::Result<void>::success();
    }

    [[nodiscard]] Domain::Result<void> redactOptionalText(
        std::optional<std::string>& value)
    {
        if (!value) {
            return Domain::Result<void>::success();
        }
        return redactText(*value);
    }

    [[nodiscard]] Domain::Result<void> redactIdempotencyKey(
        std::optional<Domain::IdempotencyKey>& key)
    {
        if (!key) {
            return Domain::Result<void>::success();
        }
        auto redacted = redactor_.redact(key->value());
        if (!redacted) {
            return Domain::Result<void>::failure(std::move(redacted).error());
        }
        auto rebuilt = Domain::IdempotencyKey::create(redacted.value());
        if (!rebuilt) {
            return Domain::Result<void>::failure(std::move(rebuilt).error());
        }
        key = std::move(rebuilt).value();
        return Domain::Result<void>::success();
    }

    [[nodiscard]] Domain::Result<Domain::InitializeProjectRequest>
    redactInitializeRequest(const Domain::InitializeProjectRequest& request)
    {
        auto value = request;
        auto result = redactOptionalText(value.displayName);
        if (!result) {
            return propagateFailure<Domain::InitializeProjectRequest>(
                std::move(result));
        }
        result = redactOptionalText(value.repositoryIdentity);
        if (!result) {
            return propagateFailure<Domain::InitializeProjectRequest>(
                std::move(result));
        }
        result = redactIdempotencyKey(value.idempotencyKey);
        if (!result) {
            return propagateFailure<Domain::InitializeProjectRequest>(
                std::move(result));
        }
        return Domain::Result<Domain::InitializeProjectRequest>::success(
            std::move(value));
    }

    [[nodiscard]] Domain::Result<Domain::ProjectMemoryWrite> redactWrite(
        const Domain::ProjectMemoryWrite& write)
    {
        auto value = write;
        value.kind = normalizeIdentifier(std::move(value.kind));
        value.sourceKind = normalizeIdentifier(std::move(value.sourceKind));

        auto normalizedTags = Domain::normalizeProjectMemoryTags(
            std::move(value.tags), limits_);
        if (!normalizedTags) {
            return propagateFailure<Domain::ProjectMemoryWrite>(
                std::move(normalizedTags));
        }
        value.tags = std::move(normalizedTags).value();

        for (auto* field : {
                 &value.kind,
                 &value.title,
                 &value.summary,
                 &value.sourceKind}) {
            auto result = redactText(*field);
            if (!result) {
                return propagateFailure<Domain::ProjectMemoryWrite>(
                    std::move(result));
            }
        }
        auto result = redactOptionalText(value.body);
        if (!result) {
            return propagateFailure<Domain::ProjectMemoryWrite>(std::move(result));
        }
        for (auto& tag : value.tags) {
            result = redactText(tag);
            if (!result) {
                return propagateFailure<Domain::ProjectMemoryWrite>(
                    std::move(result));
            }
        }
        result = redactOptionalText(value.sourceReference);
        if (!result) {
            return propagateFailure<Domain::ProjectMemoryWrite>(std::move(result));
        }
        result = redactIdempotencyKey(value.idempotencyKey);
        if (!result) {
            return propagateFailure<Domain::ProjectMemoryWrite>(std::move(result));
        }

        value.sourceKind = normalizeIdentifier(std::move(value.sourceKind));
        auto sourceKind = Domain::validateOpaqueIdentifier(value.sourceKind, 64U);
        if (!sourceKind) {
            return propagateFailure<Domain::ProjectMemoryWrite>(
                std::move(sourceKind));
        }
        value.sourceKind = std::move(sourceKind).value();
        return Domain::Result<Domain::ProjectMemoryWrite>::success(std::move(value));
    }

    [[nodiscard]] Domain::Result<Domain::UpdateProjectMemoryRequest>
    redactUpdateRequest(const Domain::UpdateProjectMemoryRequest& request)
    {
        auto value = request;
        if (value.tags) {
            auto normalized = Domain::normalizeProjectMemoryTags(
                std::move(*value.tags), limits_);
            if (!normalized) {
                return propagateFailure<Domain::UpdateProjectMemoryRequest>(
                    std::move(normalized));
            }
            value.tags = std::move(normalized).value();
        }

        for (auto* field : {&value.title, &value.summary, &value.body}) {
            auto result = redactOptionalText(*field);
            if (!result) {
                return propagateFailure<Domain::UpdateProjectMemoryRequest>(
                    std::move(result));
            }
        }
        if (value.tags) {
            for (auto& tag : *value.tags) {
                auto result = redactText(tag);
                if (!result) {
                    return propagateFailure<Domain::UpdateProjectMemoryRequest>(
                        std::move(result));
                }
            }
        }
        return Domain::Result<Domain::UpdateProjectMemoryRequest>::success(
            std::move(value));
    }

    [[nodiscard]] Domain::Result<Domain::LinkProjectMemoryRequest>
    redactLinkRequest(const Domain::LinkProjectMemoryRequest& request)
    {
        auto value = request;
        value.relation = trimAscii(std::move(value.relation));
        auto result = redactText(value.relation);
        if (!result) {
            return propagateFailure<Domain::LinkProjectMemoryRequest>(
                std::move(result));
        }
        return Domain::Result<Domain::LinkProjectMemoryRequest>::success(
            std::move(value));
    }

    [[nodiscard]] Domain::Result<Domain::ProjectMemoryDescriptor>
    registeredDescriptor(
        const Domain::ProjectId& projectId,
        const Domain::OperationContext& context)
    {
        auto currentContext = validateContext(context);
        if (!currentContext) {
            return propagateFailure<Domain::ProjectMemoryDescriptor>(
                std::move(currentContext));
        }
        auto descriptor = registry_.descriptor(projectId, context);
        if (!descriptor) {
            return descriptor;
        }
        if (descriptor.value().id != projectId) {
            return scopeMismatch<Domain::ProjectMemoryDescriptor>();
        }
        return descriptor;
    }

    [[nodiscard]] Domain::Result<ScopedRepository> openScoped(
        const Domain::ProjectId& projectId,
        const Domain::OperationContext& context)
    {
        auto descriptor = registeredDescriptor(projectId, context);
        if (!descriptor) {
            return propagateFailure<ScopedRepository>(std::move(descriptor));
        }
        auto currentContext = validateContext(context);
        if (!currentContext) {
            return propagateFailure<ScopedRepository>(std::move(currentContext));
        }
        auto opened = repositoryFactory_.open(projectId, context);
        if (!opened) {
            return propagateFailure<ScopedRepository>(std::move(opened));
        }
        auto repository = std::move(opened).value();
        if (!repository) {
            return internalFailure<ScopedRepository>(
                "The project-memory repository factory returned a null operation pin.");
        }
        if (repository->projectId() != projectId) {
            repository.reset();
            (void)repositoryFactory_.close(projectId, context);
            return scopeMismatch<ScopedRepository>();
        }
        return Domain::Result<ScopedRepository>::success(ScopedRepository{
            std::move(descriptor).value(), std::move(repository)});
    }

    [[nodiscard]] Domain::Result<Domain::ResetReport> resetOneProject(
        const Domain::ProjectId& projectId,
        const Domain::DestructiveConfirmation& confirmation,
        const Domain::OperationContext& context)
    {
        auto scoped = openScoped(projectId, context);
        if (!scoped) {
            return propagateFailure<Domain::ResetReport>(std::move(scoped));
        }
        auto owner = std::move(scoped).value();
        auto reset = owner.repository->resetMemory(confirmation, context);
        if (!reset) {
            return reset;
        }
        auto report = std::move(reset).value();
        owner.repository.reset();

        auto closed = repositoryFactory_.close(projectId, context);
        if (!closed) {
            auto error = std::move(closed).error();
            error.message +=
                " The project reset transaction committed before repository close failed.";
            return Domain::Result<Domain::ResetReport>::failure(std::move(error));
        }
        if (report.action != ResetProjectAction || report.scope != projectId.value()) {
            auto failure = scopeMismatch<Domain::ResetReport>();
            auto error = std::move(failure).error();
            error.message += " The project reset transaction was already committed.";
            return Domain::Result<Domain::ResetReport>::failure(std::move(error));
        }
        if (!report.verified || report.projectsAffected != 1U) {
            return Domain::Result<Domain::ResetReport>::failure(Domain::makeError(
                Domain::ErrorCodes::IntegrityFailure,
                "The project reset transaction committed without a verified one-project "
                "report."));
        }
        return Domain::Result<Domain::ResetReport>::success(std::move(report));
    }

    Contracts::IProjectRegistryRepository& registry_;
    Contracts::IProjectMemoryRepositoryFactory& repositoryFactory_;
    Contracts::IRedactor& redactor_;
    const Domain::ProjectMemoryLimits limits_;

    std::mutex lifecycleMutex_;
    std::condition_variable lifecycleChanged_;
    std::size_t activeOperations_{};
    bool accepting_{true};
    std::atomic<bool> shutdownComplete_{};
};

ProjectMemoryService::ProjectMemoryService(
    Contracts::IProjectRegistryRepository& registry,
    Contracts::IProjectMemoryRepositoryFactory& repositoryFactory,
    Contracts::IRedactor& redactor,
    Domain::ProjectMemoryLimits limits)
    : implementation_{std::make_unique<Impl>(
          registry,
          repositoryFactory,
          redactor,
          std::move(limits))}
{
}

ProjectMemoryService::~ProjectMemoryService() noexcept
{
    implementation_->shutdown();
}

Domain::Result<Domain::ProjectInitialization> ProjectMemoryService::initialize(
    const Domain::InitializeProjectRequest& request,
    const Domain::OperationContext& context) noexcept
{
    return implementation_->initialize(request, context);
}

Domain::Result<Domain::MemoryWriteOutcome> ProjectMemoryService::remember(
    const Domain::RememberProjectMemoryRequest& request,
    const Domain::OperationContext& context) noexcept
{
    return implementation_->remember(request, context);
}

Domain::Result<Domain::MemoryBatchOutcome> ProjectMemoryService::rememberBatch(
    const Domain::RememberProjectMemoryBatchRequest& request,
    const Domain::OperationContext& context) noexcept
{
    return implementation_->rememberBatch(request, context);
}

Domain::Result<Domain::MemoryPage> ProjectMemoryService::search(
    const Domain::SearchProjectMemoryRequest& request,
    const Domain::OperationContext& context) noexcept
{
    return implementation_->search(request, context);
}

Domain::Result<Domain::MemoryRecords> ProjectMemoryService::get(
    const Domain::GetProjectMemoryRequest& request,
    const Domain::OperationContext& context) noexcept
{
    return implementation_->get(request, context);
}

Domain::Result<Domain::ProjectMemoryRecord> ProjectMemoryService::update(
    const Domain::UpdateProjectMemoryRequest& request,
    const Domain::OperationContext& context) noexcept
{
    return implementation_->update(request, context);
}

Domain::Result<Domain::ForgetOutcome> ProjectMemoryService::forget(
    const Domain::ForgetProjectMemoryRequest& request,
    const Domain::OperationContext& context) noexcept
{
    return implementation_->forget(request, context);
}

Domain::Result<Domain::MemoryPage> ProjectMemoryService::listRecent(
    const Domain::ListRecentProjectMemoryRequest& request,
    const Domain::OperationContext& context) noexcept
{
    return implementation_->listRecent(request, context);
}

Domain::Result<Domain::LinkOutcome> ProjectMemoryService::link(
    const Domain::LinkProjectMemoryRequest& request,
    const Domain::OperationContext& context) noexcept
{
    return implementation_->link(request, context);
}

Domain::Result<Domain::ProjectMemoryExport> ProjectMemoryService::exportMemory(
    const Domain::ExportProjectMemoryRequest& request,
    const Contracts::WorkspaceAuthority& writeAuthority,
    const Contracts::AuthorizedToolCall& authorization,
    const Domain::OperationContext& context) noexcept
{
    return implementation_->exportMemory(
        request, writeAuthority, authorization, context);
}

Domain::Result<Domain::ProjectMemoryImport> ProjectMemoryService::importMemory(
    const Domain::ImportProjectMemoryRequest& request,
    const Domain::OperationContext& context) noexcept
{
    return implementation_->importMemory(request, context);
}

Domain::Result<Domain::ProjectMemoryStatus> ProjectMemoryService::status(
    const Domain::ProjectMemoryStatusRequest& request,
    const Domain::OperationContext& context) noexcept
{
    return implementation_->status(request, context);
}

Domain::Result<void> ProjectMemoryService::closeProject(
    const Domain::ProjectId& projectId,
    const Domain::OperationContext& context) noexcept
{
    return implementation_->closeProject(projectId, context);
}

Domain::Result<Domain::ResetReport> ProjectMemoryService::resetProjectMemory(
    const Domain::ProjectId& projectId,
    const Domain::DestructiveConfirmation& confirmation,
    const Domain::OperationContext& context) noexcept
{
    return implementation_->resetProjectMemory(projectId, confirmation, context);
}

Domain::Result<Domain::ResetReport> ProjectMemoryService::resetAllProjectMemory(
    const Domain::DestructiveConfirmation& confirmation,
    const Domain::OperationContext& context) noexcept
{
    return implementation_->resetAllProjectMemory(confirmation, context);
}

void ProjectMemoryService::shutdown() noexcept
{
    implementation_->shutdown();
}

} // namespace ForgeConductor::Application
