#include "ForgeConductor/Application/AgentRepositoryManagedRunStore.h"
#include "ForgeConductor/Application/ManagedRunService.h"
#include "ForgeConductor/Domain/Utf8.h"
#include "ForgeConductor/Infrastructure/Windows/BCryptSha256Hasher.h"
#include "../Fakes/RecordingProjectMemoryService.h"

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <barrier>
#include <chrono>
#include <condition_variable>
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
namespace TestFakes = ForgeConductor::Tests::Fakes;

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
        if (!records_.contains(record.runId)) {
            ++initialAdmissions;
        }
        records_.insert_or_assign(record.runId, record);
        ++saves;
        return Domain::Result<void>::success();
    }

    std::size_t saves{};
    std::size_t initialAdmissions{};

    void seed(Domain::ManagedRunRecord record)
    {
        const std::lock_guard lock{mutex_};
        records_.insert_or_assign(record.runId, std::move(record));
    }

    [[nodiscard]] std::optional<Domain::ManagedRunRecord> record(
        const Domain::SessionId& runId)
    {
        const std::lock_guard lock{mutex_};
        const auto found = records_.find(runId);
        return found == records_.end()
            ? std::nullopt
            : std::optional<Domain::ManagedRunRecord>{found->second};
    }

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
        admittedWithInitialSummary = mutation.initialSummary.has_value();
        admittedWithBinding = mutation.activeBinding &&
            mutation.run.session.clientId &&
            mutation.activeBinding->sessionId == mutation.run.session.id &&
            mutation.activeBinding->agentId == mutation.run.session.agentId &&
            mutation.activeBinding->goal == *mutation.run.goal;
        if (!admittedOpen || !admittedWithoutSummary ||
            !admittedWithInitialSummary || !admittedWithBinding) {
            return Domain::Result<
                Domain::AgentRunStartPersistenceOutcome>::failure(
                Domain::makeError(
                    Domain::ErrorCodes::InvalidRequest,
                    "The admission mutation violated the repository contract."));
        }
        run_ = mutation.run;
        run_->session.summary = mutation.initialSummary;
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
    bool admittedWithInitialSummary{};
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
    enum class Mode { Success, Offline, Block, ToolLoop, ExtendedToolLoop };

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
            lastOperation = context.operationId.value();
            lastCorrelation = context.correlationId.value();
            if (request.input.find("[ORIGINAL MANAGER TASK]") !=
                std::string::npos) {
                successorInput = request.input;
            }
        }
        if (mode == Mode::Block) {
            std::unique_lock lock{mutex_};
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
                request.input.find("Do not return a status object") !=
                    std::string::npos &&
                request.input.find(
                    "Observe retained provider context during the tool loop.") !=
                    std::string::npos &&
                request.input.find(
                    "Native tool fixture_read result: "
                    "{\"ok\":true,\"text\":\"fixture\"}") !=
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
            if (calls <= extendedToolCallLimit) {
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
    }

    Mode mode{Mode::Success};
    std::size_t extendedToolCallLimit{80U};
    std::size_t calls{};
    std::size_t cancels{};
    std::string lastProject;
    std::string lastRun;
    std::uint64_t lastGeneration{};
    std::size_t lastToolCount{};
    std::string lastOperation;
    std::string lastCorrelation;
    std::string successorInput;
    bool sawToolDescriptor{};
    bool sawToolOutput{};
    bool sawSuccessorPrompt{};

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
    ToolCatalog()
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
    std::size_t calls{};
    WorkspaceAuthority(Domain::ProjectId projectId, Domain::ClientId clientId)
        : projectId_{std::move(projectId)}, clientId_{std::move(clientId)}
    {
    }

    [[nodiscard]] Domain::Result<Contracts::WorkspaceAuthority> authorityFor(
        const Domain::ProjectId& projectId,
        const Domain::OperationContext&) noexcept override
    {
        ++calls;
        if (projectId != projectId_) {
            return Domain::Result<Contracts::WorkspaceAuthority>::failure(
                Domain::makeError(Domain::ErrorCodes::ProjectScopeMismatch,
                                  "unexpected test project"));
        }
        return issueAuthority(
            parsed(Domain::AuthorityId::parse(
                "22222222-2222-4222-8222-222222222222")),
            projectId_,
            clientId_,
            {parsed(Domain::PathText::create("C:\\managed-test"))},
            Domain::FileAccess::Read,
            {Domain::FileAccess::Read},
            {},
            false,
            7U);
    }

    [[nodiscard]] Domain::Result<Contracts::WorkspaceAuthority> narrow(
        const Contracts::WorkspaceAuthority&,
        const std::vector<Domain::PathText>&,
        const std::vector<Domain::FileAccess>&,
        bool,
        std::uint64_t,
        const Domain::OperationContext&) noexcept override
    {
        return Domain::Result<Contracts::WorkspaceAuthority>::failure(
            Domain::makeError(Domain::ErrorCodes::HostCapabilityUnavailable,
                              "unused test narrow"));
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
};
class ToolRouter final : public Contracts::IToolRouter {
public:
    [[nodiscard]] Domain::Result<Domain::ToolCallOutcome> invoke(
        const Domain::ToolCallRequest& request,
        const Contracts::WorkspaceAuthority& authority,
        const Domain::OperationContext&) noexcept override
    {
        ++calls;
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
                canonicalOutput,
                std::nullopt,
                std::nullopt});
    }

    void cancel(const Domain::OperationId&) noexcept override { ++cancels; }
    void shutdown() noexcept override {}

    std::size_t calls{};
    std::size_t cancels{};
    bool sawBinding{};
    std::string canonicalOutput{"{\"ok\":true,\"text\":\"fixture\"}"};
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
        missions.push_back(observation.handoff.mission);
        predecessorSessionIds.push_back(
            observation.handoff.predecessorSession.sessionId.value());
        for (const auto& work : observation.handoff.completedWork) {
            completedSummaries.push_back(work.summary);
        }
        if (observeError) {
            return Domain::Result<
                Domain::ContinuityAutomationOutcome>::failure(*observeError);
        }
        Domain::ContinuityAutomationOutcome outcome{
            observation.handoff.project.projectId,
            observation.handoff.handoffId,
            Domain::ContextBudgetAction::Normal};
        const auto action = calls <= scriptedActions.size()
            ? scriptedActions[calls - 1U]
            : (activateSuccessor
                ? Domain::ContextBudgetAction::Rollover
                : Domain::ContextBudgetAction::Normal);
        outcome.action = action;
        if (action == Domain::ContextBudgetAction::Checkpoint) {
            outcome.checkpointPersisted = true;
            outcome.operationId = observation.handoff.operationId;
        }
        if (action == Domain::ContextBudgetAction::Rollover ||
            action == Domain::ContextBudgetAction::Emergency) {
            outcome.checkpointPersisted = true;
            outcome.operationId = observation.handoff.operationId;
            const auto successor = scriptedActions.empty()
                ? parsed(Domain::SessionId::parse(
                    "23232323-2323-4323-8323-232323232323"))
                : parsed(Domain::SessionId::parse(
                    successorSessionIds.empty()
                        ? "24242424-2424-4424-8424-242424242424"
                        : "25252525-2525-4525-8525-252525252525"));
            outcome.rolloverRequested = true;
            outcome.successorActivated = true;
            outcome.successorSessionId = successor;
            outcome.successorProviderResponseId = parsed(
                Domain::ProviderSessionId::parse(
                    scriptedActions.empty()
                        ? "resp_successor_root"
                        : "resp_successor_" + std::to_string(calls)));
            successorSessionIds.push_back(successor.value());
        }
        return Domain::Result<Domain::ContinuityAutomationOutcome>::success(
            std::move(outcome));
    }

    [[nodiscard]] Domain::Result<void> abandonCheckpoint(
        const Domain::ProjectId& projectId,
        const Domain::ContinuityOperationId& operationId,
        const Domain::OperationContext&) noexcept override
    {
        ++abandonCalls;
        abandonedProjectId = projectId.value();
        abandonedOperationId = operationId.value();
        if (abandonError) {
            return Domain::Result<void>::failure(*abandonError);
        }
        return Domain::Result<void>::success();
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
    std::vector<std::string> missions;
    std::vector<std::string> predecessorSessionIds;
    std::vector<std::string> successorSessionIds;
    std::vector<std::string> completedSummaries;
    std::vector<Domain::ContextBudgetAction> scriptedActions;
    std::size_t abandonCalls{};
    std::string abandonedProjectId;
    std::string abandonedOperationId;
    std::optional<Domain::Error> observeError;
    std::optional<Domain::Error> abandonError;
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

[[nodiscard]] Domain::ProjectMemoryRecord cursorRecord(
    const Domain::ProjectId& projectId,
    const Domain::MemoryRecordId& recordId,
    const std::uint32_t version,
    const std::string& queueRowId,
    const std::uint64_t cursorEntry,
    const std::uint64_t entryCount,
    const std::optional<std::string>& correlationId = std::nullopt,
    const std::optional<std::string>& managedRunId = std::nullopt)
{
    const auto now = Domain::UtcTimePoint{};
    const auto completed = cursorEntry >= entryCount;
    return Domain::ProjectMemoryRecord{
        recordId,
        projectId,
        version,
        "instruction_package_queue",
        "Managed admission fixture",
        "Managed admission cursor fixture",
        std::string{"{\"schema\":\"forge-instruction-package-queue-v2\","} +
            "\"project_id\":\"" + projectId.value() +
            "\",\"queue_row_id\":\"" + queueRowId +
            "\",\"state\":\"" +
            (completed ? "completed" : "active") +
            "\",\"cursor\":{\"entry\":" +
            std::to_string(cursorEntry) +
            ",\"byte_offset\":0},\"entry_count\":" +
            std::to_string(entryCount) +
            (correlationId
                ? ",\"correlation_id\":\"" + *correlationId + "\""
                : std::string{}) +
            (managedRunId
                ? ",\"managed_run_id\":\"" + *managedRunId + "\""
                : std::string{}) +
            ",\"last_error\":null}",
        {"instruction-package"},
        1.0,
        1.0,
        "managed_run_test",
        std::nullopt,
        std::nullopt,
        now,
        now,
        now,
        std::nullopt,
        parsed(Domain::Sha256Digest::parse(std::string(64U, 'a'))),
        false,
        Domain::ProjectMemorySchemaVersion};
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
    static_assert(Domain::AgentSessionLimits::MaximumGoalBytes ==
        Domain::MaximumManagedRunTaskBytes);
    Domain::AgentRunStartRequest maximumManagedGoal{
        parsed(Domain::AgentId::parse("forge-managed-run")),
        parsed(Domain::ClientId::parse("managed-goal-boundary")),
        parsed(Domain::ProjectId::parse(
            "90909090-9090-4090-8090-909090909090")),
        std::string(Domain::MaximumManagedRunTaskBytes, 'x'),
        std::nullopt};
    assert(Domain::validateAgentRunStartRequest(maximumManagedGoal));
    maximumManagedGoal.goal.push_back('x');
    const auto oversizedManagedGoal =
        Domain::validateAgentRunStartRequest(maximumManagedGoal);
    assert(!oversizedManagedGoal);
    assert(oversizedManagedGoal.error().code ==
        Domain::ErrorCodes::PayloadTooLarge);

    AdmissionRepository admissionRepository;
    ForgeConductor::Infrastructure::Windows::BCryptSha256Hasher hasher;
    Application::AgentRepositoryManagedRunStore durableStore{
        admissionRepository,
        parsed(Domain::AgentId::parse("forge-managed-run")),
        hasher};
    const auto admittedAt = Domain::UtcTimePoint{};
    Domain::ManagedRunRecord durableRecord{
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
    durableRecord.automaticContinuity = false;
    durableRecord.admissionIdentity = parsed(
        Domain::Sha256Digest::parse(std::string(64U, 'd')));
    durableRecord.dispatchPending = true;
    durableRecord.dispatchPhase =
        Domain::ManagedRunDispatchPhase::CursorPending;
    durableRecord.instructionCursorAdvances = {
        {parsed(Domain::MemoryRecordId::parse(
             "21212121-2121-4121-8121-212121212121")),
         1U, "durable-queue", 2U, true}};
    const auto admissionContext = context(
        "30303030-3030-4030-8030-303030303030",
        "managed-admission-test");
    durableRecord.dispatchOperationId = admissionContext.operationId;
    durableRecord.dispatchCorrelationId = admissionContext.correlationId;
    assert(durableStore.save(durableRecord, admissionContext));
    assert(admissionRepository.admittedOpen);
    assert(admissionRepository.admittedWithoutSummary);
    assert(admissionRepository.admittedWithInitialSummary);
    assert(admissionRepository.admittedWithBinding);
    assert(admissionRepository.sessionSaves == 0U);
    const auto durableLoaded = durableStore.load(
        durableRecord.runId, admissionContext);
    assert(durableLoaded && durableLoaded.value());
    assert(durableLoaded.value()->state == Domain::ManagedRunState::Running);
    assert(!durableLoaded.value()->lastError);
    assert(durableLoaded.value()->authorityGeneration == 3U);
    assert(durableLoaded.value()->task == durableRecord.task);
    assert(!durableLoaded.value()->allowTools);
    assert(!durableLoaded.value()->automaticContinuity);
    assert(durableLoaded.value()->admissionIdentity ==
        durableRecord.admissionIdentity);
    assert(durableLoaded.value()->dispatchPending);
    assert(durableLoaded.value()->dispatchPhase ==
        Domain::ManagedRunDispatchPhase::CursorPending);
    assert(durableLoaded.value()->dispatchOperationId ==
        durableRecord.dispatchOperationId);
    assert(durableLoaded.value()->dispatchCorrelationId ==
        durableRecord.dispatchCorrelationId);
    assert(durableLoaded.value()->instructionCursorAdvances ==
        durableRecord.instructionCursorAdvances);

    auto sealedRecord = durableRecord;
    sealedRecord.dispatchPending = false;
    sealedRecord.instructionCursorAdvances.clear();
    sealedRecord.dispatchPhase =
        Domain::ManagedRunDispatchPhase::ProviderClaimed;
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
    assert(started.value().disposition ==
        Domain::ManagedRunAdmissionDisposition::Admitted);
    assert(started.value().snapshot.managerOwned);
    assert(started.value().snapshot.record.runId == first.runId);
    assert(started.value().snapshot.record.projectId == first.projectId);

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
    assert(duplicate.value().disposition ==
        Domain::ManagedRunAdmissionDisposition::Replayed);
    assert(transport.calls == 1U);
    auto continuityConflictRequest = first;
    continuityConflictRequest.automaticContinuity =
        !first.automaticContinuity;
    auto continuityConflict = service.start(
        continuityConflictRequest, startContext);
    assert(!continuityConflict);
    assert(continuityConflict.error().code == Domain::ErrorCodes::Conflict);
    auto conflicting = first;
    conflicting.task = "A different task.";
    auto conflict = service.start(conflicting, startContext);
    assert(!conflict);
    assert(conflict.error().code == Domain::ErrorCodes::Conflict);

    auto stableAdmission = request(
        "abababab-abab-4bab-8bab-abababababab",
        "acacacac-acac-4cac-8cac-acacacacacac",
        "Original pre-enrichment task.");
    stableAdmission.admissionIdentity = parsed(
        Domain::Sha256Digest::parse(std::string(64U, 'e')));
    const auto stableContext = context(
        "acacacac-acac-4cac-8cac-acacacacacac",
        "managed-run-stable-admission");
    const auto missingStableReplay =
        service.resolveReplay(stableAdmission, stableContext);
    assert(missingStableReplay && !missingStableReplay.value());
    const auto stableStarted = service.start(stableAdmission, stableContext);
    assert(stableStarted);
    const auto stableCompleted = waitForTerminal(
        service, stableAdmission.runId);
    assert(stableCompleted.record.admissionIdentity ==
        stableAdmission.admissionIdentity);
    const auto callsAfterStableAdmission = transport.calls;
    auto stableReplay = stableAdmission;
    stableReplay.operationId = parsed(Domain::OperationId::parse(
        "adadadad-adad-4dad-8dad-adadadadadad"));
    stableReplay.task = "A different post-enrichment snapshot.";
    stableReplay.automaticContinuity = !stableAdmission.automaticContinuity;
    const auto resolvedStableReplay =
        service.resolveReplay(stableReplay, stableContext);
    assert(resolvedStableReplay && resolvedStableReplay.value());
    assert(resolvedStableReplay.value()->record.task == stableAdmission.task);
    assert(resolvedStableReplay.value()->record.automaticContinuity ==
        stableAdmission.automaticContinuity);
    const auto stableReplayed = service.start(stableReplay, stableContext);
    assert(stableReplayed);
    assert(stableReplayed.value().disposition ==
        Domain::ManagedRunAdmissionDisposition::Replayed);
    assert(stableReplayed.value().snapshot.record.task == stableAdmission.task);
    assert(stableReplayed.value().snapshot.record.automaticContinuity ==
        stableAdmission.automaticContinuity);
    assert(transport.calls == callsAfterStableAdmission);
    auto stableConflict = stableReplay;
    stableConflict.admissionIdentity = parsed(
        Domain::Sha256Digest::parse(std::string(64U, 'f')));
    const auto resolvedStableConflict =
        service.resolveReplay(stableConflict, stableContext);
    assert(!resolvedStableConflict);
    assert(resolvedStableConflict.error().code ==
        Domain::ErrorCodes::Conflict);
    const auto stableConflictResult = service.start(
        stableConflict, stableContext);
    assert(!stableConflictResult);
    assert(stableConflictResult.error().code ==
        Domain::ErrorCodes::Conflict);

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
    auto cancellation = service.cancel(
        blocked.runId,
        context(
            "dddddddd-dddd-4ddd-8ddd-dddddddddddd",
            "managed-run-cancel"));
    assert(cancellation);
    assert(cancellation.value().cancellationRequested);
    auto cancelled = waitForTerminal(service, blocked.runId);
    assert(cancelled.record.state == Domain::ManagedRunState::Cancelled);
    assert(transport.cancels >= 1U);
    assert(store.saves >= 6U);

    service.shutdown();

    Transport durableReplayTransport;
    Application::ManagedRunService durableReplayService{
        durableReplayTransport, store, clock};
    auto durableContinuityConflict = service.start(
        continuityConflictRequest, startContext);
    assert(!durableContinuityConflict);
    assert(durableContinuityConflict.error().code ==
        Domain::ErrorCodes::TransportClosed);
    durableContinuityConflict = durableReplayService.start(
        continuityConflictRequest, startContext);
    assert(!durableContinuityConflict);
    assert(durableContinuityConflict.error().code ==
        Domain::ErrorCodes::Conflict);
    const auto resolvedDurableStableReplay =
        durableReplayService.resolveReplay(stableReplay, stableContext);
    assert(resolvedDurableStableReplay &&
        resolvedDurableStableReplay.value());
    assert(resolvedDurableStableReplay.value()->record.task ==
        stableAdmission.task);
    const auto durableStableReplay = durableReplayService.start(
        stableReplay, stableContext);
    assert(durableStableReplay);
    assert(durableStableReplay.value().disposition ==
        Domain::ManagedRunAdmissionDisposition::Replayed);
    assert(durableStableReplay.value().snapshot.record.task ==
        stableAdmission.task);
    assert(durableStableReplay.value().snapshot.record.automaticContinuity ==
        stableAdmission.automaticContinuity);
    assert(durableReplayTransport.calls == 0U);
    durableReplayService.shutdown();

    {
        Store concurrentStore;
        Transport concurrentTransport;
        concurrentTransport.mode = Transport::Mode::Block;
        Application::ManagedRunService concurrentService{
            concurrentTransport, concurrentStore, clock};
        auto concurrentRequest = request(
            "41414141-4141-4141-8141-414141414141",
            "42424242-4242-4242-8242-424242424242",
            "Admit one concurrent request.");
        concurrentRequest.admissionIdentity = parsed(
            Domain::Sha256Digest::parse(std::string(64U, '1')));
        const auto concurrentContext = context(
            "42424242-4242-4242-8242-424242424242",
            "concurrent-same-admission");
        std::barrier gate{3};
        std::optional<Domain::Result<Domain::ManagedRunStartOutcome>> left;
        std::optional<Domain::Result<Domain::ManagedRunStartOutcome>> right;
        std::thread leftThread{[&] {
            gate.arrive_and_wait();
            left.emplace(concurrentService.start(
                concurrentRequest, concurrentContext));
        }};
        std::thread rightThread{[&] {
            gate.arrive_and_wait();
            right.emplace(concurrentService.start(
                concurrentRequest, concurrentContext));
        }};
        gate.arrive_and_wait();
        leftThread.join();
        rightThread.join();
        assert(left && right && *left && *right);
        const auto admittedCount =
            ((*left).value().disposition ==
                Domain::ManagedRunAdmissionDisposition::Admitted ? 1U : 0U) +
            ((*right).value().disposition ==
                Domain::ManagedRunAdmissionDisposition::Admitted ? 1U : 0U);
        const auto replayedCount =
            ((*left).value().disposition ==
                Domain::ManagedRunAdmissionDisposition::Replayed ? 1U : 0U) +
            ((*right).value().disposition ==
                Domain::ManagedRunAdmissionDisposition::Replayed ? 1U : 0U);
        assert(admittedCount == 1U && replayedCount == 1U);
        assert(concurrentStore.initialAdmissions == 1U);
        concurrentService.shutdown();
    }

    {
        Store conflictingStore;
        Transport conflictingTransport;
        conflictingTransport.mode = Transport::Mode::Block;
        Application::ManagedRunService conflictingService{
            conflictingTransport, conflictingStore, clock};
        auto leftRequest = request(
            "43434343-4343-4343-8343-434343434343",
            "44444444-4444-4444-8444-444444444444",
            "Admit the winning concurrent identity.");
        leftRequest.admissionIdentity = parsed(
            Domain::Sha256Digest::parse(std::string(64U, '2')));
        auto rightRequest = leftRequest;
        rightRequest.task = "Do not overwrite the winning admission.";
        rightRequest.admissionIdentity = parsed(
            Domain::Sha256Digest::parse(std::string(64U, '3')));
        const auto conflictingContext = context(
            "44444444-4444-4444-8444-444444444444",
            "concurrent-conflicting-admission");
        std::barrier gate{3};
        std::optional<Domain::Result<Domain::ManagedRunStartOutcome>> left;
        std::optional<Domain::Result<Domain::ManagedRunStartOutcome>> right;
        std::thread leftThread{[&] {
            gate.arrive_and_wait();
            left.emplace(conflictingService.start(
                leftRequest, conflictingContext));
        }};
        std::thread rightThread{[&] {
            gate.arrive_and_wait();
            right.emplace(conflictingService.start(
                rightRequest, conflictingContext));
        }};
        gate.arrive_and_wait();
        leftThread.join();
        rightThread.join();
        assert(left && right);
        assert(static_cast<bool>(*left) != static_cast<bool>(*right));
        const auto& rejected = *left ? *right : *left;
        assert(!rejected &&
            rejected.error().code == Domain::ErrorCodes::Conflict);
        assert(conflictingStore.initialAdmissions == 1U);
        conflictingService.shutdown();
    }

    {
        Store cursorStore;
        Transport cursorTransport;
        ToolRouter cursorRouter;
        TestFakes::RecordingProjectMemoryService cursorMemory;
        auto cursorRequest = request(
            "45454545-4545-4545-8545-454545454545",
            "46464646-4646-4646-8646-464646464646",
            "Commit package cursors before provider dispatch.");
        cursorRequest.admissionIdentity = parsed(
            Domain::Sha256Digest::parse(std::string(64U, '4')));
        const auto cursorA = parsed(Domain::MemoryRecordId::parse(
            "47474747-4747-4747-8747-474747474747"));
        const auto cursorB = parsed(Domain::MemoryRecordId::parse(
            "48484848-4848-4848-8848-484848484848"));
        const auto oldA = cursorRecord(
            cursorRequest.projectId, cursorA, 1U, "queue-a", 0U, 1U);
        const auto oldB = cursorRecord(
            cursorRequest.projectId, cursorB, 1U, "queue-b", 0U, 1U);
        cursorRequest.instructionCursorAdvances = {
            {cursorA, 1U, "queue-a", 1U, true},
            {cursorB, 1U, "queue-b", 1U, true}};
        cursorMemory.getResult.set(
            Domain::Result<Domain::MemoryRecords>::success(
                Domain::MemoryRecords{
                    cursorRequest.projectId, {oldA, oldB}, 2'048U,
                    256U * 1024U,
                    Domain::ProjectMemorySchemaVersion,
                    Domain::ProjectMemoryCapabilityVersion}));
        cursorMemory.updateBatchResult.set(
            Domain::Result<Domain::MemoryUpdateBatchOutcome>::failure(
                Domain::makeError(
                    Domain::ErrorCodes::Conflict,
                    "injected cursor batch failure")));
        Application::ManagedRunService cursorService{
            cursorTransport,
            cursorStore,
            clock,
            Application::ManagedRunToolDependencies{
                nullptr, &cursorRouter, nullptr, &cursorMemory}};
        const auto cursorContext = context(
            "46464646-4646-4646-8646-464646464646",
            "cursor-admission");
        const auto cursorFailed =
            cursorService.start(cursorRequest, cursorContext);
        assert(!cursorFailed &&
            cursorFailed.error().code == Domain::ErrorCodes::Conflict);
        assert(cursorTransport.calls == 0U);
        assert(cursorRouter.calls == 0U);
        const auto pendingRecord =
            cursorStore.record(cursorRequest.runId);
        assert(pendingRecord && pendingRecord->dispatchPending);
        assert(pendingRecord->instructionCursorAdvances.size() == 2U);
        cursorService.shutdown();

        cursorMemory.updateBatchResult.set(
            Domain::Result<Domain::MemoryUpdateBatchOutcome>::success(
                Domain::MemoryUpdateBatchOutcome{
                    cursorRequest.projectId, {oldA, oldB},
                    Domain::ProjectMemorySchemaVersion,
                    Domain::ProjectMemoryCapabilityVersion}));
        Application::ManagedRunService cursorReplayService{
            cursorTransport,
            cursorStore,
            clock,
            Application::ManagedRunToolDependencies{
                nullptr, &cursorRouter, nullptr, &cursorMemory}};
        const auto cursorRetried =
            cursorReplayService.resolveReplay(
                cursorRequest, cursorContext);
        assert(cursorRetried && cursorRetried.value());
        const auto cursorCompleted =
            waitForTerminal(cursorReplayService, cursorRequest.runId);
        assert(cursorCompleted.record.state ==
            Domain::ManagedRunState::Completed);
        assert(cursorTransport.calls == 1U);
        assert(cursorMemory.callCount(
            TestFakes::ProjectMemoryCall::UpdateBatch) == 2U);
        const auto releasedRecord =
            cursorStore.record(cursorRequest.runId);
        assert(releasedRecord && !releasedRecord->dispatchPending);
        assert(releasedRecord->instructionCursorAdvances.empty());
        cursorReplayService.shutdown();
    }

    {
        auto pendingRequest = request(
            "49494949-4949-4949-8949-494949494949",
            "50505050-5050-4050-8050-505050505050",
            "Resume an already-applied pending cursor plan.");
        pendingRequest.admissionIdentity = parsed(
            Domain::Sha256Digest::parse(std::string(64U, '5')));
        const auto cursorId = parsed(Domain::MemoryRecordId::parse(
            "51515151-5151-4151-8151-515151515151"));
        pendingRequest.instructionCursorAdvances = {
            {cursorId, 1U, "queue-applied", 1U, true}};
        Domain::ManagedRunRecord pendingRecord{
            pendingRequest.runId,
            pendingRequest.projectId,
            pendingRequest.clientId,
            pendingRequest.task,
            pendingRequest.authorityGeneration,
            Domain::ManagedRunState::Running,
            std::nullopt,
            0U,
            0U,
            std::nullopt,
            std::nullopt,
            std::nullopt,
            {},
            clock.utcNow(),
            clock.utcNow(),
            pendingRequest.allowTools};
        pendingRecord.automaticContinuity =
            pendingRequest.automaticContinuity;
        pendingRecord.admissionIdentity =
            pendingRequest.admissionIdentity;
        pendingRecord.dispatchPending = true;
        pendingRecord.dispatchPhase =
            Domain::ManagedRunDispatchPhase::CursorPending;
        pendingRecord.dispatchOperationId = pendingRequest.operationId;
        pendingRecord.dispatchCorrelationId = pendingRequest.correlationId;
        pendingRecord.instructionCursorAdvances =
            pendingRequest.instructionCursorAdvances;
        Store pendingStore;
        pendingStore.seed(pendingRecord);
        Transport pendingTransport;
        TestFakes::RecordingProjectMemoryService pendingMemory;
        const auto alreadyApplied = cursorRecord(
            pendingRequest.projectId, cursorId, 2U,
            "queue-applied", 1U, 1U,
            pendingRequest.correlationId.value(),
            pendingRequest.runId.value());
        pendingMemory.getResult.set(
            Domain::Result<Domain::MemoryRecords>::success(
                Domain::MemoryRecords{
                    pendingRequest.projectId, {alreadyApplied}, 1'024U,
                    256U * 1024U,
                    Domain::ProjectMemorySchemaVersion,
                    Domain::ProjectMemoryCapabilityVersion}));
        Application::ManagedRunService pendingService{
            pendingTransport,
            pendingStore,
            clock,
            Application::ManagedRunToolDependencies{
                nullptr, nullptr, nullptr, &pendingMemory}};
        const auto pendingContext = context(
            "50505050-5050-4050-8050-505050505050",
            "pending-cursor-restart");
        const auto resumed =
            pendingService.resolveReplay(pendingRequest, pendingContext);
        assert(resumed && resumed.value());
        const auto resumedCompleted =
            waitForTerminal(pendingService, pendingRequest.runId);
        assert(resumedCompleted.record.state ==
            Domain::ManagedRunState::Completed);
        assert(pendingTransport.calls == 1U);
        assert(pendingMemory.callCount(
            TestFakes::ProjectMemoryCall::Update) == 0U);
        pendingService.shutdown();
    }

    {
        auto foreignRequest = request(
            "51515151-5151-4151-8151-515151515152",
            "50505050-5050-4050-8050-505050505051",
            "Reject a cursor advance committed by another admission.");
        foreignRequest.correlationId = parsed(
            Domain::CorrelationId::parse("cursor-owner-a"));
        foreignRequest.admissionIdentity = parsed(
            Domain::Sha256Digest::parse(std::string(64U, '8')));
        const auto cursorId = parsed(Domain::MemoryRecordId::parse(
            "51515151-5151-4151-8151-515151515153"));
        foreignRequest.instructionCursorAdvances = {
            {cursorId, 1U, "queue-foreign", 1U, true}};
        Domain::ManagedRunRecord foreignRecord{
            foreignRequest.runId,
            foreignRequest.projectId,
            foreignRequest.clientId,
            foreignRequest.task,
            foreignRequest.authorityGeneration,
            Domain::ManagedRunState::Running,
            std::nullopt,
            0U,
            0U,
            std::nullopt,
            std::nullopt,
            std::nullopt,
            {},
            clock.utcNow(),
            clock.utcNow(),
            foreignRequest.allowTools};
        foreignRecord.automaticContinuity =
            foreignRequest.automaticContinuity;
        foreignRecord.admissionIdentity = foreignRequest.admissionIdentity;
        foreignRecord.dispatchPending = true;
        foreignRecord.dispatchPhase =
            Domain::ManagedRunDispatchPhase::CursorPending;
        foreignRecord.dispatchOperationId = foreignRequest.operationId;
        foreignRecord.dispatchCorrelationId = foreignRequest.correlationId;
        foreignRecord.instructionCursorAdvances =
            foreignRequest.instructionCursorAdvances;
        Store foreignStore;
        foreignStore.seed(foreignRecord);
        Transport foreignTransport;
        TestFakes::RecordingProjectMemoryService foreignMemory;
        const auto advancedByAnotherRun = cursorRecord(
            foreignRequest.projectId, cursorId, 2U,
            "queue-foreign", 1U, 1U, "cursor-owner-a",
            "51515151-5151-4151-8151-515151515154");
        foreignMemory.getResult.set(
            Domain::Result<Domain::MemoryRecords>::success(
                Domain::MemoryRecords{
                    foreignRequest.projectId, {advancedByAnotherRun}, 1'024U,
                    256U * 1024U,
                    Domain::ProjectMemorySchemaVersion,
                    Domain::ProjectMemoryCapabilityVersion}));
        Application::ManagedRunService foreignService{
            foreignTransport,
            foreignStore,
            clock,
            Application::ManagedRunToolDependencies{
                nullptr, nullptr, nullptr, &foreignMemory}};
        const auto rejected = foreignService.resolveReplay(
            foreignRequest,
            context(
                "50505050-5050-4050-8050-505050505051",
                "cursor-owner-b"));
        assert(!rejected &&
            rejected.error().code == Domain::ErrorCodes::Conflict);
        assert(foreignTransport.calls == 0U);
        assert(foreignMemory.callCount(
            TestFakes::ProjectMemoryCall::Update) == 0U);
        foreignService.shutdown();
    }

    {
        auto releasedRequest = request(
            "52525252-5252-4252-8252-525252525252",
            "53535353-5353-4353-8353-535353535353",
            "Resume a released durable run without an active worker.");
        releasedRequest.admissionIdentity = parsed(
            Domain::Sha256Digest::parse(std::string(64U, '6')));
        Domain::ManagedRunRecord releasedRecord{
            releasedRequest.runId,
            releasedRequest.projectId,
            releasedRequest.clientId,
            releasedRequest.task,
            releasedRequest.authorityGeneration,
            Domain::ManagedRunState::Running,
            std::nullopt,
            0U,
            0U,
            std::nullopt,
            std::nullopt,
            std::nullopt,
            {},
            clock.utcNow(),
            clock.utcNow(),
            releasedRequest.allowTools};
        releasedRecord.automaticContinuity =
            releasedRequest.automaticContinuity;
        releasedRecord.admissionIdentity =
            releasedRequest.admissionIdentity;
        releasedRecord.dispatchPhase =
            Domain::ManagedRunDispatchPhase::Ready;
        releasedRecord.dispatchOperationId = releasedRequest.operationId;
        releasedRecord.dispatchCorrelationId =
            releasedRequest.correlationId;
        Store releasedStore;
        releasedStore.seed(releasedRecord);
        Transport releasedTransport;
        Application::ManagedRunService releasedService{
            releasedTransport, releasedStore, clock};
        const auto releasedContext = context(
            "54545454-5454-4454-8454-545454545454",
            "released-run-replay");
        auto replayRequest = releasedRequest;
        replayRequest.operationId = releasedContext.operationId;
        replayRequest.correlationId = releasedContext.correlationId;
        const auto resumed =
            releasedService.resolveReplay(replayRequest, releasedContext);
        assert(resumed && resumed.value());
        const auto resumedCompleted =
            waitForTerminal(releasedService, releasedRequest.runId);
        assert(resumedCompleted.record.state ==
            Domain::ManagedRunState::Completed);
        assert(releasedTransport.calls == 1U);
        assert(releasedTransport.lastOperation ==
            releasedRequest.operationId.value());
        assert(releasedTransport.lastCorrelation ==
            releasedRequest.correlationId.value());
        releasedService.shutdown();
    }

    {
        auto claimedRequest = request(
            "55555555-5555-4555-8555-555555555555",
            "56565656-5656-4656-8656-565656565656",
            "Never replay an ambiguously claimed provider dispatch.");
        claimedRequest.admissionIdentity = parsed(
            Domain::Sha256Digest::parse(std::string(64U, '7')));
        Domain::ManagedRunRecord claimedRecord{
            claimedRequest.runId,
            claimedRequest.projectId,
            claimedRequest.clientId,
            claimedRequest.task,
            claimedRequest.authorityGeneration,
            Domain::ManagedRunState::Running,
            std::nullopt,
            0U,
            0U,
            std::nullopt,
            std::nullopt,
            std::nullopt,
            {},
            clock.utcNow(),
            clock.utcNow(),
            claimedRequest.allowTools};
        claimedRecord.automaticContinuity =
            claimedRequest.automaticContinuity;
        claimedRecord.admissionIdentity = claimedRequest.admissionIdentity;
        claimedRecord.dispatchPhase =
            Domain::ManagedRunDispatchPhase::ProviderClaimed;
        claimedRecord.providerResponseId = parsed(
            Domain::ProviderSessionId::parse("resp_claimed_before_crash"));
        claimedRecord.dispatchOperationId = claimedRequest.operationId;
        claimedRecord.dispatchCorrelationId = claimedRequest.correlationId;
        Store claimedStore;
        claimedStore.seed(claimedRecord);
        Transport claimedTransport;
        Application::ManagedRunService claimedService{
            claimedTransport, claimedStore, clock};
        const auto claimedContext = context(
            "57575757-5757-4757-8757-575757575757",
            "claimed-run-replay");
        auto claimedReplay = claimedRequest;
        claimedReplay.operationId = claimedContext.operationId;
        claimedReplay.correlationId = claimedContext.correlationId;
        const auto refused = claimedService.resolveReplay(
            claimedReplay, claimedContext);
        assert(refused && refused.value());
        assert(refused.value()->record.state ==
            Domain::ManagedRunState::Failed);
        assert(refused.value()->record.lastError &&
            refused.value()->record.lastError->code ==
                Domain::ErrorCodes::Conflict);
        assert(claimedTransport.calls == 0U);
        const auto durableRefusal = claimedStore.record(
            claimedRequest.runId);
        assert(durableRefusal && durableRefusal->state ==
            Domain::ManagedRunState::Failed);
        claimedService.shutdown();
    }

    {
        auto cancelRequest = request(
            "58585858-5858-4858-8858-585858585858",
            "59595959-5959-4959-8959-595959595959",
            "Cancel a durable pending admission before dispatch.");
        cancelRequest.admissionIdentity = parsed(
            Domain::Sha256Digest::parse(std::string(64U, '8')));
        const auto cancelCursor = parsed(Domain::MemoryRecordId::parse(
            "60606060-6060-4060-8060-606060606060"));
        cancelRequest.instructionCursorAdvances = {
            {cancelCursor, 1U, "queue-cancel", 1U, true}};
        Domain::ManagedRunRecord cancelRecord{
            cancelRequest.runId,
            cancelRequest.projectId,
            cancelRequest.clientId,
            cancelRequest.task,
            cancelRequest.authorityGeneration,
            Domain::ManagedRunState::Running,
            std::nullopt,
            0U,
            0U,
            std::nullopt,
            std::nullopt,
            std::nullopt,
            {},
            clock.utcNow(),
            clock.utcNow(),
            cancelRequest.allowTools};
        cancelRecord.automaticContinuity =
            cancelRequest.automaticContinuity;
        cancelRecord.admissionIdentity = cancelRequest.admissionIdentity;
        cancelRecord.dispatchPending = true;
        cancelRecord.instructionCursorAdvances =
            cancelRequest.instructionCursorAdvances;
        cancelRecord.dispatchPhase =
            Domain::ManagedRunDispatchPhase::CursorPending;
        cancelRecord.dispatchOperationId = cancelRequest.operationId;
        cancelRecord.dispatchCorrelationId = cancelRequest.correlationId;
        Store cancelStore;
        cancelStore.seed(cancelRecord);
        Transport cancelTransport;
        TestFakes::RecordingProjectMemoryService cancelMemory;
        Application::ManagedRunService cancelService{
            cancelTransport,
            cancelStore,
            clock,
            Application::ManagedRunToolDependencies{
                nullptr, nullptr, nullptr, &cancelMemory}};
        const auto cancelContext = context(
            "61616161-6161-4161-8161-616161616161",
            "cancel-pending-restart");
        const auto durableCancelled = cancelService.cancel(
            cancelRequest.runId, cancelContext);
        assert(durableCancelled && durableCancelled.value().record.state ==
            Domain::ManagedRunState::Cancelled);
        assert(!durableCancelled.value().record.dispatchPending);
        assert(durableCancelled.value().record.instructionCursorAdvances.empty());
        const auto replayedCancellation = cancelService.resolveReplay(
            cancelRequest, cancelContext);
        assert(replayedCancellation && replayedCancellation.value());
        assert(replayedCancellation.value()->record.state ==
            Domain::ManagedRunState::Cancelled);
        assert(cancelMemory.callCount(
            TestFakes::ProjectMemoryCall::Update) == 0U);
        assert(cancelMemory.callCount(
            TestFakes::ProjectMemoryCall::UpdateBatch) == 0U);
        assert(cancelTransport.calls == 0U);
        cancelService.shutdown();
    }

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
    assert(continuityObserver.predecessorSessionIds ==
           std::vector<std::string>{continuityRequest.runId.value()});
    assert(continuityObserver.completedSummaries.size() == 1U);
    assert(continuityObserver.completedSummaries.front() ==
           "Native tool fixture_read result: {\"ok\":true,\"text\":\"fixture\"}");
    assert(continuityTransport.sawSuccessorPrompt);
    assert(continuityObserver.abandonCalls == 0U);
    continuityService.shutdown();

    Store checkpointOnlyStore;
    Transport checkpointOnlyTransport;
    checkpointOnlyTransport.mode = Transport::Mode::ToolLoop;
    ToolRouter checkpointOnlyRouter;
    ContinuityObserver checkpointOnlyObserver;
    checkpointOnlyObserver.scriptedActions = {
        Domain::ContextBudgetAction::Checkpoint};
    ProjectRegistry checkpointOnlyRegistry{toolRequest.projectId};
    WorkspaceAuthority checkpointOnlyAuthority{
        toolRequest.projectId, toolRequest.clientId};
    Application::ManagedRunService checkpointOnlyService{
        checkpointOnlyTransport,
        checkpointOnlyStore,
        clock,
        Application::ManagedRunToolDependencies{
            &toolCatalog, &checkpointOnlyRouter, &checkpointOnlyAuthority},
        Application::ManagedRunContinuityDependencies{
            &checkpointOnlyObserver,
            &continuityCodec,
            &checkpointOnlyRegistry,
            parsed(Domain::AdapterId::parse("managed-test-adapter")),
            1'000U,
            100U,
            std::optional<std::string>{"fixture-model"},
            std::optional<std::string>{"fixture-provider"}}};
    const auto checkpointOnlyRequest = request(
        "26262626-2626-4626-8626-262626262626",
        "27272727-2727-4727-8727-272727272727",
        "Complete after a durable checkpoint without creating a successor.");
    assert(checkpointOnlyService.start(
        checkpointOnlyRequest,
        context(
            "27272727-2727-4727-8727-272727272727",
            "managed-run-checkpoint-only")));
    const auto checkpointOnlyCompleted = waitForTerminal(
        checkpointOnlyService, checkpointOnlyRequest.runId);
    assert(checkpointOnlyCompleted.record.state ==
           Domain::ManagedRunState::Completed);
    assert(checkpointOnlyObserver.calls == 1U);
    assert(checkpointOnlyObserver.abandonCalls == 1U);
    assert(checkpointOnlyObserver.abandonedProjectId ==
           checkpointOnlyRequest.projectId.value());
    assert(checkpointOnlyObserver.abandonedOperationId ==
           checkpointOnlyRequest.runId.value());
    assert(!checkpointOnlyTransport.sawSuccessorPrompt);
    checkpointOnlyService.shutdown();

    Store failedRolloverStore;
    Transport failedRolloverTransport;
    failedRolloverTransport.mode = Transport::Mode::ToolLoop;
    ToolRouter failedRolloverRouter;
    ContinuityObserver failedRolloverObserver;
    failedRolloverObserver.observeError = Domain::makeError(
        Domain::ErrorCodes::TransportClosed,
        "The successor could not be created after checkpoint persistence.");
    failedRolloverObserver.abandonError = Domain::makeError(
        Domain::ErrorCodes::DatabaseBusy,
        "The durable checkpoint cleanup could not be committed.",
        true);
    ProjectRegistry failedRolloverRegistry{toolRequest.projectId};
    WorkspaceAuthority failedRolloverAuthority{
        toolRequest.projectId, toolRequest.clientId};
    Application::ManagedRunService failedRolloverService{
        failedRolloverTransport,
        failedRolloverStore,
        clock,
        Application::ManagedRunToolDependencies{
            &toolCatalog, &failedRolloverRouter, &failedRolloverAuthority},
        Application::ManagedRunContinuityDependencies{
            &failedRolloverObserver,
            &continuityCodec,
            &failedRolloverRegistry,
            parsed(Domain::AdapterId::parse("managed-test-adapter")),
            1'000U,
            100U,
            std::optional<std::string>{"fixture-model"},
            std::optional<std::string>{"fixture-provider"}}};
    const auto failedRolloverRequest = request(
        "28282828-2828-4828-8828-282828282828",
        "29292929-2929-4929-8929-292929292929",
        "Fail after persisting a rollover checkpoint.");
    assert(failedRolloverService.start(
        failedRolloverRequest,
        context(
            "29292929-2929-4929-8929-292929292929",
            "managed-run-failed-rollover")));
    const auto failedRollover = waitForTerminal(
        failedRolloverService, failedRolloverRequest.runId);
    assert(failedRollover.record.state ==
           Domain::ManagedRunState::Failed);
    assert(failedRollover.record.lastError);
    assert(failedRollover.record.lastError->code ==
           Domain::ErrorCodes::DatabaseBusy);
    assert(failedRolloverObserver.abandonCalls == 1U);
    assert(failedRolloverObserver.abandonedOperationId ==
           failedRolloverRequest.runId.value());
    failedRolloverService.shutdown();

    Store utf8BoundaryStore;
    Transport utf8BoundaryTransport;
    utf8BoundaryTransport.mode = Transport::Mode::ToolLoop;
    ToolRouter utf8BoundaryRouter;
    const std::string toolSummaryPrefix =
        "Native tool fixture_read result: ";
    const std::string payloadPrefix = "{\"text\":\"";
    const std::string splitCodePoint = "\xe2\x82\xac";
    utf8BoundaryRouter.canonicalOutput = payloadPrefix +
        std::string(
            2U * 1024U - 1U - toolSummaryPrefix.size() -
                payloadPrefix.size(),
            'x') +
        splitCodePoint + "\"}";
    ContinuityObserver utf8BoundaryObserver;
    utf8BoundaryObserver.activateSuccessor = true;
    ProjectRegistry utf8BoundaryRegistry{toolRequest.projectId};
    WorkspaceAuthority utf8BoundaryAuthority{
        toolRequest.projectId, toolRequest.clientId};
    Application::ManagedRunService utf8BoundaryService{
        utf8BoundaryTransport,
        utf8BoundaryStore,
        clock,
        Application::ManagedRunToolDependencies{
            &toolCatalog, &utf8BoundaryRouter, &utf8BoundaryAuthority},
        Application::ManagedRunContinuityDependencies{
            &utf8BoundaryObserver,
            &continuityCodec,
            &utf8BoundaryRegistry,
            parsed(Domain::AdapterId::parse("managed-test-adapter")),
            1'000U,
            100U,
            std::optional<std::string>{"fixture-model"},
            std::optional<std::string>{"fixture-provider"}}};
    auto utf8BoundaryRequest = request(
        "18181818-1818-4818-8818-181818181818",
        "19191919-1919-4919-8919-191919191919",
        "placeholder");
    utf8BoundaryRequest.task =
        std::string(8U * 1024U - 1U, 'm') + splitCodePoint + "tail";
    assert(utf8BoundaryService.start(
        utf8BoundaryRequest,
        context(
            "19191919-1919-4919-8919-191919191919",
            "managed-run-utf8-boundary")));
    const auto utf8BoundaryCompleted = waitForTerminal(
        utf8BoundaryService, utf8BoundaryRequest.runId);
    assert(utf8BoundaryCompleted.record.state ==
           Domain::ManagedRunState::Completed);
    assert(utf8BoundaryObserver.missions.size() == 1U);
    assert(utf8BoundaryObserver.missions.front() ==
           std::string(8U * 1024U - 1U, 'm'));
    assert(Domain::isValidUtf8(utf8BoundaryObserver.missions.front()));
    assert(utf8BoundaryObserver.completedSummaries.size() == 1U);
    assert(utf8BoundaryObserver.completedSummaries.front().size() ==
           2U * 1024U - 1U);
    assert(Domain::isValidUtf8(
        utf8BoundaryObserver.completedSummaries.front()));
    assert(utf8BoundaryObserver.completedSummaries.front().find(
               splitCodePoint) == std::string::npos);
    const std::string taskStart = "[ORIGINAL MANAGER TASK]\n";
    const std::string taskEnd = "\n[END ORIGINAL MANAGER TASK]";
    const auto repeatedTaskStart =
        utf8BoundaryTransport.successorInput.find(taskStart);
    const auto repeatedTaskEnd =
        utf8BoundaryTransport.successorInput.find(taskEnd);
    assert(repeatedTaskStart != std::string::npos);
    assert(repeatedTaskEnd != std::string::npos);
    const auto repeatedTask = utf8BoundaryTransport.successorInput.substr(
        repeatedTaskStart + taskStart.size(),
        repeatedTaskEnd - repeatedTaskStart - taskStart.size());
    assert(repeatedTask == std::string(8U * 1024U - 1U, 'm'));
    assert(Domain::isValidUtf8(repeatedTask));
    assert(utf8BoundaryTransport.successorInput.find(
               utf8BoundaryObserver.completedSummaries.front()) !=
           std::string::npos);
    utf8BoundaryService.shutdown();

    Store repeatedContinuityStore;
    Transport repeatedContinuityTransport;
    repeatedContinuityTransport.mode = Transport::Mode::ExtendedToolLoop;
    repeatedContinuityTransport.extendedToolCallLimit = 3U;
    ToolRouter repeatedContinuityRouter;
    ContinuityObserver repeatedContinuityObserver;
    repeatedContinuityObserver.scriptedActions = {
        Domain::ContextBudgetAction::Checkpoint,
        Domain::ContextBudgetAction::Rollover,
        Domain::ContextBudgetAction::Rollover};
    ProjectRegistry repeatedContinuityRegistry{toolRequest.projectId};
    WorkspaceAuthority repeatedContinuityAuthority{
        toolRequest.projectId, toolRequest.clientId};
    Application::ManagedRunService repeatedContinuityService{
        repeatedContinuityTransport,
        repeatedContinuityStore,
        clock,
        Application::ManagedRunToolDependencies{
            &toolCatalog,
            &repeatedContinuityRouter,
            &repeatedContinuityAuthority},
        Application::ManagedRunContinuityDependencies{
            &repeatedContinuityObserver,
            &continuityCodec,
            &repeatedContinuityRegistry,
            parsed(Domain::AdapterId::parse("managed-test-adapter")),
            1'000U,
            100U,
            std::optional<std::string>{"fixture-model"},
            std::optional<std::string>{"fixture-provider"}}};
    const auto repeatedContinuityRequest = request(
        "16161616-1616-4616-8616-161616161616",
        "17171717-1717-4717-8717-171717171717",
        "Exercise more than one continuity successor in one managed run.");
    assert(repeatedContinuityService.start(
        repeatedContinuityRequest,
        context(
            "17171717-1717-4717-8717-171717171717",
            "managed-run-repeated-continuity")));
    const auto repeatedContinuityCompleted = waitForTerminal(
        repeatedContinuityService, repeatedContinuityRequest.runId);
    assert(repeatedContinuityCompleted.record.state ==
           Domain::ManagedRunState::Completed);
    assert(repeatedContinuityObserver.calls == 3U);
    assert(repeatedContinuityObserver.handoffIds.size() == 3U);
    assert(repeatedContinuityObserver.handoffIds[0] ==
           repeatedContinuityRequest.runId.value());
    assert(repeatedContinuityObserver.handoffIds[1] ==
           repeatedContinuityObserver.handoffIds[0]);
    assert(repeatedContinuityObserver.handoffIds[2] !=
           repeatedContinuityObserver.handoffIds[1]);
    assert(repeatedContinuityObserver.predecessorSessionIds.size() == 3U);
    assert(repeatedContinuityObserver.predecessorSessionIds[0] ==
           repeatedContinuityRequest.runId.value());
    assert(repeatedContinuityObserver.predecessorSessionIds[1] ==
           repeatedContinuityRequest.runId.value());
    assert(repeatedContinuityObserver.successorSessionIds.size() == 2U);
    assert(repeatedContinuityObserver.successorSessionIds[0] !=
           repeatedContinuityObserver.successorSessionIds[1]);
    assert(repeatedContinuityObserver.predecessorSessionIds[2] ==
           repeatedContinuityObserver.successorSessionIds[0]);
    repeatedContinuityService.shutdown();

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
    return 0;
}
