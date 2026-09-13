#include "ForgeConductor/Application/ManagedRunService.h"

#include "ForgeConductor/Domain/Utf8.h"

#include <nlohmann/json.hpp>

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
        const Domain::ManagedRunStartRequest& request,
        const Domain::OperationContext& context) noexcept
    {
        try {
            if (auto valid = validate(request, context); !valid) {
                return Domain::Result<Domain::ManagedRunSnapshot>::failure(
                    std::move(valid).error());
            }
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
                        request.authorityGeneration) {
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
                now};
            if (auto saved = store_.save(record, context); !saved) {
                return Domain::Result<Domain::ManagedRunSnapshot>::failure(
                    std::move(saved).error());
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
                const auto [_, inserted] =
                    active_.emplace(request.runId, active);
                if (!inserted) {
                    return Domain::Result<Domain::ManagedRunSnapshot>::failure(
                        failure(
                            Domain::ErrorCodes::Conflict,
                            "The managed run id was admitted concurrently."));
                }
                active->worker = std::jthread{
                    [this, request](const std::stop_token token) {
                        execute(request, token);
                    }};
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
            {
                std::lock_guard lock{mutex_};
                const auto found = active_.find(runId);
                if (found != active_.end()) {
                    return Domain::Result<Domain::ManagedRunSnapshot>::success(
                        snapshot(
                            found->second->record,
                            found->second->worker.get_stop_token()
                                .stop_requested(),
                            found->second->pauseRequested));
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
            {
                std::lock_guard lock{mutex_};
                if (shutdown_) {
                    return;
                }
                shutdown_ = true;
                active.reserve(active_.size());
                for (const auto& entry : active_) {
                    active.push_back(entry.second);
                }
            }
            for (const auto& run : active) {
                run->pauseRequested = false;
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
            : request{std::move(value)}, record{std::move(initial)}
        {
        }

        Domain::ManagedRunStartRequest request;
        Domain::ManagedRunRecord record;
        bool pauseRequested{};
        std::condition_variable_any boundaryChanged;
        std::jthread worker;
    };

    [[nodiscard]] static bool sameRequest(
        const Domain::ManagedRunStartRequest& left,
        const Domain::ManagedRunStartRequest& right) noexcept
    {
        return left.runId == right.runId &&
            left.projectId == right.projectId &&
            left.clientId == right.clientId &&
            left.operationId == right.operationId &&
            left.authorityGeneration == right.authorityGeneration &&
            left.task == right.task;
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
                active->boundaryChanged.wait(lock, token, [&] {
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
        if (!record.retainedContextTokens ||
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
            Domain::MonotonicTimePoint::max(),
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
            record.updatedAt = clock_.utcNow();
        }

        std::optional<Contracts::WorkspaceAuthority> authority;
        std::vector<Domain::McpToolDescriptor> descriptors;
        if (tools_.catalog && tools_.router && tools_.workspaceAuthority) {
            auto resolved = tools_.workspaceAuthority->authorityFor(
                request.projectId, providerContext);
            if (!resolved) {
                record.lastError = resolved.error();
                record.state = Domain::ManagedRunState::Failed;
            } else if (resolved.value().generation() !=
                           request.authorityGeneration ||
                       resolved.value().callerId() != request.clientId) {
                record.lastError = failure(
                    Domain::ErrorCodes::Unauthorized,
                    "The managed run authority generation or client is stale.");
                record.state = Domain::ManagedRunState::Failed;
            } else {
                authority.emplace(std::move(resolved).value());
                const auto available = tools_.catalog->tools();
                descriptors.assign(available.begin(), available.end());
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
                std::move(toolOutputs)};
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
                    value.outputText.resize(Domain::MaximumManagedRunOutputBytes);
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
                auto invoked = tools_.router->invoke(
                    toolRequest, *authority, providerContext);
                if (invoked) {
                    toolOutputs.push_back(
                        {call.callId, std::move(invoked).value().canonicalPayload});
                } else {
                    nlohmann::json errorResult{
                        {"ok", false},
                        {"error",
                         {{"code", invoked.error().code},
                          {"message", invoked.error().message},
                          {"retryable", invoked.error().retryable}}}};
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
