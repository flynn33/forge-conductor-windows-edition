#include "ForgeConductor/Application/ManagedRunService.h"
#include "ForgeConductor/Application/ManagedRunWorkerPolicy.h"

#include "ForgeConductor/Domain/Utf8.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <map>
#include <limits>
#include <mutex>
#include <set>
#include <stop_token>
#include <thread>
#include <utility>
#include <vector>

namespace ForgeConductor::Application {
namespace {

[[nodiscard]] Domain::Error failure(
    const std::string_view code,
    const char* const message,
    const bool retryable = false)
{
    return Domain::makeError(code, message, retryable);
}

[[nodiscard]] Domain::Result<void> validate(
    const Domain::ManagedRunStartRequest& request,
    const Domain::OperationContext& context)
{
    if (context.isCancellationRequested()) {
        return Domain::Result<void>::failure(failure(
            Domain::ErrorCodes::Cancelled,
            "The managed run start was cancelled."));
    }
    if (context.isExpired(std::chrono::steady_clock::now()))
        return Domain::Result<void>::failure(failure(Domain::ErrorCodes::DeadlineExceeded, "The managed run admission deadline expired."));
    if (request.workerScope) {
        const auto& scope = *request.workerScope;
        if (scope.trustedRoots.empty() || scope.trustedRoots.size() > 64U || scope.grants.empty() || scope.grants.size() > 5U ||
            scope.denials.size() > 5U || scope.allowedTools.size() > 256U || !scope.timeoutSeconds || scope.timeoutSeconds > 3600U ||
            std::any_of(scope.allowedTools.begin(), scope.allowedTools.end(), [](const auto& name) {
                return name.empty() || name.size() > 128U || !isManagedWorkerToolPermitted(name);
            }))
            return Domain::Result<void>::failure(failure(Domain::ErrorCodes::InvalidRequest, "The inherited worker scope is invalid or excessive."));
        if (std::any_of(scope.grants.begin(), scope.grants.end(), [&](auto grant) {
                return std::find(scope.denials.begin(), scope.denials.end(), grant) != scope.denials.end();
            }))
            return Domain::Result<void>::failure(failure(Domain::ErrorCodes::Unauthorized, "The inherited worker scope grants a denied mode."));
    }
    if (request.authorityGeneration == 0U) {
        return Domain::Result<void>::failure(failure(
            Domain::ErrorCodes::InvalidRequest,
            "The managed run requires a nonzero project authority generation."));
    }
    if (request.task.empty() ||
        request.task.size() > Domain::MaximumManagedRunTaskBytes ||
        request.task.find('\0') != std::string::npos ||
        !Domain::isValidUtf8(request.task)) {
        return Domain::Result<void>::failure(failure(
            Domain::ErrorCodes::InvalidRequest,
            "The managed run task is empty, invalid, or exceeds its bound."));
    }
    if (request.providerReceiveTimeoutSeconds &&
        (*request.providerReceiveTimeoutSeconds == 0U ||
         *request.providerReceiveTimeoutSeconds > Domain::MaximumManagedProviderReceiveTimeoutSeconds)) {
        return Domain::Result<void>::failure(failure(
            Domain::ErrorCodes::InvalidRequest,
            "The provider receive timeout must be within 1 through 3600 seconds."));
    }
    return Domain::Result<void>::success();
}

[[nodiscard]] Domain::ManagedRunSnapshot snapshot(
    const Domain::ManagedRunRecord& record,
    const bool cancellationRequested,
    const bool pauseRequested = false)
{
    return Domain::ManagedRunSnapshot{
        record,
        true,
        cancellationRequested,
        pauseRequested};
}

[[nodiscard]] Domain::Result<Domain::ManagedFunctionCallOutput> managedFunctionOutput(
    const std::string& callId, std::string output)
{
    auto payload = nlohmann::json::parse(output, nullptr, false);
    if (!payload.is_object() || !payload.contains("image_base64"))
        return Domain::Result<Domain::ManagedFunctionCallOutput>::success({callId, std::move(output)});
    if (!payload.at("image_base64").is_string() || !payload.contains("image_mime_type") ||
        !payload.at("image_mime_type").is_string())
        return Domain::Result<Domain::ManagedFunctionCallOutput>::failure(failure(
            Domain::ErrorCodes::InternalFailure, "The native image result lacks valid preview data or MIME type."));
    Domain::ManagedImagePreview image{payload.at("image_mime_type").get<std::string>(),
        payload.at("image_base64").get<std::string>()};
    if (!Domain::isValidManagedImagePreview(image))
        return Domain::Result<Domain::ManagedFunctionCallOutput>::failure(failure(
            Domain::ErrorCodes::InternalFailure, "The native image result is invalid or exceeds its bounded preview limit."));
    payload.erase("image_base64");
    payload["image_content_block"] = true;
    return Domain::Result<Domain::ManagedFunctionCallOutput>::success({callId, payload.dump(), std::move(image)});
}

} // namespace

class ManagedRunService::Impl final {
public:
    Impl(
        Contracts::IManagedResponsesTransport& transport,
        Contracts::IManagedRunStore& store,
        Contracts::IClock& clock,
        ManagedRunToolDependencies tools,
        ManagedRunContinuityDependencies continuity)
        : transport_{transport},
          store_{store},
          clock_{clock},
          tools_{tools},
          continuity_{std::move(continuity)}
    {
    }

    ~Impl() noexcept { shutdown(); }

    [[nodiscard]] Domain::Result<Domain::ManagedRunSnapshot> start(
        const Domain::ManagedRunStartRequest& requested,
        const Domain::OperationContext& context) noexcept
    {
        try {
            auto request = requested;
            if (request.authorityGeneration == 0U && tools_.workspaceAuthority) {
                auto resolved = tools_.workspaceAuthority->authorityFor(
                    request.projectId, context);
                if (!resolved) {
                    return Domain::Result<Domain::ManagedRunSnapshot>::failure(
                        std::move(resolved).error());
                }
                request.authorityGeneration = resolved.value().generation();
                request.clientId = resolved.value().callerId();
            }
            if (auto valid = validate(request, context); !valid) {
                return Domain::Result<Domain::ManagedRunSnapshot>::failure(
                    std::move(valid).error());
            }
            reapFinishedIndependentRuns();
            {
                std::lock_guard lock{mutex_};
                if (shutdown_) {
                    return Domain::Result<Domain::ManagedRunSnapshot>::failure(
                        failure(
                            Domain::ErrorCodes::TransportClosed,
                            "The managed run service is shut down."));
                }
                const auto found = active_.find(request.runId);
                if (found != active_.end()) {
                    if (sameRequest(found->second->request, request)) {
                        return Domain::Result<Domain::ManagedRunSnapshot>::success(
                            snapshot(
                                found->second->record,
                                found->second->worker.get_stop_token()
                                    .stop_requested(),
                                found->second->pauseRequested));
                    }
                    return Domain::Result<Domain::ManagedRunSnapshot>::failure(
                        failure(
                            Domain::ErrorCodes::Conflict,
                            "The managed run id is already bound to another request."));
                }
            }

            auto persisted = store_.load(request.runId, context);
            if (!persisted) {
                return Domain::Result<Domain::ManagedRunSnapshot>::failure(
                    std::move(persisted).error());
            }
            if (persisted.value()) {
                if (persisted.value()->projectId == request.projectId &&
                    persisted.value()->clientId == request.clientId &&
                    persisted.value()->task == request.task &&
                    persisted.value()->authorityGeneration ==
                        request.authorityGeneration &&
                    persisted.value()->allowTools == request.allowTools &&
                    persisted.value()->readOnlyTools == request.readOnlyTools &&
                    persisted.value()->workerScope == request.workerScope &&
                    persisted.value()->providerReceiveTimeoutSeconds == request.providerReceiveTimeoutSeconds) {
                    return Domain::Result<Domain::ManagedRunSnapshot>::success(
                        snapshot(*persisted.value(), false));
                }
                return Domain::Result<Domain::ManagedRunSnapshot>::failure(
                    failure(
                        Domain::ErrorCodes::Conflict,
                        "The durable managed run id belongs to another request."));
            }

            const auto now = clock_.utcNow();
            Domain::ManagedRunRecord record{
                request.runId,
                request.projectId,
                request.clientId,
                request.task,
                request.authorityGeneration,
                Domain::ManagedRunState::Running,
                std::nullopt,
                0U,
                0U,
                std::nullopt,
                std::nullopt,
                std::nullopt,
                {},
                now,
                now,
                request.allowTools};
            record.readOnlyTools = request.readOnlyTools;
            record.providerReceiveTimeoutSeconds = request.providerReceiveTimeoutSeconds;
            record.workerScope = request.workerScope;
            if (!request.workerScope && !request.readOnlyTools) {
                if (auto saved = store_.save(record, context); !saved) {
                    return Domain::Result<Domain::ManagedRunSnapshot>::failure(std::move(saved).error());
                }
            }

            auto active = std::make_shared<ActiveRun>(request, record);
            {
                std::lock_guard lock{mutex_};
                if (shutdown_) {
                    return Domain::Result<Domain::ManagedRunSnapshot>::failure(
                        failure(
                            Domain::ErrorCodes::TransportClosed,
                            "The managed run service shut down during admission."));
                }
                if (active_.contains(request.runId))
                    return Domain::Result<Domain::ManagedRunSnapshot>::failure(failure(
                        Domain::ErrorCodes::Conflict, "The managed run id was admitted concurrently."));
                if (request.workerScope) {
                    const auto running = static_cast<std::size_t>(std::count_if(active_.begin(), active_.end(), [](const auto& entry) {
                        return entry.second->record.workerScope && !terminal(entry.second->record.state);
                    }));
                    if (running >= ManagedRunService::MaximumConcurrentWorkers)
                        return Domain::Result<Domain::ManagedRunSnapshot>::failure(failure(
                            Domain::ErrorCodes::LimitExceeded, "The independent worker concurrent limit is 16.", true));
                } else if (request.readOnlyTools) {
                    const auto running = static_cast<std::size_t>(std::count_if(active_.begin(), active_.end(), [](const auto& entry) {
                        return entry.second->record.readOnlyTools && !entry.second->record.workerScope && !terminal(entry.second->record.state);
                    }));
                    if (running >= ManagedRunService::MaximumConcurrentReviewers)
                        return Domain::Result<Domain::ManagedRunSnapshot>::failure(failure(
                            Domain::ErrorCodes::LimitExceeded, "The independent reviewer concurrent limit is 16.", true));
                }
                if (request.workerScope || request.readOnlyTools)
                    if (auto saved = store_.save(record, context); !saved)
                        return Domain::Result<Domain::ManagedRunSnapshot>::failure(std::move(saved).error());
                const auto [_, inserted] =
                    active_.emplace(request.runId, active);
                if (!inserted) {
                    return Domain::Result<Domain::ManagedRunSnapshot>::failure(
                        failure(
                            Domain::ErrorCodes::Conflict,
                            "The managed run id was admitted concurrently."));
                }
                try {
                    active->worker = std::jthread{
                        [this, request, run = active.get()](const std::stop_token token) {
                            execute(request, token);
                            run->threadFinished.store(true);
                        }};
                } catch (...) {
                    active_.erase(request.runId);
                    record.state = Domain::ManagedRunState::Failed;
                    record.lastError = failure(Domain::ErrorCodes::InternalFailure,
                        "The managed run worker thread could not be created.");
                    record.updatedAt = clock_.utcNow();
                    const auto error = *record.lastError;
                    if (auto saved = store_.save(record, context); !saved)
                        return Domain::Result<Domain::ManagedRunSnapshot>::failure(std::move(saved).error());
                    return Domain::Result<Domain::ManagedRunSnapshot>::failure(error);
                }
            }
            return Domain::Result<Domain::ManagedRunSnapshot>::success(
                snapshot(record, false));
        } catch (...) {
            return Domain::Result<Domain::ManagedRunSnapshot>::failure(failure(
                Domain::ErrorCodes::InternalFailure,
                "The managed run could not be started safely."));
        }
    }

    [[nodiscard]] Domain::Result<Domain::ManagedRunSnapshot> status(
        const Domain::SessionId& runId,
        const Domain::OperationContext& context) noexcept
    {
        try {
            reapFinishedIndependentRuns();
            {
                std::lock_guard lock{mutex_};
                const auto found = active_.find(runId);
                if (found != active_.end()) {
                    const auto& record = found->second->record;
                    const bool terminalIndependent = (record.workerScope || record.readOnlyTools) && terminal(record.state);
                    if (!terminalIndependent) return Domain::Result<Domain::ManagedRunSnapshot>::success(
                        snapshot(record, found->second->worker.get_stop_token().stop_requested(), found->second->pauseRequested));
                }
            }
            auto persisted = store_.load(runId, context);
            if (!persisted) {
                return Domain::Result<Domain::ManagedRunSnapshot>::failure(
                    std::move(persisted).error());
            }
            if (!persisted.value()) {
                return Domain::Result<Domain::ManagedRunSnapshot>::failure(
                    failure(
                        Domain::ErrorCodes::SessionNotFound,
                        "The managed run was not found."));
            }
            return Domain::Result<Domain::ManagedRunSnapshot>::success(
                snapshot(*persisted.value(), false));
        } catch (...) {
            return Domain::Result<Domain::ManagedRunSnapshot>::failure(failure(
                Domain::ErrorCodes::InternalFailure,
                "The managed run status failed safely."));
        }
    }

    [[nodiscard]] Domain::Result<Domain::ManagedRunSnapshot> cancel(
        const Domain::SessionId& runId,
        const Domain::OperationContext& context) noexcept
    {
        try {
            std::shared_ptr<ActiveRun> active;
            std::optional<Domain::ProviderSessionId> responseId;
            {
                std::lock_guard lock{mutex_};
                const auto found = active_.find(runId);
                if (found != active_.end()) {
                    active = found->second;
                    if (active->record.state == Domain::ManagedRunState::Running ||
                        active->record.state == Domain::ManagedRunState::Paused) {
                        active->record.state =
                            Domain::ManagedRunState::Cancelling;
                        active->record.updatedAt = clock_.utcNow();
                    }
                    active->pauseRequested = false;
                    active->worker.request_stop();
                    responseId = active->record.providerResponseId;
                    active->boundaryChanged.notify_all();
                }
            }
            if (!active) {
                return status(runId, context);
            }
            transport_.cancel(
                active->request.operationId,
                responseId);
            if (tools_.router) {
                tools_.router->cancel(active->request.operationId);
            }
            // Provider cancellation remains outside the service lock. The
            // worker publishes this record under the same mutex, so copying
            // its strings/optionals must also hold it.
            const std::lock_guard lock{mutex_};
            return Domain::Result<Domain::ManagedRunSnapshot>::success(
                snapshot(active->record, true));
        } catch (...) {
            return Domain::Result<Domain::ManagedRunSnapshot>::failure(failure(
                Domain::ErrorCodes::InternalFailure,
                "The managed run cancellation failed safely."));
        }
    }

    [[nodiscard]] Domain::Result<Domain::ManagedRunSnapshot> pause(
        const Domain::SessionId& runId,
        const Domain::OperationContext& context) noexcept
    {
        try {
            std::shared_ptr<ActiveRun> active;
            {
                std::lock_guard lock{mutex_};
                const auto found = active_.find(runId);
                if (found != active_.end()) {
                    active = found->second;
                    if (active->record.state == Domain::ManagedRunState::Running) {
                        active->pauseRequested = true;
                    }
                    return Domain::Result<Domain::ManagedRunSnapshot>::success(
                        snapshot(
                            active->record,
                            active->worker.get_stop_token().stop_requested(),
                            active->pauseRequested));
                }
            }
            return status(runId, context);
        } catch (...) {
            return Domain::Result<Domain::ManagedRunSnapshot>::failure(failure(
                Domain::ErrorCodes::InternalFailure,
                "The managed run pause request failed safely."));
        }
    }

    [[nodiscard]] Domain::Result<Domain::ManagedRunSnapshot> resume(
        const Domain::SessionId& runId,
        const Domain::OperationContext& context) noexcept
    {
        try {
            std::shared_ptr<ActiveRun> active;
            std::optional<Domain::ManagedRunRecord> record;
            bool foundActive{};
            {
                std::lock_guard lock{mutex_};
                const auto found = active_.find(runId);
                if (found != active_.end()) {
                    foundActive = true;
                    active = found->second;
                    if (active->record.state == Domain::ManagedRunState::Paused ||
                        (active->record.state == Domain::ManagedRunState::Running &&
                         active->pauseRequested)) {
                        active->pauseRequested = false;
                        if (active->record.state == Domain::ManagedRunState::Paused) {
                            active->record.state = Domain::ManagedRunState::Running;
                            active->record.updatedAt = clock_.utcNow();
                        }
                        record.emplace(active->record);
                        active->boundaryChanged.notify_all();
                    } else {
                        return Domain::Result<Domain::ManagedRunSnapshot>::success(
                            snapshot(
                                active->record,
                                active->worker.get_stop_token().stop_requested(),
                                active->pauseRequested));
                    }
                }
            }
            if (!foundActive) {
                return status(runId, context);
            }
            auto saved = store_.save(*record, context);
            if (!saved) {
                return Domain::Result<Domain::ManagedRunSnapshot>::failure(
                    std::move(saved).error());
            }
            return Domain::Result<Domain::ManagedRunSnapshot>::success(
                snapshot(*record, false, false));
        } catch (...) {
            return Domain::Result<Domain::ManagedRunSnapshot>::failure(failure(
                Domain::ErrorCodes::InternalFailure,
                "The managed run resume request failed safely."));
        }
    }

    void shutdown() noexcept
    {
        std::vector<std::shared_ptr<ActiveRun>> active;
        try {
            reapFinishedIndependentRuns();
            {
                std::lock_guard lock{mutex_};
                if (shutdown_) {
                    return;
                }
                shutdown_ = true;
                active.reserve(active_.size());
                for (const auto& entry : active_) {
                    entry.second->pauseRequested = false;
                    active.push_back(entry.second);
                }
            }
            for (const auto& run : active) {
                {
                    const std::lock_guard lock{mutex_};
                    if ((run->record.workerScope || run->record.readOnlyTools) && terminal(run->record.state)) continue;
                }
                run->worker.request_stop();
                run->boundaryChanged.notify_all();
                transport_.cancel(
                    run->request.operationId,
                    std::nullopt);
                if (tools_.router) {
                    tools_.router->cancel(run->request.operationId);
                }
            }
            for (const auto& run : active) {
                if (run->worker.joinable()) {
                    run->worker.join();
                }
            }
        } catch (...) {
        }
    }

private:
    struct ActiveRun final {
        ActiveRun(
            Domain::ManagedRunStartRequest value,
            Domain::ManagedRunRecord initial)
            : request{std::move(value)}, record{std::move(initial)},
              deadline{request.workerScope ? std::chrono::steady_clock::now() + std::chrono::seconds{request.workerScope->timeoutSeconds}
                  : Domain::MonotonicTimePoint::max()}
        {
        }

        Domain::ManagedRunStartRequest request;
        Domain::ManagedRunRecord record;
        Domain::MonotonicTimePoint deadline;
        bool pauseRequested{};
        std::condition_variable_any boundaryChanged;
        std::atomic_bool threadFinished{};
        std::jthread worker;
    };

    [[nodiscard]] static bool terminal(const Domain::ManagedRunState state) noexcept
    {
        return state == Domain::ManagedRunState::Completed || state == Domain::ManagedRunState::Failed ||
            state == Domain::ManagedRunState::Cancelled;
    }

    void reapFinishedIndependentRuns()
    {
        std::vector<std::shared_ptr<ActiveRun>> retired;
        {
            const std::lock_guard lock{mutex_};
            if (shutdown_) return;
            for (auto current = active_.begin(); current != active_.end();) {
                if ((current->second->record.workerScope || current->second->record.readOnlyTools) && current->second->threadFinished.load() &&
                    terminal(current->second->record.state)) {
                    retired.push_back(std::move(current->second));
                    current = active_.erase(current);
                } else ++current;
            }
        }
        for (const auto& run : retired) if (run->worker.joinable()) run->worker.join();
    }

    [[nodiscard]] Domain::Result<Contracts::WorkspaceAuthority> currentWorkerAuthority(
        const Domain::ManagedRunStartRequest& request,
        const Domain::OperationContext& context)
    {
        auto current = tools_.workspaceAuthority->authorityFor(request.projectId, context);
        if (!current) return current;
        if (current.value().projectId() != request.projectId || current.value().callerId() != request.clientId ||
            current.value().generation() == (std::numeric_limits<std::uint64_t>::max)())
            return Domain::Result<Contracts::WorkspaceAuthority>::failure(failure(
                Domain::ErrorCodes::Unauthorized, "The independent worker owner binding is no longer valid."));
        const auto& frozen = *request.workerScope;
        auto roots = frozen.trustedRoots;
        auto grants = frozen.grants;
        std::erase_if(roots, [&](const auto& root) {
            return std::find(current.value().trustedRoots().begin(), current.value().trustedRoots().end(), root) ==
                current.value().trustedRoots().end();
        });
        std::erase_if(grants, [&](auto grant) {
            return std::find(current.value().grants().begin(), current.value().grants().end(), grant) == current.value().grants().end() ||
                std::find(current.value().denials().begin(), current.value().denials().end(), grant) != current.value().denials().end();
        });
        const bool shell = frozen.shellEnabled && current.value().shellEnabled() &&
            std::find(grants.begin(), grants.end(), Domain::FileAccess::Execute) != grants.end();
        // Revocation stops this attempt; a fresh, explicitly scoped attempt can
        // use the remaining rights. The admitted receipt's frozen scope is immutable.
        if (roots.empty() || grants.empty() || roots != frozen.trustedRoots || grants != frozen.grants || shell != frozen.shellEnabled)
            return Domain::Result<Contracts::WorkspaceAuthority>::failure(failure(
                Domain::ErrorCodes::Unauthorized, "Current owner policy revoked part of the independent worker's frozen scope."));
        return tools_.workspaceAuthority->narrow(current.value(), roots, grants, shell,
            current.value().generation() + 1U, context);
    }

    [[nodiscard]] static bool sameRequest(
        const Domain::ManagedRunStartRequest& left,
        const Domain::ManagedRunStartRequest& right) noexcept
    {
        return left.runId == right.runId &&
            left.projectId == right.projectId &&
            left.clientId == right.clientId &&
            left.operationId == right.operationId &&
            left.authorityGeneration == right.authorityGeneration &&
            left.task == right.task &&
            left.allowTools == right.allowTools &&
            left.readOnlyTools == right.readOnlyTools &&
            left.workerScope == right.workerScope &&
            left.providerReceiveTimeoutSeconds == right.providerReceiveTimeoutSeconds;
    }

    void publishActive(const Domain::ManagedRunRecord& record) noexcept
    {
        try {
            std::lock_guard lock{mutex_};
            const auto found = active_.find(record.runId);
            if (found != active_.end()) {
                found->second->record = record;
            }
        } catch (...) {
        }
    }

    [[nodiscard]] bool awaitDispatchBoundary(
        const Domain::ManagedRunStartRequest& request,
        Domain::ManagedRunRecord& record,
        const Domain::OperationContext& persistenceContext,
        const std::stop_token token) noexcept
    {
        try {
            std::shared_ptr<ActiveRun> active;
            {
                std::lock_guard lock{mutex_};
                const auto found = active_.find(request.runId);
                if (found == active_.end()) {
                    record.state = Domain::ManagedRunState::Failed;
                    record.lastError = failure(
                        Domain::ErrorCodes::IntegrityFailure,
                        "The managed run owner disappeared before a dispatch boundary.");
                    return false;
                }
                active = found->second;
            }
            for (;;) {
                if (token.stop_requested()) { record.state = Domain::ManagedRunState::Cancelled; return false; }
                if (std::chrono::steady_clock::now() >= active->deadline) {
                    record.state = Domain::ManagedRunState::Failed;
                    record.lastError = failure(Domain::ErrorCodes::DeadlineExceeded, "The independent worker exceeded its total budget.", true);
                    return false;
                }
                bool shouldPause{};
                {
                    std::lock_guard lock{mutex_};
                    shouldPause = active->pauseRequested;
                }
                if (!shouldPause) {
                    if (token.stop_requested()) {
                        record.state = Domain::ManagedRunState::Cancelled;
                        return false;
                    }
                    if (record.state == Domain::ManagedRunState::Paused) {
                        record.state = Domain::ManagedRunState::Running;
                        record.updatedAt = clock_.utcNow();
                        publishActive(record);
                        if (auto saved = store_.save(record, persistenceContext); !saved) {
                            record.state = Domain::ManagedRunState::Failed;
                            record.lastError = saved.error();
                            return false;
                        }
                    }
                    return true;
                }

                if (record.state != Domain::ManagedRunState::Paused) {
                    record.state = Domain::ManagedRunState::Paused;
                    record.updatedAt = clock_.utcNow();
                    publishActive(record);
                    if (auto saved = store_.save(record, persistenceContext); !saved) {
                        record.state = Domain::ManagedRunState::Failed;
                        record.lastError = saved.error();
                        return false;
                    }
                }
                std::unique_lock lock{mutex_};
                active->boundaryChanged.wait_until(lock, token, active->deadline, [&] {
                    return !active->pauseRequested;
                });
                if (token.stop_requested()) {
                    record.state = Domain::ManagedRunState::Cancelled;
                    return false;
                }
            }
        } catch (...) {
            record.state = Domain::ManagedRunState::Failed;
            record.lastError = failure(
                Domain::ErrorCodes::InternalFailure,
                "The managed run dispatch boundary failed safely.");
            return false;
        }
    }

    [[nodiscard]] Domain::Result<
        std::optional<Domain::ContinuityAutomationOutcome>>
    observeContinuity(
        const Domain::ManagedRunStartRequest& request,
        const Domain::ManagedRunRecord& record,
        const std::vector<Domain::ContinuityWorkEntry>& completedToolWork,
        const Domain::OperationContext& context) noexcept
    {
        if (!request.automaticContinuity || !record.retainedContextTokens ||
            !continuity_.automation || !continuity_.codec ||
            !continuity_.projects || !continuity_.adapterId ||
            continuity_.contextCapacity == 0U) {
            return Domain::Result<
                std::optional<Domain::ContinuityAutomationOutcome>>::success(
                std::nullopt);
        }
        auto descriptor = continuity_.projects->descriptor(
            request.projectId, context);
        if (!descriptor) {
            return Domain::Result<
                std::optional<Domain::ContinuityAutomationOutcome>>::failure(
                std::move(descriptor).error());
        }
        if (descriptor.value().aliases.empty()) {
            return Domain::Result<
                std::optional<Domain::ContinuityAutomationOutcome>>::failure(
                failure(
                    Domain::ErrorCodes::IntegrityFailure,
                    "The managed run project has no canonical workspace alias."));
        }
        auto handoffId = Domain::ContinuityHandoffId::parse(
            request.runId.value());
        auto operationId = Domain::ContinuityOperationId::parse(
            request.runId.value());
        auto placeholder = Domain::Sha256Digest::parse(std::string(64U, '0'));
        if (!handoffId || !operationId || !placeholder) {
            return Domain::Result<
                std::optional<Domain::ContinuityAutomationOutcome>>::failure(
                failure(
                    Domain::ErrorCodes::IntegrityFailure,
                    "The managed run identity cannot form continuity identity."));
        }
        auto completedWork = completedToolWork;
        if (record.outputText) {
            auto summary = *record.outputText;
            if (summary.size() > 2U * 1024U) {
                summary.resize(2U * 1024U);
            }
            completedWork.push_back({
                std::nullopt,
                std::move(summary),
                std::optional<std::string>{"completed"}});
        }
        std::vector<Domain::ContinuityWorkEntry> openWork;
        if (!record.pendingFunctionCalls.empty()) {
            openWork.push_back({
                std::optional<std::string>{"pending-provider-tools"},
                "Resolve pending provider tool calls without repeating uncertain effects.",
                std::optional<std::string>{"open"}});
        }
        auto mission = request.task;
        if (mission.size() > 8U * 1024U) {
            mission.resize(8U * 1024U);
        }
        auto activeFiles = descriptor.value().aliases;
        if (activeFiles.size() > Domain::MaximumContinuityHandoffListItems) {
            activeFiles.erase(
                activeFiles.begin() +
                    static_cast<std::ptrdiff_t>(
                        Domain::MaximumContinuityHandoffListItems),
                activeFiles.end());
        }
        Domain::ContinuityHandoff draft{
            std::move(handoffId).value(),
            std::move(operationId).value(),
            record.updatedAt,
            Domain::ContinuityProject{
                request.projectId,
                descriptor.value().displayName,
                descriptor.value().aliases.front(),
                "unknown",
                "unknown",
                {}},
            Domain::ContinuitySession{
                request.runId,
                record.providerResponseId,
                continuity_.model,
                continuity_.provider},
            std::nullopt,
            std::move(mission),
            {
                "Continue only from the canonical context handoff.",
                "Do not repeat a tool effect whose completion is uncertain."},
            Domain::ContinuityCurrentWork{
                "R1",
                request.runId.value(),
                record.pendingFunctionCalls.empty()
                    ? "Continue the Manager-owned task."
                    : "Resolve pending provider tool calls without repeating uncertain effects.",
                std::move(activeFiles)},
            std::move(completedWork),
            std::move(openWork),
            {{"The Manager owns ordinary inference and successor activation.",
              std::nullopt}},
            Domain::ContinuityValidation{{}, {}, {}},
            {},
            {},
            {{1U,
              "Resume the Manager-owned task from this handoff.",
              "",
              "Useful work continues under the acknowledged successor."}},
            Domain::ContinuityHostState{
                *continuity_.adapterId,
                Domain::ContinuityState::Idle,
                "provider_usage",
                {},
                std::nullopt},
            std::move(placeholder).value(),
            true};
        auto document = continuity_.codec->encode(draft, context);
        if (!document) {
            return Domain::Result<
                std::optional<Domain::ContinuityAutomationOutcome>>::failure(
                std::move(document).error());
        }
        auto observed = continuity_.automation->observe(
            Domain::ContinuityAutomationObservation{
                std::move(document).value().handoff,
                Domain::ContextBudgetSignals{
                    continuity_.contextCapacity,
                    continuity_.reservedTokens,
                    std::nullopt,
                    record.retainedContextTokens,
                    std::nullopt,
                    std::nullopt,
                    false},
                false},
            context);
        if (!observed) {
            return Domain::Result<
                std::optional<Domain::ContinuityAutomationOutcome>>::failure(
                std::move(observed).error());
        }
        return Domain::Result<
            std::optional<Domain::ContinuityAutomationOutcome>>::success(
            std::move(observed).value());
    }

    void execute(
        const Domain::ManagedRunStartRequest& request,
        const std::stop_token token) noexcept
    {
        const Domain::OperationContext providerContext{
            request.operationId,
            [&] { std::lock_guard lock{mutex_}; return active_.at(request.runId)->deadline; }(),
            token,
            request.correlationId};
        Domain::ManagedRunRecord record = [&] {
            std::lock_guard lock{mutex_};
            return active_.at(request.runId)->record;
        }();
        const Domain::OperationContext persistenceContext{
            request.operationId,
            Domain::MonotonicTimePoint::max(),
            {},
            request.correlationId};

        if (token.stop_requested()) {
            record.state = Domain::ManagedRunState::Cancelled;
            record.lastError = failure(Domain::ErrorCodes::Cancelled, "The managed run was cancelled before authority resolution.");
            record.updatedAt = clock_.utcNow();
        }

        std::optional<Contracts::WorkspaceAuthority> authority;
        std::vector<Domain::McpToolDescriptor> descriptors;
        if (record.state == Domain::ManagedRunState::Running && tools_.workspaceAuthority &&
            (!request.allowTools || (tools_.catalog && tools_.router))) {
            auto resolved = tools_.workspaceAuthority->authorityFor(
                request.projectId, providerContext);
            if (!resolved) {
                record.lastError = resolved.error();
                record.state = token.stop_requested() || resolved.error().code == Domain::ErrorCodes::Cancelled
                    ? Domain::ManagedRunState::Cancelled : Domain::ManagedRunState::Failed;
            } else if ((request.workerScope ? resolved.value().generation() > request.authorityGeneration
                           : resolved.value().generation() != request.authorityGeneration) ||
                       resolved.value().callerId() != request.clientId) {
                record.lastError = failure(
                    Domain::ErrorCodes::Unauthorized,
                    "The managed run authority generation or client is stale.");
                record.state = Domain::ManagedRunState::Failed;
            } else {
                if (request.allowTools) {
                    if (request.workerScope) {
                        const auto& scope = *request.workerScope;
                        if (resolved.value().generation() == (std::numeric_limits<std::uint64_t>::max)()) {
                            record.lastError = failure(Domain::ErrorCodes::Unauthorized, "Worker authority generation cannot be narrowed.");
                            record.state = Domain::ManagedRunState::Failed;
                        } else {
                            auto inherited = tools_.workspaceAuthority->narrow(resolved.value(), scope.trustedRoots, scope.grants,
                                scope.shellEnabled, resolved.value().generation() + 1U, providerContext);
                            if (!inherited) {
                                record.lastError = inherited.error();
                                record.state = token.stop_requested() || inherited.error().code == Domain::ErrorCodes::Cancelled
                                    ? Domain::ManagedRunState::Cancelled : Domain::ManagedRunState::Failed;
                            }
                            else authority.emplace(std::move(inherited).value());
                        }
                    } else authority.emplace(std::move(resolved).value());
                    const auto available = tools_.catalog->tools();
                    for (const auto& descriptor : available) {
                        if ((!request.readOnlyTools || descriptor.tool.effect == Domain::ToolEffect::Read) &&
                            (!request.workerScope || (isManagedWorkerToolPermitted(descriptor.tool.name) &&
                                std::find(request.workerScope->allowedTools.begin(), request.workerScope->allowedTools.end(), descriptor.tool.name) != request.workerScope->allowedTools.end()))) {
                            descriptors.push_back(descriptor);
                        }
                    }
                }
            }
        }

        std::string input = request.task;
        std::vector<Domain::ManagedFunctionCallOutput> toolOutputs;
        std::vector<Domain::ContinuityWorkEntry> completedToolWork;
        std::set<Domain::ProviderSessionId> observedResponses;
        while (record.state == Domain::ManagedRunState::Running) {
            if (!awaitDispatchBoundary(
                    request, record, persistenceContext, token)) {
                break;
            }
            Domain::ManagedProviderTurnRequest turn{
                request.projectId,
                request.runId,
                request.authorityGeneration,
                std::move(input),
                record.providerResponseId,
                descriptors,
                std::move(toolOutputs), request.providerReceiveTimeoutSeconds};
            auto outcome = transport_.complete(turn, providerContext);
            record.updatedAt = clock_.utcNow();
            if (!outcome) {
                record.lastError = outcome.error();
                record.state = token.stop_requested() ||
                        outcome.error().code == Domain::ErrorCodes::Cancelled
                    ? Domain::ManagedRunState::Cancelled
                    : Domain::ManagedRunState::Failed;
                break;
            }

            auto value = std::move(outcome).value();
            if (!observedResponses.insert(value.responseId).second) {
                record.lastError = failure(
                    Domain::ErrorCodes::IntegrityFailure,
                    "The provider repeated a response identity; usage and effects were not replayed.");
                record.state = Domain::ManagedRunState::Failed;
                break;
            }
            record.providerResponseId = std::move(value.responseId);
            const auto maximum = (std::numeric_limits<std::uint64_t>::max)();
            record.inputTokens = record.inputTokens > maximum - value.inputTokens
                ? maximum
                : record.inputTokens + value.inputTokens;
            record.outputTokens = record.outputTokens > maximum - value.outputTokens
                ? maximum
                : record.outputTokens + value.outputTokens;
            record.retainedContextTokens = value.retainedContextTokens;
            record.pendingFunctionCalls = value.functionCalls;
            if (auto saved = store_.save(record, persistenceContext); !saved) {
                record.state = Domain::ManagedRunState::Failed;
                record.lastError = saved.error();
                break;
            }
            publishActive(record);
            if (!awaitDispatchBoundary(
                    request, record, persistenceContext, token)) {
                break;
            }
            if (value.functionCalls.empty()) {
                if (value.outputText.size() > Domain::MaximumManagedRunOutputBytes) {
                    auto end = Domain::MaximumManagedRunOutputBytes;
                    while (end > 0U && (static_cast<unsigned char>(value.outputText[end]) & 0xc0U) == 0x80U) --end;
                    value.outputText.resize(end);
                    record.outputTruncated = true;
                }
                record.outputText = std::move(value.outputText);
                record.pendingFunctionCalls.clear();
                record.state = token.stop_requested()
                    ? Domain::ManagedRunState::Cancelled
                    : Domain::ManagedRunState::Completed;
                break;
            }
            if (!authority || !tools_.router) {
                record.lastError = failure(
                    Domain::ErrorCodes::HostCapabilityUnavailable,
                    "The provider requested a tool, but managed native tools are unavailable.");
                record.state = Domain::ManagedRunState::Failed;
                break;
            }

            toolOutputs.clear();
            toolOutputs.reserve(value.functionCalls.size());
            for (const auto& call : value.functionCalls) {
                if (!awaitDispatchBoundary(
                        request, record, persistenceContext, token)) {
                    break;
                }
                auto toolRequestId = Domain::RequestId::parse(call.callId, 512U);
                if (!toolRequestId) {
                    record.lastError = failure(
                        Domain::ErrorCodes::MalformedMessage,
                        "The provider function call id is invalid.");
                    record.state = Domain::ManagedRunState::Failed;
                    break;
                }
                Domain::ToolCallRequest toolRequest{
                    Domain::McpRequestMetadata{
                        std::move(toolRequestId).value(),
                        request.correlationId,
                        request.clientId,
                        request.projectId,
                        "managed-run-v1"},
                    call.name,
                    call.canonicalArguments};
                // Enforce independently of advertisement: a provider can emit
                // an unadvertised write tool, including through resumed state.
                const bool permitted = (!request.readOnlyTools && !request.workerScope) ||
                    std::any_of(descriptors.begin(), descriptors.end(),
                        [&](const auto& descriptor) {
                            return descriptor.tool.name == call.name &&
                                (!request.readOnlyTools || descriptor.tool.effect == Domain::ToolEffect::Read);
                        });
                if (permitted && request.workerScope) {
                    auto refreshed = currentWorkerAuthority(request, providerContext);
                    if (!refreshed) {
                        record.lastError = refreshed.error();
                        record.state = token.stop_requested() || refreshed.error().code == Domain::ErrorCodes::Cancelled
                            ? Domain::ManagedRunState::Cancelled : Domain::ManagedRunState::Failed;
                        break;
                    }
                    authority.emplace(std::move(refreshed).value());
                }
                auto invoked = permitted
                    ? tools_.router->invoke(toolRequest, *authority, providerContext)
                    : Domain::Result<Domain::ToolCallOutcome>::failure(failure(
                        Domain::ErrorCodes::Unauthorized,
                        request.workerScope ? "The worker may invoke only its inherited advertised tool set."
                            : "The independent reviewer may invoke only read-only tools."));
                auto output = invoked
                    ? managedFunctionOutput(call.callId, std::move(invoked).value().canonicalPayload)
                    : Domain::Result<Domain::ManagedFunctionCallOutput>::failure(invoked.error());
                if (output) {
                    toolOutputs.push_back(std::move(output).value());
                } else {
                    nlohmann::json errorResult{
                        {"ok", false},
                        {"error",
                         {{"code", output.error().code},
                          {"message", output.error().message},
                          {"retryable", output.error().retryable}}}};
                    toolOutputs.push_back({call.callId, errorResult.dump()});
                }
                auto summary = "Native tool " + call.name + " result: " +
                    toolOutputs.back().canonicalOutput;
                if (summary.size() > 2U * 1024U) {
                    summary.resize(2U * 1024U);
                }
                if (completedToolWork.size() ==
                    Domain::MaximumContinuityHandoffListItems) {
                    completedToolWork.erase(completedToolWork.begin());
                }
                completedToolWork.push_back(
                    {call.callId, std::move(summary),
                     std::optional<std::string>{"completed"}});
            }
            if (record.state != Domain::ManagedRunState::Running) {
                break;
            }
            record.pendingFunctionCalls.clear();
            if (auto saved = store_.save(record, persistenceContext); !saved) {
                record.state = Domain::ManagedRunState::Failed;
                record.lastError = saved.error();
                break;
            }
            publishActive(record);
            auto continuity = observeContinuity(
                request, record, completedToolWork, providerContext);
            if (!continuity) {
                record.state = Domain::ManagedRunState::Failed;
                record.lastError = std::move(continuity).error();
                break;
            }
            if (continuity.value() &&
                continuity.value()->successorActivated) {
                if (!continuity.value()->successorProviderResponseId) {
                    record.state = Domain::ManagedRunState::Failed;
                    record.lastError = failure(
                        Domain::ErrorCodes::IntegrityFailure,
                        "Continuity activated a successor without a provider response identity.");
                    break;
                }
                record.providerResponseId =
                    continuity.value()->successorProviderResponseId;
                toolOutputs.clear();
                input =
                    "Continue the Manager-owned task from the canonical handoff. "
                    "Treat completed_work as authoritative and do not repeat "
                    "completed tool effects. When the task is satisfied, return "
                    "a terminal response without another tool call.";
                if (auto saved = store_.save(record, persistenceContext); !saved) {
                    record.state = Domain::ManagedRunState::Failed;
                    record.lastError = saved.error();
                    break;
                }
                publishActive(record);
                continue;
            }
            input.clear();
        }
        if (record.state == Domain::ManagedRunState::Running) {
            record.lastError = failure(
                Domain::ErrorCodes::LimitExceeded,
                "The managed run exceeded the bounded provider tool loop.");
            record.state = Domain::ManagedRunState::Failed;
        }
        record.updatedAt = clock_.utcNow();
        if (auto saved = store_.save(record, persistenceContext); !saved) {
            record.state = Domain::ManagedRunState::Failed;
            record.lastError = saved.error();
        }
        try {
            std::lock_guard lock{mutex_};
            const auto found = active_.find(request.runId);
            if (found != active_.end()) {
                found->second->record = std::move(record);
            }
        } catch (...) {
        }
    }

    Contracts::IManagedResponsesTransport& transport_;
    Contracts::IManagedRunStore& store_;
    Contracts::IClock& clock_;
    ManagedRunToolDependencies tools_;
    ManagedRunContinuityDependencies continuity_;
    std::mutex mutex_;
    std::map<Domain::SessionId, std::shared_ptr<ActiveRun>> active_;
    bool shutdown_{};
};

ManagedRunService::ManagedRunService(
    Contracts::IManagedResponsesTransport& transport,
    Contracts::IManagedRunStore& store,
    Contracts::IClock& clock,
    ManagedRunToolDependencies tools,
    ManagedRunContinuityDependencies continuity)
    : implementation_{
          std::make_unique<Impl>(
              transport, store, clock, tools, std::move(continuity))}
{
}

ManagedRunService::~ManagedRunService() noexcept = default;

Domain::Result<Domain::ManagedRunSnapshot> ManagedRunService::start(
    const Domain::ManagedRunStartRequest& request,
    const Domain::OperationContext& context) noexcept
{
    return implementation_->start(request, context);
}

Domain::Result<Domain::ManagedRunSnapshot> ManagedRunService::status(
    const Domain::SessionId& runId,
    const Domain::OperationContext& context) noexcept
{
    return implementation_->status(runId, context);
}

Domain::Result<Domain::ManagedRunSnapshot> ManagedRunService::cancel(
    const Domain::SessionId& runId,
    const Domain::OperationContext& context) noexcept
{
    return implementation_->cancel(runId, context);
}

Domain::Result<Domain::ManagedRunSnapshot> ManagedRunService::pause(
    const Domain::SessionId& runId,
    const Domain::OperationContext& context) noexcept
{
    return implementation_->pause(runId, context);
}

Domain::Result<Domain::ManagedRunSnapshot> ManagedRunService::resume(
    const Domain::SessionId& runId,
    const Domain::OperationContext& context) noexcept
{
    return implementation_->resume(runId, context);
}

void ManagedRunService::shutdown() noexcept
{
    implementation_->shutdown();
}

} // namespace ForgeConductor::Application
