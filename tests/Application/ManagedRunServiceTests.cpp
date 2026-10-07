#include "ForgeConductor/Application/AgentRepositoryManagedRunStore.h"
#include "ForgeConductor/Application/ManagedRunService.h"
#include "ForgeConductor/Application/ManagedRunWorkerPolicy.h"
#include "ForgeConductor/Infrastructure/Windows/BCryptSha256Hasher.h"

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <map>
#include <mutex>
#include <stop_token>
#include <string>
#include <thread>

using namespace std::chrono_literals;

namespace {

namespace Domain = ForgeConductor::Domain;
namespace Contracts = ForgeConductor::Contracts;
namespace Application = ForgeConductor::Application;

template <typename T>
[[nodiscard]] T parsed(Domain::Result<T> value)
{
    assert(value);
    return std::move(value).value();
}

class Clock final : public Contracts::IClock {
public:
    [[nodiscard]] Domain::UtcTimePoint utcNow() const noexcept override
    {
        const std::lock_guard lock{mutex_};
        return utc_;
    }

    [[nodiscard]] Domain::MonotonicTimePoint monotonicNow() const noexcept override
    {
        return {};
    }

    void advance()
    {
        const std::lock_guard lock{mutex_};
        utc_ += 1s;
    }

private:
    mutable std::mutex mutex_;
    Domain::UtcTimePoint utc_{};
};

class Store final : public Contracts::IManagedRunStore {
public:
    [[nodiscard]] Domain::Result<std::optional<Domain::ManagedRunRecord>> load(
        const Domain::SessionId& runId,
        const Domain::OperationContext&) noexcept override
    {
        const std::lock_guard lock{mutex_};
        const auto found = records_.find(runId);
        return Domain::Result<
            std::optional<Domain::ManagedRunRecord>>::success(
            found == records_.end()
                ? std::nullopt
                : std::optional<Domain::ManagedRunRecord>{found->second});
    }

    [[nodiscard]] Domain::Result<void> save(
        const Domain::ManagedRunRecord& record,
        const Domain::OperationContext&) noexcept override
    {
        const std::lock_guard lock{mutex_};
        records_.insert_or_assign(record.runId, record);
        ++saves;
        return Domain::Result<void>::success();
    }

    std::size_t saves{};

private:
    std::mutex mutex_;
    std::map<Domain::SessionId, Domain::ManagedRunRecord> records_;
};

class AdmissionRepository final : public Contracts::IAgentSessionRepository {
public:
    [[nodiscard]] Domain::Result<void> save(
        const Domain::AgentSession& session,
        const Domain::OperationContext&) noexcept override
    {
        assert(run_);
        run_->session = session;
        ++sessionSaves;
        return Domain::Result<void>::success();
    }

    [[nodiscard]] Domain::Result<std::optional<Domain::AgentSession>> get(
        const Domain::SessionId& id,
        const Domain::OperationContext&) noexcept override
    {
        return Domain::Result<std::optional<Domain::AgentSession>>::success(
            run_ && run_->session.id == id
                ? std::optional<Domain::AgentSession>{run_->session}
                : std::nullopt);
    }

    [[nodiscard]] Domain::Result<std::vector<Domain::AgentSession>> list(
        const std::optional<Domain::AgentId>&,
        const std::optional<Domain::SessionStatus>&,
        std::size_t,
        const Domain::OperationContext&) noexcept override
    {
        return Domain::Result<std::vector<Domain::AgentSession>>::success({});
    }

    [[nodiscard]] Domain::Result<Domain::AgentRunStartPersistenceOutcome>
    startRun(
        const Domain::AgentRunStartMutation& mutation,
        const Domain::OperationContext&) noexcept override
    {
        admittedOpen = mutation.run.session.status == Domain::SessionStatus::Open;
        admittedWithoutSummary = !mutation.run.session.summary;
        admittedWithBinding = mutation.activeBinding &&
            mutation.run.session.clientId &&
            mutation.activeBinding->sessionId == mutation.run.session.id &&
            mutation.activeBinding->agentId == mutation.run.session.agentId &&
            mutation.activeBinding->goal == *mutation.run.goal;
        if (!admittedOpen || !admittedWithoutSummary || !admittedWithBinding) {
            return Domain::Result<
                Domain::AgentRunStartPersistenceOutcome>::failure(
                Domain::makeError(
                    Domain::ErrorCodes::InvalidRequest,
                    "The admission mutation violated the repository contract."));
        }
        run_ = mutation.run;
        return Domain::Result<
            Domain::AgentRunStartPersistenceOutcome>::success(
            {*run_, mutation.activeBinding, 0U});
    }

    [[nodiscard]] Domain::Result<std::optional<Domain::AgentRunRecord>> getRun(
        const Domain::SessionId& id,
        const Domain::OperationContext&) noexcept override
    {
        return Domain::Result<
            std::optional<Domain::AgentRunRecord>>::success(
            run_ && run_->session.id == id ? run_ : std::nullopt);
    }

    [[nodiscard]] Domain::Result<Domain::AgentRunReattachOutcome> reattachRun(
        const Domain::AgentRunReattachMutation&,
        const Domain::OperationContext&) noexcept override
    {
        return unavailable<Domain::AgentRunReattachOutcome>();
    }

    [[nodiscard]] Domain::Result<Domain::AgentRunCompletePersistenceOutcome>
    completeRun(
        const Domain::AgentRunCompleteMutation&,
        const Domain::OperationContext&) noexcept override
    {
        return unavailable<Domain::AgentRunCompletePersistenceOutcome>();
    }

    [[nodiscard]] Domain::Result<bool> touchRun(
        const Domain::SessionId&,
        Domain::UtcTimePoint,
        const Domain::OperationContext&) noexcept override
    {
        return unavailable<bool>();
    }

    [[nodiscard]] Domain::Result<std::optional<Domain::AgentRunRecord>>
    latestOpenRun(
        const Domain::ClientId&,
        const Domain::OperationContext&) noexcept override
    {
        return Domain::Result<
            std::optional<Domain::AgentRunRecord>>::success(std::nullopt);
    }

    [[nodiscard]] Domain::Result<Domain::AgentRunRecoveryOutcome> recoverRun(
        const Domain::AgentRunRecoveryRequest&,
        const Domain::OperationContext&) noexcept override
    {
        return unavailable<Domain::AgentRunRecoveryOutcome>();
    }

    [[nodiscard]] Domain::Result<Domain::AgentProjectionRepairOutcome>
    repairProjection(
        const Domain::AgentProjectionRepairRequest&,
        const Domain::OperationContext&) noexcept override
    {
        return unavailable<Domain::AgentProjectionRepairOutcome>();
    }

    [[nodiscard]] Domain::Result<Domain::AgentStaleCloseOutcome> closeStale(
        const Domain::AgentStaleCloseRequest&,
        const Domain::OperationContext&) noexcept override
    {
        return unavailable<Domain::AgentStaleCloseOutcome>();
    }

    [[nodiscard]] Domain::Result<void> quickCheck(
        const Domain::OperationContext&) noexcept override
    {
        return Domain::Result<void>::success();
    }

    void close() noexcept override {}

    bool admittedOpen{};
    bool admittedWithoutSummary{};
    bool admittedWithBinding{};
    std::size_t sessionSaves{};

    void corruptSealedSummary()
    {
        assert(run_ && run_->session.summary);
        auto& encoded = *run_->session.summary;
        const auto position = encoded.find("sealed result");
        assert(position != std::string::npos);
        encoded.replace(position, std::string{"sealed result"}.size(),
            "altered result");
    }

private:
    template <typename T>
    [[nodiscard]] static Domain::Result<T> unavailable() noexcept
    {
        return Domain::Result<T>::failure(Domain::makeError(
            Domain::ErrorCodes::HostCapabilityUnavailable,
            "This repository operation is outside the admission test."));
    }

    std::optional<Domain::AgentRunRecord> run_;
};

class Transport final : public Contracts::IManagedResponsesTransport {
public:
    enum class Mode { Success, Offline, Block, ToolLoop, ExtendedToolLoop, ReadOnlyAttack, WorkerAttack };

    [[nodiscard]] Domain::Result<Domain::ManagedProviderTurnResult> complete(
        const Domain::ManagedProviderTurnRequest& request,
        const Domain::OperationContext& context) noexcept override
    {
        {
            const std::lock_guard lock{mutex_};
            ++calls;
            lastProject = request.projectId.value();
            lastRun = request.runId.value();
            lastGeneration = request.authorityGeneration;
            lastToolCount = request.tools.size();
            lastReceiveTimeout = request.providerReceiveTimeoutSeconds;
        }
        if (mode == Mode::Block) {
            std::unique_lock lock{mutex_};
            firstToolResponseEntered_ = true;
            cv_.notify_all();
            cv_.wait(lock, context.cancellation, [this] { return released_; });
            return Domain::Result<Domain::ManagedProviderTurnResult>::failure(
                Domain::makeError(
                    Domain::ErrorCodes::Cancelled,
                    "The controlled provider request was cancelled."));
        }
        if (mode == Mode::Offline) {
            return Domain::Result<Domain::ManagedProviderTurnResult>::failure(
                Domain::makeError(
                    Domain::ErrorCodes::TransportClosed,
                    "The controlled provider is offline.",
                    true));
        }
        if (mode == Mode::ReadOnlyAttack) {
            if (calls == 1U) {
                sawToolDescriptor = request.tools.size() == 1U &&
                    request.tools.front().tool.name == "fixture_read" &&
                    request.tools.front().tool.effect == Domain::ToolEffect::Read;
                sawFreshContext = !request.previousResponseId && request.toolOutputs.empty();
                return Domain::Result<Domain::ManagedProviderTurnResult>::success({
                    parsed(Domain::ProviderSessionId::parse("reviewer-response-1")), {},
                    20U, 2U, 22U, {{"reviewer-write-attempt", "fixture_write", "{}"}}});
            }
            sawDeniedWrite = request.toolOutputs.size() == 1U &&
                request.toolOutputs.front().canonicalOutput.find("unauthorized") != std::string::npos;
            return Domain::Result<Domain::ManagedProviderTurnResult>::success({
                parsed(Domain::ProviderSessionId::parse("reviewer-response-2")),
                "Read-only review finished; attempted write was denied.", 23U, 4U, 27U, {}});
        }
        if (mode == Mode::WorkerAttack) {
            if (calls == 1U) {
                sawFreshContext = !request.previousResponseId && request.toolOutputs.empty();
                sawToolDescriptor = request.tools.size() == 2U &&
                    std::none_of(request.tools.begin(), request.tools.end(), [](const auto& descriptor) {
                        return descriptor.tool.name == "agent_spawn" || descriptor.tool.name == "session_checkpoint" ||
                            descriptor.tool.name == "session_handoff";
                    });
                {
                    std::unique_lock lock{mutex_};
                    firstToolResponseEntered_ = true;
                    cv_.notify_all();
                    cv_.wait(lock, context.cancellation, [this] { return !holdFirstToolResponse_; });
                    if (context.cancellation.stop_requested()) return Domain::Result<Domain::ManagedProviderTurnResult>::failure(
                        Domain::makeError(Domain::ErrorCodes::Cancelled, "The controlled worker response was cancelled."));
                }
                return Domain::Result<Domain::ManagedProviderTurnResult>::success({
                    parsed(Domain::ProviderSessionId::parse("worker-response-1")), {}, 10U, 3U, 13U,
                    {{"worker-write", "fixture_write", "{}"}, {"worker-recursive", "agent_spawn", "{}"}, {"worker-forged", "host_permission_grant", "{}"},
                        {"worker-checkpoint", "session_checkpoint", "{}"}, {"worker-handoff", "session_handoff", "{}"}}});
            }
            sawDeniedWrite = request.toolOutputs.size() == 5U &&
                request.toolOutputs[1].canonicalOutput.find("unauthorized") != std::string::npos &&
                request.toolOutputs[2].canonicalOutput.find("unauthorized") != std::string::npos &&
                request.toolOutputs[3].callId == "worker-checkpoint" &&
                request.toolOutputs[3].canonicalOutput.find("unauthorized") != std::string::npos &&
                request.toolOutputs[4].callId == "worker-handoff" &&
                request.toolOutputs[4].canonicalOutput.find("unauthorized") != std::string::npos;
            return Domain::Result<Domain::ManagedProviderTurnResult>::success({
                parsed(Domain::ProviderSessionId::parse("worker-response-2")), "Mutable worker finished.", 12U, 4U, 16U, {}});
        }
        if (mode == Mode::ToolLoop) {
            if (calls == 1U) {
                sawToolDescriptor = request.tools.size() == 1U &&
                    request.tools.front().tool.name == "fixture_read";
                {
                    std::unique_lock lock{mutex_};
                    firstToolResponseEntered_ = true;
                    cv_.notify_all();
                    cv_.wait(lock, context.cancellation, [this] {
                        return !holdFirstToolResponse_;
                    });
                    if (context.cancellation.stop_requested()) {
                        return Domain::Result<Domain::ManagedProviderTurnResult>::failure(
                            Domain::makeError(
                                Domain::ErrorCodes::Cancelled,
                                "The controlled tool response was cancelled."));
                    }
                }
                return Domain::Result<Domain::ManagedProviderTurnResult>::success(
                    Domain::ManagedProviderTurnResult{
                        parsed(Domain::ProviderSessionId::parse("resp_tool_1")),
                        {},
                        30U,
                        5U,
                        35U,
                        {{"call_fixture_1", "fixture_read", "{\"path\":\"README.md\"}"}}});
            }
            sawSuccessorPrompt = request.previousResponseId &&
                request.previousResponseId->value() == "resp_successor_root" &&
                request.input.find("Treat completed_work as authoritative") !=
                    std::string::npos &&
                request.input.find("return a terminal response") !=
                    std::string::npos &&
                request.toolOutputs.empty();
            sawToolOutput = request.input.empty() &&
                request.previousResponseId &&
                request.previousResponseId->value() == "resp_tool_1" &&
                request.toolOutputs.size() == 1U &&
                request.toolOutputs.front().callId == "call_fixture_1" &&
                request.toolOutputs.front().canonicalOutput ==
                    "{\"ok\":true,\"text\":\"fixture\"}";
            return Domain::Result<Domain::ManagedProviderTurnResult>::success(
                Domain::ManagedProviderTurnResult{
                    parsed(Domain::ProviderSessionId::parse("resp_tool_2")),
                    "tool loop completed",
                    40U,
                    7U,
                    47U,
                    {}});
        }
        if (mode == Mode::ExtendedToolLoop) {
            if (calls <= 80U) {
                const auto suffix = std::to_string(calls);
                return Domain::Result<Domain::ManagedProviderTurnResult>::success(
                    Domain::ManagedProviderTurnResult{
                        parsed(Domain::ProviderSessionId::parse(
                            "resp_extended_" + suffix)),
                        {},
                        1U,
                        1U,
                        calls,
                        {{"call_extended_" + suffix,
                          "fixture_read",
                          "{\"path\":\"README.md\"}"}}});
            }
            return Domain::Result<Domain::ManagedProviderTurnResult>::success(
                Domain::ManagedProviderTurnResult{
                    parsed(Domain::ProviderSessionId::parse(
                        "resp_extended_done")),
                    "extended tool loop completed",
                    1U,
                    1U,
                    calls,
                    {}});
        }
        return Domain::Result<Domain::ManagedProviderTurnResult>::success(
            Domain::ManagedProviderTurnResult{
                parsed(Domain::ProviderSessionId::parse("resp_ordinary_1")),
                "ordinary run completed",
                21U,
                8U,
                29U});
    }

    void cancel(
        const Domain::OperationId&,
        const std::optional<Domain::ProviderSessionId>&) noexcept override
    {
        {
            const std::lock_guard lock{mutex_};
            ++cancels;
        }
        cv_.notify_all();
        if (onCancel) onCancel();
    }

    std::function<void()> onCancel;
    Mode mode{Mode::Success};
    std::size_t calls{};
    std::size_t cancels{};
    std::string lastProject;
    std::string lastRun;
    std::uint64_t lastGeneration{};
    std::size_t lastToolCount{};
    std::optional<std::uint32_t> lastReceiveTimeout;
    bool sawToolDescriptor{};
    bool sawToolOutput{};
    bool sawSuccessorPrompt{};
    bool sawFreshContext{};
    bool sawDeniedWrite{};

    void holdFirstToolResponse() noexcept
    {
        const std::lock_guard lock{mutex_};
        holdFirstToolResponse_ = true;
    }

    void waitForFirstToolResponse()
    {
        std::unique_lock lock{mutex_};
        cv_.wait(lock, [this] { return firstToolResponseEntered_; });
    }

    void releaseFirstToolResponse() noexcept
    {
        {
            const std::lock_guard lock{mutex_};
            holdFirstToolResponse_ = false;
        }
        cv_.notify_all();
    }

private:
    std::mutex mutex_;
    std::condition_variable_any cv_;
    bool released_{};
    bool holdFirstToolResponse_{};
    bool firstToolResponseEntered_{};
};

class ToolCatalog final : public Contracts::IToolCatalog {
public:
    explicit ToolCatalog(const bool includeWrite = false)
    {
        tools_.push_back(Domain::McpToolDescriptor{
            Domain::ToolDescriptor{
                "fixture_read",
                "Read a controlled fixture.",
                "fixture",
                Domain::ToolEffect::Read,
                Domain::ToolAvailability::Available,
                true,
                false},
            "{\"additionalProperties\":false,\"properties\":{\"path\":{\"type\":\"string\"}},\"required\":[\"path\"],\"type\":\"object\"}"});
        if (includeWrite) {
            auto write = tools_.front();
            write.tool.name = "fixture_write";
            write.tool.effect = Domain::ToolEffect::Write;
            tools_.push_back(std::move(write));
            for (const auto name : {"session_checkpoint", "session_handoff"}) {
                auto continuity = tools_.front();
                continuity.tool.name = name;
                continuity.tool.effect = Domain::ToolEffect::Write;
                tools_.push_back(std::move(continuity));
            }
        }
    }

    [[nodiscard]] std::span<const Domain::McpToolDescriptor> tools()
        const noexcept override
    {
        return tools_;
    }

private:
    std::vector<Domain::McpToolDescriptor> tools_;
};

class WorkspaceAuthority final : public Contracts::IWorkspaceAuthority {
public:
    std::atomic_size_t calls{};
    bool waitForCancellation{};
    std::atomic_int revokedPolicy{};
    WorkspaceAuthority(Domain::ProjectId projectId, Domain::ClientId clientId, bool writable = false, bool shellEnabled = false)
        : projectId_{std::move(projectId)}, clientId_{std::move(clientId)}, writable_{writable}, shellEnabled_{shellEnabled}
    {
    }

    [[nodiscard]] Domain::Result<Contracts::WorkspaceAuthority> authorityFor(
        const Domain::ProjectId& projectId,
        const Domain::OperationContext& operation) noexcept override
    {
        ++calls;
        if (waitForCancellation) {
            std::unique_lock lock{resolutionMutex_};
            resolving_ = true;
            resolutionChanged_.notify_all();
            resolutionChanged_.wait(lock, operation.cancellation, [] { return false; });
            return Domain::Result<Contracts::WorkspaceAuthority>::failure(
                Domain::makeError(Domain::ErrorCodes::Cancelled, "Fixture authority resolution was cancelled."));
        }
        if (projectId != projectId_) {
            return Domain::Result<Contracts::WorkspaceAuthority>::failure(
                Domain::makeError(Domain::ErrorCodes::ProjectScopeMismatch,
                                  "unexpected test project"));
        }
        auto grants = writable_ ? std::vector<Domain::FileAccess>{Domain::FileAccess::Read, Domain::FileAccess::Write}
                                : std::vector<Domain::FileAccess>{Domain::FileAccess::Read};
        const auto revoked = revokedPolicy.load();
        const bool shell = shellEnabled_ && revoked != 2;
        if (shell) grants.push_back(Domain::FileAccess::Execute);
        return issueAuthority(
            parsed(Domain::AuthorityId::parse(
                "22222222-2222-4222-8222-222222222222")),
            projectId_,
            clientId_,
            {parsed(Domain::PathText::create(revoked == 1 ? "C:\\other-project" : "C:\\managed-test"))},
            Domain::FileAccess::Read,
            std::move(grants),
            {},
            shell,
            7U);
    }

    void waitUntilResolving()
    {
        std::unique_lock lock{resolutionMutex_};
        assert(resolutionChanged_.wait_for(lock, 2s, [&] { return resolving_; }));
    }

    [[nodiscard]] Domain::Result<Contracts::WorkspaceAuthority> narrow(
        const Contracts::WorkspaceAuthority& authority,
        const std::vector<Domain::PathText>& roots,
        const std::vector<Domain::FileAccess>& grants,
        bool shell,
        std::uint64_t generation,
        const Domain::OperationContext&) noexcept override
    {
        return narrowAuthority(authority, roots, grants, shell, generation);
    }

    [[nodiscard]] Domain::Result<Contracts::AuthorizedPath> authorize(
        const Contracts::WorkspaceAuthority&,
        const Domain::PathAuthorizationRequest&,
        const Domain::OperationContext&) noexcept override
    {
        return Domain::Result<Contracts::AuthorizedPath>::failure(
            Domain::makeError(Domain::ErrorCodes::HostCapabilityUnavailable,
                              "unused test authorize"));
    }

private:
    Domain::ProjectId projectId_;
    Domain::ClientId clientId_;
    bool writable_{};
    bool shellEnabled_{};
    std::mutex resolutionMutex_;
    std::condition_variable_any resolutionChanged_;
    bool resolving_{};
};
class ToolRouter final : public Contracts::IToolRouter {
public:
    [[nodiscard]] Domain::Result<Domain::ToolCallOutcome> invoke(
        const Domain::ToolCallRequest& request,
        const Contracts::WorkspaceAuthority& authority,
        const Domain::OperationContext&) noexcept override
    {
        ++calls;
        lastName = request.toolName;
        lastRoots = authority.trustedRoots();
        lastGrants = authority.grants();
        lastShell = authority.shellEnabled();
        sawBinding = request.toolName == "fixture_read" &&
            request.canonicalArguments == "{\"path\":\"README.md\"}" &&
            request.metadata.projectId ==
                std::optional<Domain::ProjectId>{authority.projectId()} &&
            request.metadata.clientId == authority.callerId() &&
            authority.generation() == 7U;
        return Domain::Result<Domain::ToolCallOutcome>::success(
            Domain::ToolCallOutcome{
                Domain::ToolExecutionReceipt{
                    request.metadata.requestId,
                    request.toolName,
                    true,
                    std::nullopt,
                    1ms},
                "{\"ok\":true,\"text\":\"fixture\"}",
                std::nullopt,
                std::nullopt});
    }

    void cancel(const Domain::OperationId&) noexcept override { ++cancels; }
    void shutdown() noexcept override {}

    std::size_t calls{};
    std::size_t cancels{};
    bool sawBinding{};
    std::string lastName;
    std::vector<Domain::PathText> lastRoots;
    std::vector<Domain::FileAccess> lastGrants;
    bool lastShell{};
};

class ContinuityCodec final : public Contracts::IContinuityDocumentCodec {
public:
    [[nodiscard]] Domain::Result<Contracts::ContinuityDocument> encode(
        const Domain::ContinuityHandoff& handoff,
        const Domain::OperationContext&) noexcept override
    {
        return Domain::Result<Contracts::ContinuityDocument>::success(
            Contracts::ContinuityDocument{handoff, "{}"});
    }

    [[nodiscard]] Domain::Result<Contracts::ContinuityDocument> decode(
        std::string_view,
        const Domain::OperationContext&) noexcept override
    {
        return Domain::Result<Contracts::ContinuityDocument>::failure(
            Domain::makeError(Domain::ErrorCodes::InvalidRequest, "unused decode"));
    }
};

class ProjectRegistry final : public Contracts::IProjectRegistryRepository {
public:
    explicit ProjectRegistry(Domain::ProjectId projectId)
        : descriptor_{
              std::move(projectId),
              "Managed test project",
              std::nullopt,
              {parsed(Domain::PathText::create("C:\\managed-test"))}}
    {
    }

    [[nodiscard]] Domain::Result<Domain::ProjectInitialization> initialize(
        const Domain::InitializeProjectRequest&,
        const Domain::OperationContext&) noexcept override
    {
        return Domain::Result<Domain::ProjectInitialization>::failure(
            Domain::makeError(Domain::ErrorCodes::InvalidRequest, "unused initialize"));
    }

    [[nodiscard]] Domain::Result<Domain::ProjectMemoryDescriptor> descriptor(
        const Domain::ProjectId& projectId,
        const Domain::OperationContext&) noexcept override
    {
        if (projectId != descriptor_.id) {
            return Domain::Result<Domain::ProjectMemoryDescriptor>::failure(
                Domain::makeError(Domain::ErrorCodes::SessionNotFound,
                    "unexpected project"));
        }
        return Domain::Result<Domain::ProjectMemoryDescriptor>::success(descriptor_);
    }

    [[nodiscard]] Domain::Result<std::vector<Domain::ProjectMemoryDescriptor>> list(
        std::size_t,
        const Domain::OperationContext&) noexcept override
    {
        return Domain::Result<std::vector<Domain::ProjectMemoryDescriptor>>::success(
            {descriptor_});
    }

    [[nodiscard]] Domain::Result<void> detachAlias(
        const Domain::ProjectId&,
        const Domain::PathText&,
        const Domain::OperationContext&) noexcept override
    {
        return Domain::Result<void>::failure(
            Domain::makeError(Domain::ErrorCodes::InvalidRequest, "unused detach"));
    }

private:
    Domain::ProjectMemoryDescriptor descriptor_;
};

class ContinuityObserver final : public Contracts::IContinuityAutomation {
public:
    [[nodiscard]] Domain::Result<Domain::ContinuityAutomationOutcome> observe(
        const Domain::ContinuityAutomationObservation& observation,
        const Domain::OperationContext&) noexcept override
    {
        ++calls;
        retained.push_back(
            observation.budgetSignals.providerUsed.value_or(0U));
        handoffIds.push_back(observation.handoff.handoffId.value());
        for (const auto& work : observation.handoff.completedWork) {
            completedSummaries.push_back(work.summary);
        }
        Domain::ContinuityAutomationOutcome outcome{
            observation.handoff.project.projectId,
            observation.handoff.handoffId,
            Domain::ContextBudgetAction::Normal};
        if (activateSuccessor) {
            outcome.action = Domain::ContextBudgetAction::Rollover;
            outcome.rolloverRequested = true;
            outcome.successorActivated = true;
            outcome.successorProviderResponseId = parsed(
                Domain::ProviderSessionId::parse("resp_successor_root"));
        }
        return Domain::Result<Domain::ContinuityAutomationOutcome>::success(
            std::move(outcome));
    }

    void cancel(const Domain::OperationId&) noexcept override {}
    [[nodiscard]] std::size_t trackedProjectCount() const noexcept override
    {
        return calls == 0U ? 0U : 1U;
    }
    void shutdown() noexcept override {}

    std::size_t calls{};
    std::vector<std::uint64_t> retained;
    std::vector<std::string> handoffIds;
    std::vector<std::string> completedSummaries;
    bool activateSuccessor{};
};

[[nodiscard]] Domain::OperationContext context(
    const char* operation,
    const char* correlation)
{
    return Domain::OperationContext{
        parsed(Domain::OperationId::parse(operation)),
        Domain::MonotonicTimePoint::max(),
        {},
        parsed(Domain::CorrelationId::parse(correlation))};
}

[[nodiscard]] Domain::ManagedRunStartRequest request(
    const char* run,
    const char* operation,
    const char* task)
{
    return Domain::ManagedRunStartRequest{
        parsed(Domain::SessionId::parse(run)),
        parsed(Domain::ProjectId::parse(
            "11111111-1111-4111-8111-111111111111")),
        parsed(Domain::ClientId::parse("manager-owned-test")),
        parsed(Domain::OperationId::parse(operation)),
        parsed(Domain::CorrelationId::parse("managed-run-test")),
        7U,
        task};
}

[[nodiscard]] Domain::ManagedRunSnapshot waitForTerminal(
    Application::ManagedRunService& service,
    const Domain::SessionId& runId)
{
    const auto query = context(
        "99999999-9999-4999-8999-999999999999",
        "managed-run-status");
    for (std::size_t attempt = 0; attempt < 1'000U; ++attempt) {
        auto current = service.status(runId, query);
        assert(current);
        if (current.value().record.state != Domain::ManagedRunState::Running &&
            current.value().record.state !=
                Domain::ManagedRunState::Cancelling) {
            return std::move(current).value();
        }
        std::this_thread::sleep_for(5ms);
    }
    assert(false);
    return service.status(runId, query).value();
}

[[nodiscard]] Domain::ManagedRunSnapshot waitForState(
    Application::ManagedRunService& service,
    const Domain::SessionId& runId,
    const Domain::ManagedRunState expected)
{
    const auto query = context(
        "88888888-8888-4888-8888-888888888888",
        "managed-run-state");
    for (std::size_t attempt = 0; attempt < 1'000U; ++attempt) {
        auto current = service.status(runId, query);
        assert(current);
        if (current.value().record.state == expected) {
            return std::move(current).value();
        }
        std::this_thread::sleep_for(5ms);
    }
    assert(false);
    return service.status(runId, query).value();
}

} // namespace

int main()
{
    AdmissionRepository admissionRepository;
    ForgeConductor::Infrastructure::Windows::BCryptSha256Hasher hasher;
    Application::AgentRepositoryManagedRunStore durableStore{
        admissionRepository,
        parsed(Domain::AgentId::parse("forge-managed-run")),
        hasher};
    const auto admittedAt = Domain::UtcTimePoint{};
    const Domain::ManagedRunRecord durableRecord{
        parsed(Domain::SessionId::parse(
            "10101010-1010-4010-8010-101010101010")),
        parsed(Domain::ProjectId::parse(
            "20202020-2020-4020-8020-202020202020")),
        parsed(Domain::ClientId::parse("managed-admission-test")),
        "Persist a Manager-owned run.",
        3U,
        Domain::ManagedRunState::Running,
        std::nullopt,
        0U,
        0U,
        std::nullopt,
        std::nullopt,
        std::nullopt,
        {},
        admittedAt,
        admittedAt,
        false};
    const auto admissionContext = context(
        "30303030-3030-4030-8030-303030303030",
        "managed-admission-test");
    assert(durableStore.save(durableRecord, admissionContext));
    assert(admissionRepository.admittedOpen);
    assert(admissionRepository.admittedWithoutSummary);
    assert(admissionRepository.admittedWithBinding);
    assert(admissionRepository.sessionSaves == 1U);
    const auto durableLoaded = durableStore.load(
        durableRecord.runId, admissionContext);
    assert(durableLoaded && durableLoaded.value());
    assert(durableLoaded.value()->state == Domain::ManagedRunState::Failed);
    assert(durableLoaded.value()->lastError);
    assert(durableLoaded.value()->lastError->code == Domain::ErrorCodes::Conflict);
    assert(durableLoaded.value()->authorityGeneration == 3U);
    assert(durableLoaded.value()->task == durableRecord.task);
    assert(!durableLoaded.value()->allowTools);

    auto sealedRecord = durableRecord;
    sealedRecord.state = Domain::ManagedRunState::Completed;
    sealedRecord.providerResponseId = parsed(
        Domain::ProviderSessionId::parse("resp_sealed_result"));
    sealedRecord.outputText = "sealed result";
    sealedRecord.inputTokens = 12U;
    sealedRecord.outputTokens = 3U;
    assert(durableStore.save(sealedRecord, admissionContext));
    const auto sealedLoaded = durableStore.load(
        sealedRecord.runId, admissionContext);
    assert(sealedLoaded && sealedLoaded.value());
    assert(sealedLoaded.value()->evidenceSeal);
    assert(sealedLoaded.value()->evidenceIntegrity ==
        Domain::ManagedRunEvidenceIntegrity::Verified);
    auto checkedRecord = *sealedLoaded.value();
    checkedRecord.nativeTaskChecks.push_back(Domain::ManagedNativeTaskCheck{
        parsed(Domain::Sha256Digest::parse(std::string(64U, 'a'))),
        parsed(Domain::Sha256Digest::parse(std::string(64U, 'b'))),
        parsed(Domain::Sha256Digest::parse(std::string(64U, 'c'))),
        0, true, false, false, true, 17U,
        Domain::UtcTimePoint{std::chrono::milliseconds{1'700'000'000'000}}});
    assert(durableStore.save(checkedRecord, admissionContext));
    const auto checkedLoaded = durableStore.load(
        checkedRecord.runId, admissionContext);
    assert(checkedLoaded && checkedLoaded.value());
    assert(checkedLoaded.value()->nativeTaskChecks.size() == 1U);
    assert(checkedLoaded.value()->nativeTaskChecks.front().passed);
    assert(checkedLoaded.value()->nativeTaskChecks.front().exitCode == 0);
    assert(checkedLoaded.value()->evidenceIntegrity ==
        Domain::ManagedRunEvidenceIntegrity::Verified);
    admissionRepository.corruptSealedSummary();
    const auto alteredLoaded = durableStore.load(
        sealedRecord.runId, admissionContext);
    assert(alteredLoaded && alteredLoaded.value());
    assert(alteredLoaded.value()->state == Domain::ManagedRunState::Completed);
    assert(alteredLoaded.value()->evidenceIntegrity ==
        Domain::ManagedRunEvidenceIntegrity::Mismatch);

    Clock clock;
    Store store;
    Transport transport;
    Application::ManagedRunService service{transport, store, clock};

    const auto first = request(
        "aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa",
        "aaaaaaaa-bbbb-4ccc-8ddd-eeeeeeeeeeee",
        "Perform ordinary work.");
    const auto startContext = context(
        "aaaaaaaa-bbbb-4ccc-8ddd-eeeeeeeeeeee",
        "managed-run-test");
    auto started = service.start(first, startContext);
    assert(started);
    assert(started.value().managerOwned);
    assert(started.value().record.runId == first.runId);
    assert(started.value().record.projectId == first.projectId);

    auto completed = waitForTerminal(service, first.runId);
    assert(completed.record.state == Domain::ManagedRunState::Completed);
    assert(completed.record.providerResponseId);
    assert(completed.record.providerResponseId->value() == "resp_ordinary_1");
    assert(completed.record.inputTokens == 21U);
    assert(completed.record.outputTokens == 8U);
    assert(completed.record.retainedContextTokens == 29U);
    assert(completed.record.outputText == "ordinary run completed");
    assert(transport.calls == 1U);
    assert(transport.lastProject == first.projectId.value());
    assert(transport.lastRun == first.runId.value());
    assert(transport.lastGeneration == 7U);

    auto duplicate = service.start(first, startContext);
    assert(duplicate);
    assert(transport.calls == 1U);
    auto conflicting = first;
    conflicting.task = "A different task.";
    auto conflict = service.start(conflicting, startContext);
    assert(!conflict);
    assert(conflict.error().code == Domain::ErrorCodes::Conflict);

    transport.mode = Transport::Mode::Offline;
    const auto offline = request(
        "bbbbbbbb-bbbb-4bbb-8bbb-bbbbbbbbbbbb",
        "bbbbbbbb-cccc-4ddd-8eee-ffffffffffff",
        "Observe an offline failure.");
    auto offlineStarted = service.start(
        offline,
        context(
            "bbbbbbbb-cccc-4ddd-8eee-ffffffffffff",
            "managed-run-test"));
    assert(offlineStarted);
    auto failed = waitForTerminal(service, offline.runId);
    assert(failed.record.state == Domain::ManagedRunState::Failed);
    assert(failed.record.lastError);
    assert(failed.record.lastError->code ==
           Domain::ErrorCodes::TransportClosed);

    transport.mode = Transport::Mode::Block;
    const auto blocked = request(
        "cccccccc-cccc-4ccc-8ccc-cccccccccccc",
        "cccccccc-dddd-4eee-8fff-aaaaaaaaaaaa",
        "Wait until cancelled.");
    auto blockedStarted = service.start(
        blocked,
        context(
            "cccccccc-dddd-4eee-8fff-aaaaaaaaaaaa",
            "managed-run-test"));
    assert(blockedStarted);
    transport.waitForFirstToolResponse();
    // Allow the worker to publish terminal cancellation while the provider
    // cancellation callback is still in flight. Status must remain callable,
    // and the final returned snapshot must copy the published terminal record.
    transport.onCancel = [&] {
        const auto published = waitForTerminal(service, blocked.runId);
        assert(published.record.state == Domain::ManagedRunState::Cancelled);
        assert(published.record.lastError);
        assert(published.record.lastError->code == Domain::ErrorCodes::Cancelled);
    };
    auto cancellation = service.cancel(
        blocked.runId,
        context(
            "dddddddd-dddd-4ddd-8ddd-dddddddddddd",
            "managed-run-cancel"));
    assert(cancellation);
    assert(cancellation.value().cancellationRequested);
    assert(cancellation.value().record.state == Domain::ManagedRunState::Cancelled);
    assert(cancellation.value().record.lastError);
    transport.onCancel = {};
    auto cancelled = waitForTerminal(service, blocked.runId);
    assert(cancelled.record.state == Domain::ManagedRunState::Cancelled);
    assert(transport.cancels >= 1U);
    assert(store.saves >= 6U);

    service.shutdown();

    Store modelOnlyStore;
    Transport modelOnlyTransport;
    ToolCatalog modelOnlyCatalog;
    ToolRouter modelOnlyRouter;
    auto modelOnlyRequest = request(
        "d0d0d0d0-d0d0-40d0-80d0-d0d0d0d0d0d0",
        "d1d1d1d1-d1d1-41d1-81d1-d1d1d1d1d1d1",
        "Respond without native tools.");
    modelOnlyRequest.allowTools = false;
    WorkspaceAuthority modelOnlyAuthority{
        modelOnlyRequest.projectId, modelOnlyRequest.clientId};
    Application::ManagedRunService modelOnlyService{
        modelOnlyTransport, modelOnlyStore, clock,
        Application::ManagedRunToolDependencies{
            &modelOnlyCatalog, &modelOnlyRouter, &modelOnlyAuthority}};
    const auto modelOnlyContext = context(
        "d1d1d1d1-d1d1-41d1-81d1-d1d1d1d1d1d1",
        "managed-run-model-only");
    assert(modelOnlyService.start(modelOnlyRequest, modelOnlyContext));
    const auto modelOnlyCompleted = waitForTerminal(
        modelOnlyService, modelOnlyRequest.runId);
    assert(modelOnlyCompleted.record.state ==
           Domain::ManagedRunState::Completed);
    assert(!modelOnlyCompleted.record.allowTools);
    assert(modelOnlyTransport.lastToolCount == 0U);
    assert(modelOnlyAuthority.calls == 1U);
    assert(modelOnlyRouter.calls == 0U);
    auto expandedRequest = modelOnlyRequest;
    expandedRequest.allowTools = true;
    const auto expanded = modelOnlyService.start(
        expandedRequest, modelOnlyContext);
    assert(!expanded && expanded.error().code ==
           Domain::ErrorCodes::Conflict);
    modelOnlyService.shutdown();

    Store toolStore;
    Transport toolTransport;
    toolTransport.mode = Transport::Mode::ToolLoop;
    ToolCatalog toolCatalog;
    ToolRouter toolRouter;
    const auto toolRequest = request(
        "eeeeeeee-eeee-4eee-8eee-eeeeeeeeeeee",
        "eeeeeeee-ffff-4aaa-8bbb-cccccccccccc",
        "Use the controlled native tool.");
    WorkspaceAuthority toolAuthority{
        toolRequest.projectId, toolRequest.clientId};
    Application::ManagedRunService toolService{
        toolTransport,
        toolStore,
        clock,
        Application::ManagedRunToolDependencies{
            &toolCatalog, &toolRouter, &toolAuthority}};
    auto toolStarted = toolService.start(
        toolRequest,
        context(
            "eeeeeeee-ffff-4aaa-8bbb-cccccccccccc",
            "managed-run-tool-loop"));
    assert(toolStarted);
    const auto toolCompleted = waitForTerminal(toolService, toolRequest.runId);
    assert(toolCompleted.record.state == Domain::ManagedRunState::Completed);
    assert(toolCompleted.record.providerResponseId);
    assert(toolCompleted.record.providerResponseId->value() == "resp_tool_2");
    assert(toolCompleted.record.inputTokens == 70U);
    assert(toolCompleted.record.outputTokens == 12U);
    assert(toolCompleted.record.retainedContextTokens == 47U);
    assert(toolCompleted.record.outputText == "tool loop completed");
    assert(toolCompleted.record.pendingFunctionCalls.empty());
    assert(toolTransport.sawToolDescriptor);
    assert(toolTransport.sawToolOutput);
    assert(toolRouter.calls == 1U);
    assert(toolRouter.sawBinding);
    toolService.shutdown();

    Store extendedStore;
    Transport extendedTransport;
    extendedTransport.mode = Transport::Mode::ExtendedToolLoop;
    ToolRouter extendedRouter;
    const auto extendedRequest = request(
        "f0f0f0f0-f0f0-40f0-80f0-f0f0f0f0f0f0",
        "f1f1f1f1-f1f1-41f1-81f1-f1f1f1f1f1f1",
        "Complete an extended native tool workflow.");
    WorkspaceAuthority extendedAuthority{
        extendedRequest.projectId, extendedRequest.clientId};
    Application::ManagedRunService extendedService{
        extendedTransport,
        extendedStore,
        clock,
        Application::ManagedRunToolDependencies{
            &toolCatalog, &extendedRouter, &extendedAuthority}};
    assert(extendedService.start(
        extendedRequest,
        context(
            "f1f1f1f1-f1f1-41f1-81f1-f1f1f1f1f1f1",
            "managed-run-extended-tool-loop")));
    const auto extendedCompleted = waitForTerminal(
        extendedService, extendedRequest.runId);
    assert(extendedCompleted.record.state ==
           Domain::ManagedRunState::Completed);
    assert(extendedCompleted.record.outputText ==
           "extended tool loop completed");
    assert(extendedTransport.calls == 81U);
    assert(extendedRouter.calls == 80U);
    extendedService.shutdown();

    Store continuityStore;
    Transport continuityTransport;
    continuityTransport.mode = Transport::Mode::ToolLoop;
    ToolRouter continuityRouter;
    ContinuityCodec continuityCodec;
    ContinuityObserver continuityObserver;
    continuityObserver.activateSuccessor = true;
    ProjectRegistry projectRegistry{toolRequest.projectId};
    WorkspaceAuthority continuityAuthority{
        toolRequest.projectId, toolRequest.clientId};
    Application::ManagedRunService continuityService{
        continuityTransport,
        continuityStore,
        clock,
        Application::ManagedRunToolDependencies{
            &toolCatalog, &continuityRouter, &continuityAuthority},
        Application::ManagedRunContinuityDependencies{
            &continuityObserver,
            &continuityCodec,
            &projectRegistry,
            parsed(Domain::AdapterId::parse("managed-test-adapter")),
            1'000U,
            100U,
            std::optional<std::string>{"fixture-model"},
            std::optional<std::string>{"fixture-provider"}}};
    const auto continuityRequest = request(
        "12121212-1212-4212-8212-121212121212",
        "13131313-1313-4313-8313-131313131313",
        "Observe retained provider context during the tool loop.");
    assert(continuityService.start(
        continuityRequest,
        context(
            "13131313-1313-4313-8313-131313131313",
            "managed-run-continuity")));
    const auto continuityCompleted = waitForTerminal(
        continuityService, continuityRequest.runId);
    assert(continuityCompleted.record.state == Domain::ManagedRunState::Completed);
    assert(continuityCompleted.record.inputTokens == 70U);
    assert(continuityCompleted.record.outputTokens == 12U);
    assert(continuityCompleted.record.retainedContextTokens == 47U);
    assert(continuityObserver.calls == 1U);
    assert((continuityObserver.retained == std::vector<std::uint64_t>{35U}));
    assert(continuityObserver.handoffIds.size() == 1U);
    assert(continuityObserver.handoffIds.front() == continuityRequest.runId.value());
    assert(continuityObserver.completedSummaries.size() == 1U);
    assert(continuityObserver.completedSummaries.front() ==
           "Native tool fixture_read result: {\"ok\":true,\"text\":\"fixture\"}");
    assert(continuityTransport.sawSuccessorPrompt);
    continuityService.shutdown();

    Store continuityDisabledStore;
    Transport continuityDisabledTransport;
    continuityDisabledTransport.mode = Transport::Mode::ToolLoop;
    ToolRouter continuityDisabledRouter;
    ContinuityObserver continuityDisabledObserver;
    ProjectRegistry continuityDisabledRegistry{toolRequest.projectId};
    WorkspaceAuthority continuityDisabledAuthority{
        toolRequest.projectId, toolRequest.clientId};
    Application::ManagedRunService continuityDisabledService{
        continuityDisabledTransport,
        continuityDisabledStore,
        clock,
        Application::ManagedRunToolDependencies{
            &toolCatalog, &continuityDisabledRouter, &continuityDisabledAuthority},
        Application::ManagedRunContinuityDependencies{
            &continuityDisabledObserver,
            &continuityCodec,
            &continuityDisabledRegistry,
            parsed(Domain::AdapterId::parse("managed-test-adapter")),
            1'000U,
            100U,
            std::optional<std::string>{"fixture-model"},
            std::optional<std::string>{"fixture-provider"}}};
    auto continuityDisabledRequest = request(
        "14141414-1414-4414-8414-141414141414",
        "15151515-1515-4515-8515-151515151515",
        "Complete the tool loop without automatic continuity.");
    continuityDisabledRequest.automaticContinuity = false;
    assert(continuityDisabledService.start(
        continuityDisabledRequest,
        context(
            "15151515-1515-4515-8515-151515151515",
            "managed-run-continuity-disabled")));
    const auto continuityDisabledCompleted = waitForTerminal(
        continuityDisabledService, continuityDisabledRequest.runId);
    assert(continuityDisabledCompleted.record.state ==
           Domain::ManagedRunState::Completed);
    assert(continuityDisabledObserver.calls == 0U);
    assert(!continuityDisabledTransport.sawSuccessorPrompt);
    continuityDisabledService.shutdown();

    Store pauseStore;
    Transport pauseTransport;
    pauseTransport.mode = Transport::Mode::ToolLoop;
    pauseTransport.holdFirstToolResponse();
    ToolRouter pauseRouter;
    const auto pauseRequest = request(
        "ffffffff-ffff-4fff-8fff-ffffffffffff",
        "ffffffff-aaaa-4bbb-8ccc-dddddddddddd",
        "Pause before dispatching the controlled tool.");
    WorkspaceAuthority pauseAuthority{
        pauseRequest.projectId, pauseRequest.clientId};
    Application::ManagedRunService pauseService{
        pauseTransport,
        pauseStore,
        clock,
        Application::ManagedRunToolDependencies{
            &toolCatalog, &pauseRouter, &pauseAuthority}};
    assert(pauseService.start(
        pauseRequest,
        context(
            "ffffffff-aaaa-4bbb-8ccc-dddddddddddd",
            "managed-run-pause")));
    pauseTransport.waitForFirstToolResponse();
    const auto pauseAccepted = pauseService.pause(
        pauseRequest.runId,
        context(
            "77777777-7777-4777-8777-777777777777",
            "managed-run-pause"));
    assert(pauseAccepted && pauseAccepted.value().pauseRequested);
    pauseTransport.releaseFirstToolResponse();
    const auto paused = waitForState(
        pauseService, pauseRequest.runId, Domain::ManagedRunState::Paused);
    assert(paused.pauseRequested);
    assert(pauseRouter.calls == 0U);
    const auto resumed = pauseService.resume(
        pauseRequest.runId,
        context(
            "66666666-6666-4666-8666-666666666666",
            "managed-run-resume"));
    assert(resumed);
    const auto pauseCompleted = waitForTerminal(
        pauseService, pauseRequest.runId);
    assert(pauseCompleted.record.state == Domain::ManagedRunState::Completed);
    assert(pauseRouter.calls == 1U);
    pauseService.shutdown();

    Store reviewerStore;
    Transport reviewerTransport;
    reviewerTransport.mode = Transport::Mode::ReadOnlyAttack;
    ToolCatalog reviewerCatalog{true};
    ToolRouter reviewerRouter;
    auto review = request("abababab-abab-4bab-8bab-abababababab",
        "acacacac-acac-4cac-8cac-acacacacacac", "Independent opening message");
    review.readOnlyTools = true;
    review.automaticContinuity = false;
    review.providerReceiveTimeoutSeconds = 1800U;
    WorkspaceAuthority reviewerAuthority{review.projectId, review.clientId};
    Application::ManagedRunService reviewerService{reviewerTransport, reviewerStore, clock,
        {&reviewerCatalog, &reviewerRouter, &reviewerAuthority}};
    assert(reviewerService.start(review, context("acacacac-acac-4cac-8cac-acacacacacac", "reviewer-test")));
    const auto reviewed = waitForTerminal(reviewerService, review.runId);
    assert(reviewed.record.state == Domain::ManagedRunState::Completed);
    assert(reviewed.record.readOnlyTools);
    assert(reviewed.record.providerReceiveTimeoutSeconds == 1800U);
    assert(reviewerTransport.lastReceiveTimeout == 1800U);
    assert(reviewerTransport.sawFreshContext);
    assert(reviewerTransport.sawToolDescriptor);
    assert(reviewerTransport.sawDeniedWrite);
    assert(reviewerRouter.calls == 0U);
    // Idempotency cannot turn an existing reviewer into a write-capable run.
    auto changedReview = review;
    changedReview.readOnlyTools = false;
    auto reviewerConflict = reviewerService.start(changedReview,
        context("adadadad-adad-4dad-8dad-adadadadadad", "reviewer-conflict"));
    assert(!reviewerConflict && reviewerConflict.error().code == Domain::ErrorCodes::Conflict);
    reviewerService.shutdown();
    AdmissionRepository reviewerRepository;
    Application::AgentRepositoryManagedRunStore reviewerDurableStore{reviewerRepository,
        parsed(Domain::AgentId::parse("forge-managed-run")), hasher};
    assert(reviewerDurableStore.save(reviewed.record,
        context("aeaeaeae-aeae-4eae-8eae-aeaeaeaeaeae", "reviewer-persist")));
    auto restoredReview = reviewerDurableStore.load(review.runId,
        context("afafafaf-afaf-4faf-8faf-afafafafafaf", "reviewer-load"));
    assert(restoredReview && restoredReview.value());
    assert(restoredReview.value()->readOnlyTools);
    assert(restoredReview.value()->providerReceiveTimeoutSeconds == 1800U);
    assert(restoredReview.value()->evidenceIntegrity == Domain::ManagedRunEvidenceIntegrity::Verified);
    Store workerStore;
    Transport workerTransport; workerTransport.mode = Transport::Mode::WorkerAttack;
    ToolCatalog workerCatalog{true}; ToolRouter workerRouter;
    auto worker = request("11112222-3333-4444-8555-666677778888", "22223333-4444-4555-8666-777788889999", "Perform a bounded mutable task.");
    WorkspaceAuthority workerAuthority{worker.projectId, worker.clientId, true};
    const auto workerToken = parsed(workerAuthority.authorityFor(worker.projectId, context("22223333-4444-4555-8666-777788889999", "worker-scope")));
    worker.workerScope = Domain::ManagedRunWorkerScope{workerToken.trustedRoots(), workerToken.grants(), workerToken.denials(), false,
        Application::managedWorkerToolNames(workerCatalog), 600U};
    worker.automaticContinuity = false;
    Application::ManagedRunService workerService{workerTransport, workerStore, clock, {&workerCatalog, &workerRouter, &workerAuthority}};
    assert(workerService.start(worker, context("22223333-4444-4555-8666-777788889999", "worker-start")));
    const auto worked = waitForTerminal(workerService, worker.runId);
    assert(worked.record.state == Domain::ManagedRunState::Completed && worked.record.workerScope == worker.workerScope && !worked.record.readOnlyTools);
    assert(workerTransport.sawFreshContext && workerTransport.sawToolDescriptor && workerTransport.sawDeniedWrite);
    assert(workerRouter.calls == 1U && workerRouter.lastName == "fixture_write" && workerRouter.lastRoots == workerToken.trustedRoots()
        && workerRouter.lastGrants == workerToken.grants() && !workerRouter.lastShell);
    assert(worked.record.inputTokens == 22U && worked.record.outputTokens == 7U);
    auto forbiddenWorker = worker; forbiddenWorker.runId = parsed(Domain::SessionId::parse("33334444-5555-4666-8777-888899990000"));
    for (const auto forbidden : {"agent_spawn", "session_checkpoint", "session_handoff"}) {
        forbiddenWorker.workerScope = worker.workerScope;
        forbiddenWorker.workerScope->allowedTools.push_back(forbidden);
        auto refused = workerService.start(forbiddenWorker, context("44445555-6666-4777-8888-999900001111", "worker-denied"));
        assert(!refused && refused.error().code == Domain::ErrorCodes::InvalidRequest);
    }
    const auto wrongStore = reviewerDurableStore.save(worked.record, context("55556666-7777-4888-8999-000011112222", "worker-wrong-store"));
    assert(!wrongStore && wrongStore.error().code == Domain::ErrorCodes::Unauthorized);
    workerService.shutdown();
    // Cancellation must remain cancellation even before the native issuer has
    // returned a token; shutdown must seal the same outcome without inference.
    for (const bool viaShutdown : {false, true}) {
        Store cancelledStore;
        Transport cancelledTransport;
        ToolRouter cancelledRouter;
        WorkspaceAuthority resolvingAuthority{worker.projectId, worker.clientId, true};
        resolvingAuthority.waitForCancellation = true;
        Application::ManagedRunService cancelledService{cancelledTransport, cancelledStore, clock,
            {&workerCatalog, &cancelledRouter, &resolvingAuthority}};
        assert(cancelledService.start(worker, context("22223333-4444-4555-8666-777788889999", "worker-resolution-cancel")));
        resolvingAuthority.waitUntilResolving();
        if (viaShutdown) cancelledService.shutdown();
        else assert(cancelledService.cancel(worker.runId,
            context("55556666-7777-4888-8999-000011112222", "worker-resolution-cancel")));
        const auto stopped = waitForTerminal(cancelledService, worker.runId);
        assert(stopped.record.state == Domain::ManagedRunState::Cancelled && stopped.record.lastError &&
            stopped.record.lastError->code == Domain::ErrorCodes::Cancelled);
        assert(cancelledTransport.calls == 0U && cancelledRouter.calls == 0U && resolvingAuthority.calls == 1U);
        cancelledService.shutdown();
    }
    // The provider can return a tool after owner policy changes. Mutable
    // desktop/network handlers must never receive that stale capability.
    for (const int revoke : {1, 2}) {
        Store revokedStore;
        Transport revokedTransport;
        revokedTransport.mode = Transport::Mode::WorkerAttack;
        revokedTransport.holdFirstToolResponse();
        WorkspaceAuthority revokedAuthority{worker.projectId, worker.clientId, true, true};
        ToolRouter revokedRouter;
        auto admitted = worker;
        const auto originalScope = parsed(revokedAuthority.authorityFor(worker.projectId,
            context("22223333-4444-4555-8666-777788889999", "worker-original-policy")));
        admitted.workerScope = Domain::ManagedRunWorkerScope{originalScope.trustedRoots(), originalScope.grants(),
            originalScope.denials(), originalScope.shellEnabled(), Application::managedWorkerToolNames(workerCatalog), 600U};
        Application::ManagedRunService revokedService{revokedTransport, revokedStore, clock,
            {&workerCatalog, &revokedRouter, &revokedAuthority}};
        assert(revokedService.start(admitted,
            context("22223333-4444-4555-8666-777788889999", "worker-revoked-policy")));
        revokedTransport.waitForFirstToolResponse();
        revokedAuthority.revokedPolicy.store(revoke);
        revokedTransport.releaseFirstToolResponse();
        const auto rejected = waitForTerminal(revokedService, admitted.runId);
        assert(rejected.record.state == Domain::ManagedRunState::Failed && rejected.record.lastError &&
            rejected.record.lastError->code == Domain::ErrorCodes::Unauthorized && revokedRouter.calls == 0U &&
            revokedTransport.calls == 1U && rejected.record.workerScope == admitted.workerScope &&
            !rejected.record.pendingFunctionCalls.empty());
        revokedService.shutdown();
    }
    // Completed workers retain durable results, while their finished threads
    // no longer consume admission slots or receive shutdown cancellation.
    Store retainedStore;
    Transport boundedTransport;
    WorkspaceAuthority boundedAuthority{worker.projectId, worker.clientId, true};
    Application::ManagedRunService boundedService{boundedTransport, retainedStore, clock,
        {&workerCatalog, &workerRouter, &boundedAuthority}};
    const auto boundedWorker = [&](const unsigned index) {
        const auto suffix = std::to_string(index);
        const auto digits = std::string(12U - suffix.size(), '0') + suffix;
        auto admitted = worker;
        admitted.runId = parsed(Domain::SessionId::parse("50000000-0000-4000-8000-" + digits));
        admitted.operationId = parsed(Domain::OperationId::parse("60000000-0000-4000-8000-" + digits));
        admitted.task = "Bounded independent worker resource fixture.";
        return admitted;
    };
    for (unsigned index = 1U; index <= 40U; ++index) {
        const auto next = boundedWorker(index);
        assert(boundedService.start(next, context("22223333-4444-4555-8666-777788889999", "worker-retirement")));
        assert(waitForTerminal(boundedService, next.runId).record.state == Domain::ManagedRunState::Completed);
    }
    assert(boundedTransport.cancels == 0U);
    boundedTransport.mode = Transport::Mode::Block;
    static_assert(Application::ManagedRunService::MaximumConcurrentWorkers == 16U);
    for (unsigned index = 100U; index < 116U; ++index)
        assert(boundedService.start(boundedWorker(index),
            context("22223333-4444-4555-8666-777788889999", "worker-active-bound")));
    const auto excessive = boundedWorker(116U);
    const auto rejected = boundedService.start(excessive,
        context("22223333-4444-4555-8666-777788889999", "worker-active-bound"));
    assert(!rejected && rejected.error().code == Domain::ErrorCodes::LimitExceeded);
    assert(!parsed(retainedStore.load(excessive.runId,
        context("22223333-4444-4555-8666-777788889999", "worker-no-orphan"))));
    const auto firstActive = boundedWorker(100U);
    assert(boundedService.cancel(firstActive.runId,
        context("22223333-4444-4555-8666-777788889999", "worker-release-slot")));
    assert(waitForTerminal(boundedService, firstActive.runId).record.state == Domain::ManagedRunState::Cancelled);
    assert(boundedService.start(excessive,
        context("22223333-4444-4555-8666-777788889999", "worker-reused-slot")));
    boundedService.shutdown();
    assert(boundedTransport.cancels == 17U);
    assert(parsed(boundedService.status(boundedWorker(1U).runId,
        context("22223333-4444-4555-8666-777788889999", "worker-retained-result"))).record.state ==
        Domain::ManagedRunState::Completed);
    assert(waitForTerminal(boundedService, excessive.runId).record.state == Domain::ManagedRunState::Cancelled);
    return 0;
}
