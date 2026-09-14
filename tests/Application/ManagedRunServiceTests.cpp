#include "ForgeConductor/Application/AgentRepositoryManagedRunStore.h"
#include "ForgeConductor/Application/ManagedRunService.h"

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
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
    }

    Mode mode{Mode::Success};
    std::size_t calls{};
    std::size_t cancels{};
    std::string lastProject;
    std::string lastRun;
    std::uint64_t lastGeneration{};
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
    WorkspaceAuthority(Domain::ProjectId projectId, Domain::ClientId clientId)
        : projectId_{std::move(projectId)}, clientId_{std::move(clientId)}
    {
    }

    [[nodiscard]] Domain::Result<Contracts::WorkspaceAuthority> authorityFor(
        const Domain::ProjectId& projectId,
        const Domain::OperationContext&) noexcept override
    {
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
                "{\"ok\":true,\"text\":\"fixture\"}",
                std::nullopt,
                std::nullopt});
    }

    void cancel(const Domain::OperationId&) noexcept override { ++cancels; }
    void shutdown() noexcept override {}

    std::size_t calls{};
    std::size_t cancels{};
    bool sawBinding{};
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
    Application::AgentRepositoryManagedRunStore durableStore{
        admissionRepository,
        parsed(Domain::AgentId::parse("forge-managed-run"))};
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
        admittedAt};
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
