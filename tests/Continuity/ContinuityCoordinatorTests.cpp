#include "ForgeConductor/Application/ContinuityCoordinator.h"
#include "Fakes/ContinuityRepositoryFake.h"
#include "Fakes/FoundationFakes.h"
#include "Fakes/ProjectRepositoryFakes.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace {

using namespace std::chrono_literals;
namespace Application = ForgeConductor::Application;
namespace Contracts = ForgeConductor::Contracts;
namespace Domain = ForgeConductor::Domain;
namespace Fakes = ForgeConductor::Tests::Fakes;

std::atomic_size_t assertionCount{};

void require(const bool condition, const std::string_view expression)
{
    assertionCount.fetch_add(1U, std::memory_order_relaxed);
    if (!condition) {
        throw std::runtime_error{
            "Requirement failed: " + std::string{expression}};
    }
}

#define REQUIRE(condition) require(static_cast<bool>(condition), #condition)

template <typename T>
[[nodiscard]] T take(Domain::Result<T> result)
{
    if (!result) {
        throw std::runtime_error{
            result.error().code + ": " + result.error().message};
    }
    return std::move(result).value();
}

void take(Domain::Result<void> result)
{
    if (!result) {
        throw std::runtime_error{
            result.error().code + ": " + result.error().message};
    }
}

template <typename T>
[[nodiscard]] T parse(const std::string_view value)
{
    return take(T::parse(value));
}

template <typename T>
void requireError(
    const Domain::Result<T>& result,
    const std::string_view expectedCode)
{
    REQUIRE(!result);
    REQUIRE(result.error().code == expectedCode);
}

[[nodiscard]] std::string uuidText(const std::uint64_t value)
{
    std::ostringstream stream;
    stream << "10000000-0000-4000-8000-" << std::hex << std::nouppercase
           << std::setfill('0') << std::setw(12) << value;
    return stream.str();
}

[[nodiscard]] Domain::OperationContext operationContext(
    const Contracts::IClock& clock,
    const std::uint64_t identifier,
    const std::string_view correlation,
    const std::stop_token cancellation = {},
    const bool expired = false)
{
    return Domain::OperationContext{
        parse<Domain::OperationId>(uuidText(50'000U + identifier)),
        clock.monotonicNow() + (expired ? 0s : 5min),
        cancellation,
        parse<Domain::CorrelationId>(correlation)};
}

[[nodiscard]] Domain::ContinuityHandoff handoffFor(
    const std::uint64_t identifier,
    const Domain::UtcTimePoint createdAt,
    const Domain::AdapterId& adapterId)
{
    return Domain::ContinuityHandoff{
        parse<Domain::ContinuityHandoffId>(uuidText(20'000U + identifier)),
        parse<Domain::ContinuityOperationId>(uuidText(10'000U + identifier)),
        createdAt,
        Domain::ContinuityProject{
            parse<Domain::ProjectId>(uuidText(1'000U + identifier)),
            "Continuity project " + std::to_string(identifier),
            take(Domain::PathText::create(
                "D:/continuity/project-" + std::to_string(identifier))),
            "main",
            "0123456789abcdef",
            {}},
        Domain::ContinuitySession{
            parse<Domain::SessionId>(uuidText(30'000U + identifier)),
            std::nullopt,
            std::optional<std::string>{"test-model"},
            std::optional<std::string>{"test-provider"}},
        std::nullopt,
        "Recover every committed continuity boundary",
        {"Create at most one physical successor"},
        Domain::ContinuityCurrentWork{
            "P11",
            "continuity-coordinator",
            "Exercise restart recovery",
            {take(Domain::PathText::create(
                "tests/Continuity/ContinuityCoordinatorTests.cpp"))}},
        {{std::optional<std::string>{"checkpoint"},
          "Canonical handoff assembled",
          std::optional<std::string>{"complete"}}},
        {{std::optional<std::string>{"recovery"},
          "Finish rollover after restart",
          std::optional<std::string>{"open"}}},
        {{"Persist intent before every external host effect",
          std::optional<std::string>{"Crash recovery requires a durable boundary"}}},
        Domain::ContinuityValidation{{"G10"}, {"G11"}, {}},
        {},
        {},
        {{1U,
          "Recover the continuity operation",
          "",
          "The successor is active exactly once"}},
        Domain::ContinuityHostState{
            adapterId,
            Domain::ContinuityState::Idle,
            "coordinator-test",
            {},
            std::nullopt},
        parse<Domain::Sha256Digest>(
            "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"),
        true};
}

class DurableRepositoryFactory final
    : public Contracts::IContinuityRepositoryFactory {
public:
    void add(std::shared_ptr<Fakes::ContinuityRepositoryFake> repository)
    {
        std::lock_guard lock{mutex_};
        repositories_.push_back(std::move(repository));
    }

    [[nodiscard]] Domain::Result<std::shared_ptr<Contracts::IContinuityRepository>>
    openContinuity(
        const Domain::ProjectId& projectId,
        const Domain::OperationContext& context) noexcept override
    {
        try {
            std::lock_guard lock{mutex_};
            if (shutdown_ || context.isCancellationRequested()) {
                return failure<std::shared_ptr<Contracts::IContinuityRepository>>(
                    Domain::ErrorCodes::Cancelled,
                    "The durable repository factory is closed or cancelled.");
            }
            const auto found = std::find_if(
                repositories_.begin(), repositories_.end(),
                [&](const auto& repository) {
                    return repository->projectId() == projectId;
                });
            if (found == repositories_.end()) {
                return failure<std::shared_ptr<Contracts::IContinuityRepository>>(
                    Domain::ErrorCodes::ProjectNotFound,
                    "The durable continuity project is not registered.");
            }
            openProjects_.insert(projectId.value());
            std::shared_ptr<Contracts::IContinuityRepository> result = *found;
            return Domain::Result<
                std::shared_ptr<Contracts::IContinuityRepository>>::success(
                    std::move(result));
        } catch (...) {
            return failure<std::shared_ptr<Contracts::IContinuityRepository>>(
                Domain::ErrorCodes::InternalFailure,
                "The durable repository could not be opened.");
        }
    }

    [[nodiscard]] Domain::Result<void> close(
        const Domain::ProjectId& projectId,
        const Domain::OperationContext&) noexcept override
    {
        try {
            std::lock_guard lock{mutex_};
            openProjects_.erase(projectId.value());
            return Domain::Result<void>::success();
        } catch (...) {
            return failure<void>(
                Domain::ErrorCodes::InternalFailure,
                "The durable repository could not be closed.");
        }
    }

    [[nodiscard]] std::size_t openCount() const noexcept override
    {
        try {
            std::lock_guard lock{mutex_};
            return openProjects_.size();
        } catch (...) {
            return 0U;
        }
    }

    void shutdown() noexcept override
    {
        try {
            std::lock_guard lock{mutex_};
            shutdown_ = true;
            openProjects_.clear();
        } catch (...) {
        }
    }

private:
    template <typename T>
    [[nodiscard]] static Domain::Result<T> failure(
        const std::string_view code,
        std::string message)
    {
        return Domain::Result<T>::failure(
            Domain::makeError(code, std::move(message)));
    }

    mutable std::mutex mutex_;
    std::vector<std::shared_ptr<Fakes::ContinuityRepositoryFake>> repositories_;
    std::set<std::string> openProjects_;
    bool shutdown_{};
};

class DurableSessionHost final : public Contracts::ISessionHostAdapter {
public:
    explicit DurableSessionHost(Domain::AdapterId adapterId)
        : adapterId_{std::move(adapterId)}
    {
    }

    [[nodiscard]] const Domain::AdapterId& identifier() const noexcept override
    {
        return adapterId_;
    }

    [[nodiscard]] std::string_view version() const noexcept override
    {
        return "p11-test-host-1";
    }

    [[nodiscard]] Domain::Result<Domain::HostCapabilities> capabilities(
        const Domain::OperationContext& context) noexcept override
    {
        try {
            std::lock_guard lock{mutex_};
            auto accepted = accept(context);
            if (!accepted) {
                return propagate<Domain::HostCapabilities>(std::move(accepted));
            }
            ++capabilityCalls_;
            return Domain::Result<Domain::HostCapabilities>::success(capabilities_);
        } catch (...) {
            return internalFailure<Domain::HostCapabilities>();
        }
    }

    [[nodiscard]] Domain::Result<Domain::HostSession> createSession(
        const Domain::SessionCreationRequest& request,
        const Domain::OperationContext& context) noexcept override
    {
        try {
            std::lock_guard lock{mutex_};
            auto accepted = accept(context);
            if (!accepted) {
                return propagate<Domain::HostSession>(std::move(accepted));
            }
            ++createCalls_;
            if (failNextCreate_) {
                failNextCreate_ = false;
                return Domain::Result<Domain::HostSession>::failure(
                    Domain::makeError(
                        Domain::ErrorCodes::ProcessLaunchFailed,
                        "The deterministic host launch failed once.",
                        true));
            }
            const auto key = sessionKey(request.projectId, request.idempotencyKey);
            const auto existing = sessions_.find(key);
            if (existing != sessions_.end()) {
                return Domain::Result<Domain::HostSession>::success(existing->second);
            }
            Domain::HostSession session{
                parse<Domain::SessionId>(request.operationId.value()),
                request.projectId,
                request.operationId,
                request.predecessorSessionId,
                request.idempotencyKey,
                std::nullopt,
                std::optional<std::string>{"test-model"},
                Domain::HostSessionStatus::Ready};
            sessions_.emplace(key, session);
            ++physicalCreates_;
            return Domain::Result<Domain::HostSession>::success(std::move(session));
        } catch (...) {
            return internalFailure<Domain::HostSession>();
        }
    }

    [[nodiscard]] Domain::Result<std::optional<Domain::HostSession>>
    queryByIdempotencyKey(
        const Domain::ProjectId& projectId,
        const Domain::IdempotencyKey& key,
        const Domain::OperationContext& context) noexcept override
    {
        try {
            std::lock_guard lock{mutex_};
            auto accepted = accept(context);
            if (!accepted) {
                return propagate<std::optional<Domain::HostSession>>(
                    std::move(accepted));
            }
            ++queryByKeyCalls_;
            if (failNextQuery_) {
                failNextQuery_ = false;
                return Domain::Result<
                    std::optional<Domain::HostSession>>::failure(
                    Domain::makeError(
                        Domain::ErrorCodes::InternalFailure,
                        "The deterministic host reconciliation failed once.",
                        true));
            }
            const auto found = sessions_.find(sessionKey(projectId, key));
            return Domain::Result<std::optional<Domain::HostSession>>::success(
                found == sessions_.end()
                    ? std::nullopt
                    : std::optional<Domain::HostSession>{found->second});
        } catch (...) {
            return internalFailure<std::optional<Domain::HostSession>>();
        }
    }

    [[nodiscard]] Domain::Result<void> bootstrap(
        const Domain::HostSession& session,
        const Domain::ContinuityHandoff& handoff,
        const Domain::OperationContext& context) noexcept override
    {
        try {
            auto compatible = Domain::validateBootstrapCompatibility(
                session, handoff);
            if (!compatible) {
                return compatible;
            }
            std::lock_guard lock{mutex_};
            auto accepted = accept(context);
            if (!accepted) {
                return accepted;
            }
            ++bootstrapCalls_;
            bootstraps_.insert(
                session.id.value() + ":" + handoff.handoffId.value() + ":" +
                handoff.contentSha256.value());
            return Domain::Result<void>::success();
        } catch (...) {
            return internalFailure<void>();
        }
    }

    [[nodiscard]] Domain::Result<Domain::HandoffAcknowledgement>
    awaitAcknowledgement(
        const Domain::HostSession& session,
        const Domain::ContinuityHandoffId& handoffId,
        const Domain::Sha256Digest& handoffSha256,
        const Domain::OperationContext& context) noexcept override
    {
        try {
            std::lock_guard lock{mutex_};
            auto accepted = accept(context);
            if (!accepted) {
                return propagate<Domain::HandoffAcknowledgement>(
                    std::move(accepted));
            }
            ++acknowledgementCalls_;
            const auto digest = badAcknowledgement_
                ? parse<Domain::Sha256Digest>(
                      "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb")
                : handoffSha256;
            return Domain::Result<Domain::HandoffAcknowledgement>::success(
                Domain::HandoffAcknowledgement{
                    handoffId, session.id, adapterId_, digest});
        } catch (...) {
            return internalFailure<Domain::HandoffAcknowledgement>();
        }
    }

    [[nodiscard]] Domain::Result<Domain::HostSessionStatus> query(
        const Domain::SessionId& sessionId,
        const Domain::OperationContext& context) noexcept override
    {
        try {
            std::lock_guard lock{mutex_};
            auto accepted = accept(context);
            if (!accepted) {
                return propagate<Domain::HostSessionStatus>(std::move(accepted));
            }
            const auto found = std::find_if(
                sessions_.begin(), sessions_.end(), [&](const auto& entry) {
                    return entry.second.id == sessionId;
                });
            if (found == sessions_.end()) {
                return Domain::Result<Domain::HostSessionStatus>::failure(
                    Domain::makeError(
                        Domain::ErrorCodes::SessionNotFound,
                        "The deterministic host session was not found."));
            }
            return Domain::Result<Domain::HostSessionStatus>::success(
                found->second.status);
        } catch (...) {
            return internalFailure<Domain::HostSessionStatus>();
        }
    }

    [[nodiscard]] Domain::Result<Domain::HostRecoveryReport> recover(
        const Domain::HostRecoveryRequest&,
        const Domain::OperationContext& context) noexcept override
    {
        try {
            std::lock_guard lock{mutex_};
            auto accepted = accept(context);
            if (!accepted) {
                return propagate<Domain::HostRecoveryReport>(std::move(accepted));
            }
            Domain::HostRecoveryReport report{};
            report.inspected = sessions_.size();
            report.recovered = sessions_.size();
            for (const auto& [key, session] : sessions_) {
                static_cast<void>(key);
                report.sessions.push_back(session);
            }
            return Domain::Result<Domain::HostRecoveryReport>::success(
                std::move(report));
        } catch (...) {
            return internalFailure<Domain::HostRecoveryReport>();
        }
    }

    void cancel(const Domain::OperationId& operationId) noexcept override
    {
        try {
            std::lock_guard lock{mutex_};
            cancelledOperation_ = operationId;
            ++cancelCalls_;
        } catch (...) {
        }
    }

    void shutdown() noexcept override
    {
        try {
            std::lock_guard lock{mutex_};
            shutdown_ = true;
        } catch (...) {
        }
    }

    void setCapabilities(const Domain::HostCapabilities capabilities) noexcept
    {
        std::lock_guard lock{mutex_};
        capabilities_ = capabilities;
    }

    void failNextCreate() noexcept
    {
        std::lock_guard lock{mutex_};
        failNextCreate_ = true;
    }

    void failNextQuery() noexcept
    {
        std::lock_guard lock{mutex_};
        failNextQuery_ = true;
    }

    void setBadAcknowledgement(const bool value) noexcept
    {
        std::lock_guard lock{mutex_};
        badAcknowledgement_ = value;
    }

    [[nodiscard]] std::size_t physicalCreateCount() const noexcept
    {
        std::lock_guard lock{mutex_};
        return physicalCreates_;
    }

    [[nodiscard]] std::size_t createCallCount() const noexcept
    {
        std::lock_guard lock{mutex_};
        return createCalls_;
    }

    [[nodiscard]] std::size_t distinctBootstrapCount() const noexcept
    {
        std::lock_guard lock{mutex_};
        return bootstraps_.size();
    }

    [[nodiscard]] std::size_t bootstrapCallCount() const noexcept
    {
        std::lock_guard lock{mutex_};
        return bootstrapCalls_;
    }

    [[nodiscard]] std::size_t queryByKeyCallCount() const noexcept
    {
        std::lock_guard lock{mutex_};
        return queryByKeyCalls_;
    }

    [[nodiscard]] std::size_t capabilityCallCount() const noexcept
    {
        std::lock_guard lock{mutex_};
        return capabilityCalls_;
    }

    [[nodiscard]] std::size_t cancelCallCount() const noexcept
    {
        std::lock_guard lock{mutex_};
        return cancelCalls_;
    }

    [[nodiscard]] const std::optional<Domain::OperationId>
    cancelledOperation() const noexcept
    {
        std::lock_guard lock{mutex_};
        return cancelledOperation_;
    }

private:
    template <typename T, typename U>
    [[nodiscard]] static Domain::Result<T> propagate(Domain::Result<U>&& result)
    {
        return Domain::Result<T>::failure(std::move(result).error());
    }

    template <typename T>
    [[nodiscard]] static Domain::Result<T> internalFailure()
    {
        return Domain::Result<T>::failure(Domain::makeError(
            Domain::ErrorCodes::InternalFailure,
            "The deterministic durable host failed internally."));
    }

    [[nodiscard]] Domain::Result<void> accept(
        const Domain::OperationContext& context) const
    {
        if (shutdown_) {
            return Domain::Result<void>::failure(Domain::makeError(
                Domain::ErrorCodes::TransportClosed,
                "The deterministic durable host is shut down."));
        }
        if (context.isCancellationRequested()) {
            return Domain::Result<void>::failure(Domain::makeError(
                Domain::ErrorCodes::Cancelled,
                "The deterministic durable host call was cancelled."));
        }
        return Domain::Result<void>::success();
    }

    [[nodiscard]] static std::string sessionKey(
        const Domain::ProjectId& projectId,
        const Domain::IdempotencyKey& key)
    {
        return projectId.value() + ":" + key.value();
    }

    Domain::AdapterId adapterId_;
    mutable std::mutex mutex_;
    Domain::HostCapabilities capabilities_{
        true, true, true, true, true, true, true, true};
    std::map<std::string, Domain::HostSession> sessions_;
    std::set<std::string> bootstraps_;
    std::optional<Domain::OperationId> cancelledOperation_;
    std::size_t capabilityCalls_{};
    std::size_t createCalls_{};
    std::size_t queryByKeyCalls_{};
    std::size_t physicalCreates_{};
    std::size_t bootstrapCalls_{};
    std::size_t acknowledgementCalls_{};
    std::size_t cancelCalls_{};
    bool failNextCreate_{};
    bool failNextQuery_{};
    bool badAcknowledgement_{};
    bool shutdown_{};
};

struct Scenario final {
    explicit Scenario(const std::uint64_t identifier)
        : clock{
              Domain::UtcTimePoint{std::chrono::seconds{1'700'000'000}},
              Domain::MonotonicTimePoint{1h}},
          registry{16U, clock.monotonicNow()},
          repository{std::make_shared<Fakes::ContinuityRepositoryFake>(
              parse<Domain::ProjectId>(uuidText(1'000U + identifier)),
              clock.monotonicNow())},
          host{parse<Domain::AdapterId>("p11-durable-host")},
          handoff{handoffFor(identifier, clock.utcNow(), host.identifier())},
          context{operationContext(clock, identifier, "p11-scenario")}
    {
        factory.add(repository);
        take(registry.seedDescriptor(Domain::ProjectMemoryDescriptor{
            handoff.project.projectId,
            handoff.project.displayName,
            std::optional<std::string>{"repository-" + std::to_string(identifier)},
            {handoff.project.repositoryRoot}}));
    }

    Fakes::FakeClock clock;
    Fakes::ProjectRegistryRepositoryFake registry;
    DurableRepositoryFactory factory;
    std::shared_ptr<Fakes::ContinuityRepositoryFake> repository;
    DurableSessionHost host;
    Domain::ContinuityHandoff handoff;
    Domain::OperationContext context;
};

enum class CrashBoundary {
    CheckpointIntent,
    CheckpointPersisted,
    SuccessorCreateIntent,
    CreateEffectBeforeCommit,
    SuccessorCommit,
    BootstrapIntent,
    BootstrapEffectBeforeAcknowledgement,
    AcknowledgementCommit,
    PredecessorSealIntent,
    CompletedPointerCommit
};

[[nodiscard]] std::string_view boundaryName(const CrashBoundary boundary) noexcept
{
    switch (boundary) {
    case CrashBoundary::CheckpointIntent:
        return "checkpoint_intent";
    case CrashBoundary::CheckpointPersisted:
        return "checkpoint_persisted";
    case CrashBoundary::SuccessorCreateIntent:
        return "successor_create_intent";
    case CrashBoundary::CreateEffectBeforeCommit:
        return "create_effect_before_commit";
    case CrashBoundary::SuccessorCommit:
        return "successor_commit";
    case CrashBoundary::BootstrapIntent:
        return "bootstrap_intent";
    case CrashBoundary::BootstrapEffectBeforeAcknowledgement:
        return "bootstrap_effect_before_acknowledgement";
    case CrashBoundary::AcknowledgementCommit:
        return "acknowledgement_commit";
    case CrashBoundary::PredecessorSealIntent:
        return "predecessor_seal_intent";
    case CrashBoundary::CompletedPointerCommit:
        return "completed_pointer_commit";
    }
    return "unknown";
}

[[nodiscard]] Domain::ContinuityOperation transition(
    Scenario& scenario,
    const Domain::ContinuityOperation& operation,
    const Domain::ContinuityState next,
    std::optional<Domain::SessionId> successor = std::nullopt)
{
    return take(scenario.repository->compareAndSet(
        operation.operationId,
        operation.state,
        next,
        std::move(successor),
        std::optional<std::string>{std::string{Domain::wireName(next)}},
        scenario.context));
}

[[nodiscard]] Domain::HostSession createHostEffect(
    Scenario& scenario,
    const Domain::ContinuityOperation& operation)
{
    return take(scenario.host.createSession(
        Domain::SessionCreationRequest{
            operation.operationId,
            operation.projectId,
            operation.predecessorSessionId,
            operation.idempotencyKey},
        scenario.context));
}

void stage(Scenario& scenario, const CrashBoundary boundary)
{
    const auto idempotency = take(Domain::IdempotencyKey::create(
        scenario.handoff.operationId.value()));
    auto operation = take(scenario.repository->createOperation(
        scenario.handoff, idempotency, scenario.context));
    take(scenario.repository->storeHandoff(
        scenario.handoff, scenario.context));
    operation = transition(
        scenario, operation, Domain::ContinuityState::CheckpointPreparing);
    if (boundary == CrashBoundary::CheckpointIntent) {
        return;
    }

    take(scenario.repository->storeHandoff(
        scenario.handoff, scenario.context));
    operation = transition(
        scenario, operation, Domain::ContinuityState::CheckpointPersisted);
    if (boundary == CrashBoundary::CheckpointPersisted) {
        return;
    }

    operation = transition(
        scenario, operation, Domain::ContinuityState::SuccessorCreating);
    if (boundary == CrashBoundary::SuccessorCreateIntent) {
        return;
    }

    const auto successor = createHostEffect(scenario, operation);
    if (boundary == CrashBoundary::CreateEffectBeforeCommit) {
        return;
    }

    operation = transition(
        scenario,
        operation,
        Domain::ContinuityState::SuccessorCreated,
        successor.id);
    if (boundary == CrashBoundary::SuccessorCommit) {
        return;
    }

    operation = transition(
        scenario,
        operation,
        Domain::ContinuityState::BootstrapSending,
        successor.id);
    if (boundary == CrashBoundary::BootstrapIntent) {
        return;
    }

    const auto durableHandoff = take(scenario.repository->handoff(
        operation.projectId, operation.handoffId, scenario.context));
    REQUIRE(durableHandoff.has_value());
    take(scenario.host.bootstrap(
        successor, *durableHandoff, scenario.context));
    if (boundary == CrashBoundary::BootstrapEffectBeforeAcknowledgement) {
        return;
    }

    const auto acknowledgement = take(scenario.host.awaitAcknowledgement(
        successor,
        durableHandoff->handoffId,
        durableHandoff->contentSha256,
        scenario.context));
    operation = take(scenario.repository->acknowledge(
        operation.operationId, acknowledgement, scenario.context));
    if (boundary == CrashBoundary::AcknowledgementCommit) {
        return;
    }

    operation = transition(
        scenario,
        operation,
        Domain::ContinuityState::PredecessorSealing,
        successor.id);
    if (boundary == CrashBoundary::PredecessorSealIntent) {
        return;
    }

    operation = transition(
        scenario,
        operation,
        Domain::ContinuityState::Completed,
        successor.id);
    REQUIRE(operation.state == Domain::ContinuityState::Completed);
}

void recoverEveryCommittedCrashBoundary()
{
    const std::vector<CrashBoundary> boundaries{
        CrashBoundary::CheckpointIntent,
        CrashBoundary::CheckpointPersisted,
        CrashBoundary::SuccessorCreateIntent,
        CrashBoundary::CreateEffectBeforeCommit,
        CrashBoundary::SuccessorCommit,
        CrashBoundary::BootstrapIntent,
        CrashBoundary::BootstrapEffectBeforeAcknowledgement,
        CrashBoundary::AcknowledgementCommit,
        CrashBoundary::PredecessorSealIntent,
        CrashBoundary::CompletedPointerCommit};

    std::uint64_t identifier{};
    for (const auto boundary : boundaries) {
        Scenario scenario{++identifier};
        stage(scenario, boundary);
        const auto before = scenario.repository->storedOperation();
        REQUIRE(before.has_value());

        // A new coordinator represents a process restart. The repository and
        // host are deliberately retained because their commits/effects survive.
        Application::ContinuityCoordinator restarted{
            scenario.registry, scenario.factory, scenario.host, scenario.clock};
        const auto report = take(restarted.recoverIncompleteOperations(
            Domain::ContinuityRecoveryRequest{
                scenario.handoff.project.projectId, true},
            operationContext(
                scenario.clock,
                100U + identifier,
                boundaryName(boundary))));

        const auto completed = scenario.repository->storedOperation();
        REQUIRE(completed.has_value());
        REQUIRE(completed->state == Domain::ContinuityState::Completed);
        REQUIRE(completed->successorSessionId.has_value());
        REQUIRE(completed->successorSessionId !=
                std::optional<Domain::SessionId>{
                    scenario.handoff.predecessorSession.sessionId});
        REQUIRE(scenario.host.physicalCreateCount() == 1U);
        REQUIRE(scenario.host.distinctBootstrapCount() == 1U);
        const auto active = take(scenario.repository->activeSession(
            scenario.handoff.project.projectId, scenario.context));
        REQUIRE(active == completed->successorSessionId);

        if (boundary == CrashBoundary::CompletedPointerCommit) {
            REQUIRE(report.inspected == 0U);
            REQUIRE(report.resumed == 0U);
        } else {
            REQUIRE(report.inspected == 1U);
            REQUIRE(report.resumed == 1U);
            REQUIRE(report.failed == 0U);
            REQUIRE(report.operations.size() == 1U);
            REQUIRE(report.operations.front().state ==
                    Domain::ContinuityState::Completed);
        }
        if (boundary == CrashBoundary::CreateEffectBeforeCommit) {
            REQUIRE(scenario.host.queryByKeyCallCount() >= 1U);
        }
        if (boundary ==
            CrashBoundary::BootstrapEffectBeforeAcknowledgement) {
            REQUIRE(scenario.host.bootstrapCallCount() == 2U);
            REQUIRE(scenario.host.distinctBootstrapCount() == 1U);
        }
    }
}

void exactAcknowledgementAndResumeAreBoundToTheSuccessor()
{
    Scenario scenario{20U};
    auto checkpoint = take(Application::ContinuityCoordinator{
        scenario.registry, scenario.factory, scenario.host, scenario.clock}
                                   .checkpoint(
                                       Domain::CheckpointRequest{scenario.handoff},
                                       scenario.context));
    REQUIRE(checkpoint.operation.state ==
            Domain::ContinuityState::CheckpointPersisted);

    scenario.host.setBadAcknowledgement(true);
    Application::ContinuityCoordinator firstProcess{
        scenario.registry, scenario.factory, scenario.host, scenario.clock};
    const auto rejected = firstProcess.requestRollover(
        Domain::RolloverRequest{
            scenario.handoff.project.projectId,
            scenario.handoff.operationId},
        scenario.context);
    requireError(rejected, Domain::ErrorCodes::IntegrityFailure);
    REQUIRE(scenario.repository->storedOperation()->state ==
            Domain::ContinuityState::BootstrapSending);
    REQUIRE(!scenario.repository->storedOperation()->acknowledgedSessionId);

    scenario.host.setBadAcknowledgement(false);
    Application::ContinuityCoordinator restarted{
        scenario.registry, scenario.factory, scenario.host, scenario.clock};
    const auto recovered = take(restarted.recoverIncompleteOperations(
        Domain::ContinuityRecoveryRequest{
            scenario.handoff.project.projectId, true},
        operationContext(scenario.clock, 120U, "ack-recovery")));
    REQUIRE(recovered.resumed == 1U);
    REQUIRE(scenario.host.physicalCreateCount() == 1U);
    REQUIRE(scenario.host.distinctBootstrapCount() == 1U);

    Scenario acknowledged{21U};
    stage(acknowledged, CrashBoundary::AcknowledgementCommit);
    Application::ContinuityCoordinator resumeProcess{
        acknowledged.registry,
        acknowledged.factory,
        acknowledged.host,
        acknowledged.clock};
    const auto operation = acknowledged.repository->storedOperation();
    REQUIRE(operation && operation->successorSessionId);
    const auto resumed = take(resumeProcess.resume(
        Domain::HandoffResumeRequest{
            acknowledged.handoff.project.projectId,
            acknowledged.handoff.handoffId,
            *operation->successorSessionId},
        acknowledged.context));
    REQUIRE(resumed.operation.state == Domain::ContinuityState::Completed);
    REQUIRE(resumed.session.id == *operation->successorSessionId);
    REQUIRE(resumed.handoff.handoffId == acknowledged.handoff.handoffId);
}

void checkpointHonorsCallerIdempotencyAndKeepsDeterministicDefault()
{
    Scenario explicitScenario{22U};
    const auto explicitKey = take(Domain::IdempotencyKey::create(
        "caller-supplied-checkpoint-key"));
    Application::ContinuityCoordinator explicitCoordinator{
        explicitScenario.registry,
        explicitScenario.factory,
        explicitScenario.host,
        explicitScenario.clock};
    const auto explicitCheckpoint = take(explicitCoordinator.checkpoint(
        Domain::CheckpointRequest{explicitScenario.handoff, explicitKey},
        explicitScenario.context));
    REQUIRE(explicitCheckpoint.operation.idempotencyKey == explicitKey);
    REQUIRE(explicitScenario.repository->storedOperation()->idempotencyKey ==
            explicitKey);

    Scenario defaultScenario{23U};
    const auto derivedKey = take(Domain::IdempotencyKey::create(
        defaultScenario.handoff.operationId.value()));
    Application::ContinuityCoordinator defaultCoordinator{
        defaultScenario.registry,
        defaultScenario.factory,
        defaultScenario.host,
        defaultScenario.clock};
    const auto defaultCheckpoint = take(defaultCoordinator.checkpoint(
        Domain::CheckpointRequest{defaultScenario.handoff},
        defaultScenario.context));
    REQUIRE(defaultCheckpoint.operation.idempotencyKey == derivedKey);
    REQUIRE(defaultScenario.repository->storedOperation()->idempotencyKey ==
            derivedKey);

    Scenario recoveryScenario{24U};
    const auto recoveryKey = take(Domain::IdempotencyKey::create(
        "caller-supplied-recovery-key"));
    auto recoveringOperation = take(recoveryScenario.repository->createOperation(
        recoveryScenario.handoff,
        recoveryKey,
        recoveryScenario.context));
    take(recoveryScenario.repository->storeHandoff(
        recoveryScenario.handoff, recoveryScenario.context));
    recoveringOperation = transition(
        recoveryScenario,
        recoveringOperation,
        Domain::ContinuityState::CheckpointPreparing);
    Application::ContinuityCoordinator recoveryCoordinator{
        recoveryScenario.registry,
        recoveryScenario.factory,
        recoveryScenario.host,
        recoveryScenario.clock};
    const auto recovered = take(recoveryCoordinator.requestRollover(
        Domain::RolloverRequest{
            recoveryScenario.handoff.project.projectId,
            recoveryScenario.handoff.operationId},
        recoveryScenario.context));
    REQUIRE(recovered.operation.state == Domain::ContinuityState::Completed);
    REQUIRE(recovered.operation.idempotencyKey == recoveryKey);
    REQUIRE(recoveryScenario.host.physicalCreateCount() == 1U);
}

void resumeReconciliationPrecedesMutationAndSupportsIdempotentCreate()
{
    Scenario failedQuery{22U};
    stage(failedQuery, CrashBoundary::AcknowledgementCommit);
    const auto acknowledged = failedQuery.repository->storedOperation();
    REQUIRE(acknowledged && acknowledged->successorSessionId);
    const auto transitionsBefore = take(failedQuery.repository->transitionCount(
        acknowledged->operationId, failedQuery.context));
    failedQuery.host.failNextQuery();

    Application::ContinuityCoordinator failedResume{
        failedQuery.registry,
        failedQuery.factory,
        failedQuery.host,
        failedQuery.clock};
    const auto failure = failedResume.resume(
        Domain::HandoffResumeRequest{
            failedQuery.handoff.project.projectId,
            failedQuery.handoff.handoffId,
            *acknowledged->successorSessionId},
        failedQuery.context);
    requireError(failure, Domain::ErrorCodes::InternalFailure);
    REQUIRE(failedQuery.repository->storedOperation()->state ==
            Domain::ContinuityState::Acknowledged);
    REQUIRE(take(failedQuery.repository->transitionCount(
                acknowledged->operationId, failedQuery.context)) ==
            transitionsBefore);
    REQUIRE(!take(failedQuery.repository->activeSession(
        failedQuery.handoff.project.projectId, failedQuery.context)));

    Scenario idempotentOnly{23U};
    stage(idempotentOnly, CrashBoundary::AcknowledgementCommit);
    const auto idempotentOperation =
        idempotentOnly.repository->storedOperation();
    REQUIRE(idempotentOperation && idempotentOperation->successorSessionId);
    REQUIRE(idempotentOnly.host.physicalCreateCount() == 1U);
    REQUIRE(idempotentOnly.host.createCallCount() == 1U);
    idempotentOnly.host.setCapabilities(Domain::HostCapabilities{
        true, true, true, true, true, false, true, true});

    Application::ContinuityCoordinator idempotentResume{
        idempotentOnly.registry,
        idempotentOnly.factory,
        idempotentOnly.host,
        idempotentOnly.clock};
    const auto resumed = take(idempotentResume.resume(
        Domain::HandoffResumeRequest{
            idempotentOnly.handoff.project.projectId,
            idempotentOnly.handoff.handoffId,
            *idempotentOperation->successorSessionId},
        idempotentOnly.context));
    REQUIRE(resumed.operation.state == Domain::ContinuityState::Completed);
    REQUIRE(resumed.session.id == *idempotentOperation->successorSessionId);
    REQUIRE(idempotentOnly.host.queryByKeyCallCount() == 0U);
    REQUIRE(idempotentOnly.host.createCallCount() == 2U);
    REQUIRE(idempotentOnly.host.physicalCreateCount() == 1U);
    REQUIRE(take(idempotentOnly.repository->activeSession(
                idempotentOnly.handoff.project.projectId,
                idempotentOnly.context)) ==
            idempotentOperation->successorSessionId);

    Scenario queryOnlyMissing{24U};
    stage(queryOnlyMissing, CrashBoundary::AcknowledgementCommit);
    const auto queryOnlyOperation =
        queryOnlyMissing.repository->storedOperation();
    REQUIRE(queryOnlyOperation && queryOnlyOperation->successorSessionId);
    DurableSessionHost emptyQueryOnlyHost{
        queryOnlyMissing.handoff.hostState.adapterId};
    emptyQueryOnlyHost.setCapabilities(Domain::HostCapabilities{
        false, true, true, true, false, true, true, true});
    Application::ContinuityCoordinator queryOnlyResume{
        queryOnlyMissing.registry,
        queryOnlyMissing.factory,
        emptyQueryOnlyHost,
        queryOnlyMissing.clock};
    const auto missing = queryOnlyResume.resume(
        Domain::HandoffResumeRequest{
            queryOnlyMissing.handoff.project.projectId,
            queryOnlyMissing.handoff.handoffId,
            *queryOnlyOperation->successorSessionId},
        queryOnlyMissing.context);
    requireError(missing, Domain::ErrorCodes::HostCapabilityUnavailable);
    REQUIRE(emptyQueryOnlyHost.capabilityCallCount() == 1U);
    REQUIRE(emptyQueryOnlyHost.queryByKeyCallCount() == 1U);
    REQUIRE(emptyQueryOnlyHost.createCallCount() == 0U);
    REQUIRE(emptyQueryOnlyHost.physicalCreateCount() == 0U);
    REQUIRE(queryOnlyMissing.repository->storedOperation()->state ==
            Domain::ContinuityState::Acknowledged);
    REQUIRE(!take(queryOnlyMissing.repository->activeSession(
        queryOnlyMissing.handoff.project.projectId,
        queryOnlyMissing.context)));

    Scenario wrongAdapter{25U};
    stage(wrongAdapter, CrashBoundary::AcknowledgementCommit);
    const auto wrongAdapterOperation =
        wrongAdapter.repository->storedOperation();
    REQUIRE(wrongAdapterOperation && wrongAdapterOperation->successorSessionId);
    DurableSessionHost mismatchedHost{
        parse<Domain::AdapterId>("another-p11-host")};
    Application::ContinuityCoordinator mismatchedResume{
        wrongAdapter.registry,
        wrongAdapter.factory,
        mismatchedHost,
        wrongAdapter.clock};
    const auto mismatched = mismatchedResume.resume(
        Domain::HandoffResumeRequest{
            wrongAdapter.handoff.project.projectId,
            wrongAdapter.handoff.handoffId,
            *wrongAdapterOperation->successorSessionId},
        wrongAdapter.context);
    requireError(mismatched, Domain::ErrorCodes::IntegrityFailure);
    REQUIRE(mismatchedHost.capabilityCallCount() == 0U);
    REQUIRE(mismatchedHost.queryByKeyCallCount() == 0U);
    REQUIRE(mismatchedHost.createCallCount() == 0U);
    REQUIRE(wrongAdapter.repository->storedOperation()->state ==
            Domain::ContinuityState::Acknowledged);
    REQUIRE(!take(wrongAdapter.repository->activeSession(
        wrongAdapter.handoff.project.projectId,
        wrongAdapter.context)));
}

void capabilityFailureDoesNotWeakenTheDurableCheckpoint()
{
    Scenario scenario{30U};
    Application::ContinuityCoordinator coordinator{
        scenario.registry, scenario.factory, scenario.host, scenario.clock};
    const auto checkpoint = take(coordinator.checkpoint(
        Domain::CheckpointRequest{scenario.handoff}, scenario.context));
    REQUIRE(checkpoint.operation.state ==
            Domain::ContinuityState::CheckpointPersisted);

    scenario.host.setCapabilities(Domain::HostCapabilities{
        true, false, true, true, true, true, true, true});
    const auto unavailable = coordinator.requestRollover(
        Domain::RolloverRequest{
            scenario.handoff.project.projectId,
            scenario.handoff.operationId},
        scenario.context);
    requireError(unavailable, Domain::ErrorCodes::HostCapabilityUnavailable);
    REQUIRE(scenario.repository->storedOperation()->state ==
            Domain::ContinuityState::CheckpointPersisted);
    REQUIRE(scenario.host.physicalCreateCount() == 0U);

    scenario.host.setCapabilities(Domain::HostCapabilities{
        true, true, true, true, true, true, true, true});
    const auto recovered = take(coordinator.recoverIncompleteOperations(
        Domain::ContinuityRecoveryRequest{
            scenario.handoff.project.projectId, true},
        operationContext(scenario.clock, 130U, "capability-recovery")));
    REQUIRE(recovered.resumed == 1U);
}

void recoverableHostFailureResumesOnlyAfterItsDurableRetryTime()
{
    Scenario scenario{40U};
    Application::ContinuityCoordinator firstProcess{
        scenario.registry, scenario.factory, scenario.host, scenario.clock};
    const auto checkpoint = take(firstProcess.checkpoint(
        Domain::CheckpointRequest{scenario.handoff}, scenario.context));
    REQUIRE(checkpoint.operation.state ==
            Domain::ContinuityState::CheckpointPersisted);
    scenario.host.failNextCreate();

    const auto failed = firstProcess.requestRollover(
        Domain::RolloverRequest{
            scenario.handoff.project.projectId,
            scenario.handoff.operationId},
        scenario.context);
    requireError(failed, Domain::ErrorCodes::ProcessLaunchFailed);
    const auto retry = scenario.repository->storedOperation();
    REQUIRE(retry.has_value());
    REQUIRE(retry->state == Domain::ContinuityState::RetryWait);
    REQUIRE(retry->retryResumeState ==
            std::optional<Domain::ContinuityState>{
                Domain::ContinuityState::SuccessorCreating});
    REQUIRE(retry->retryAt && *retry->retryAt > scenario.clock.utcNow());

    Application::ContinuityCoordinator restarted{
        scenario.registry, scenario.factory, scenario.host, scenario.clock};
    const auto waiting = take(restarted.recoverIncompleteOperations(
        Domain::ContinuityRecoveryRequest{
            scenario.handoff.project.projectId, true},
        operationContext(scenario.clock, 140U, "retry-wait")));
    REQUIRE(waiting.inspected == 1U);
    REQUIRE(waiting.failed == 1U);
    REQUIRE(waiting.resumed == 0U);
    REQUIRE(scenario.repository->storedOperation()->state ==
            Domain::ContinuityState::RetryWait);

    scenario.clock.advance(2s);
    const auto completed = take(restarted.recoverIncompleteOperations(
        Domain::ContinuityRecoveryRequest{
            scenario.handoff.project.projectId, true},
        operationContext(scenario.clock, 141U, "retry-resume")));
    REQUIRE(completed.resumed == 1U);
    REQUIRE(completed.failed == 0U);
    REQUIRE(scenario.repository->storedOperation()->state ==
            Domain::ContinuityState::Completed);
    REQUIRE(scenario.host.physicalCreateCount() == 1U);
}

void cancellationDeadlineRecoveryCancelAndShutdownOwnTheirBoundaries()
{
    Scenario scenario{50U};
    Application::ContinuityCoordinator coordinator{
        scenario.registry, scenario.factory, scenario.host, scenario.clock};

    std::stop_source source;
    REQUIRE(source.request_stop());
    const auto cancelledContext = operationContext(
        scenario.clock, 150U, "cancelled", source.get_token());
    requireError(
        coordinator.checkpoint(
            Domain::CheckpointRequest{scenario.handoff}, cancelledContext),
        Domain::ErrorCodes::Cancelled);
    REQUIRE(!scenario.repository->storedOperation());

    const auto expiredContext = operationContext(
        scenario.clock, 151U, "expired", {}, true);
    requireError(
        coordinator.checkpoint(
            Domain::CheckpointRequest{scenario.handoff}, expiredContext),
        Domain::ErrorCodes::DeadlineExceeded);
    REQUIRE(!scenario.repository->storedOperation());

    const auto checkpoint = take(coordinator.checkpoint(
        Domain::CheckpointRequest{scenario.handoff}, scenario.context));
    REQUIRE(checkpoint.operation.state ==
            Domain::ContinuityState::CheckpointPersisted);
    const auto cancelledRecovery = take(coordinator.recoverIncompleteOperations(
        Domain::ContinuityRecoveryRequest{
            scenario.handoff.project.projectId, false},
        operationContext(scenario.clock, 152U, "recovery-cancel")));
    REQUIRE(cancelledRecovery.inspected == 1U);
    REQUIRE(cancelledRecovery.cancelled == 1U);
    REQUIRE(scenario.repository->storedOperation()->state ==
            Domain::ContinuityState::Cancelled);
    REQUIRE(scenario.host.physicalCreateCount() == 0U);

    coordinator.cancel(scenario.context.operationId);
    REQUIRE(scenario.host.cancelCallCount() == 1U);
    REQUIRE(scenario.host.cancelledOperation() ==
            std::optional<Domain::OperationId>{scenario.context.operationId});
    coordinator.shutdown();
    requireError(
        coordinator.status(
            scenario.handoff.project.projectId, scenario.context),
        Domain::ErrorCodes::TransportClosed);
}

void concurrentProjectsRemainIsolatedAndBounded()
{
    constexpr std::size_t ProjectCount = 8U;
    Fakes::FakeClock clock{
        Domain::UtcTimePoint{std::chrono::seconds{1'700'100'000}},
        Domain::MonotonicTimePoint{2h}};
    Fakes::ProjectRegistryRepositoryFake registry{ProjectCount, clock.monotonicNow()};
    DurableRepositoryFactory factory;
    DurableSessionHost host{parse<Domain::AdapterId>("p11-shared-host")};
    std::vector<std::shared_ptr<Fakes::ContinuityRepositoryFake>> repositories;
    std::vector<Domain::ContinuityHandoff> handoffs;
    repositories.reserve(ProjectCount);
    handoffs.reserve(ProjectCount);

    Application::ContinuityCoordinator coordinator{
        registry, factory, host, clock};
    for (std::size_t index = 0U; index < ProjectCount; ++index) {
        const auto identifier = 100U + index;
        auto handoff = handoffFor(identifier, clock.utcNow(), host.identifier());
        auto repository = std::make_shared<Fakes::ContinuityRepositoryFake>(
            handoff.project.projectId, clock.monotonicNow());
        factory.add(repository);
        take(registry.seedDescriptor(Domain::ProjectMemoryDescriptor{
            handoff.project.projectId,
            handoff.project.displayName,
            std::optional<std::string>{"concurrent-repository"},
            {handoff.project.repositoryRoot}}));
        const auto checkpoint = take(coordinator.checkpoint(
            Domain::CheckpointRequest{handoff},
            operationContext(clock, 200U + index, "concurrent-checkpoint")));
        REQUIRE(checkpoint.operation.state ==
                Domain::ContinuityState::CheckpointPersisted);
        repositories.push_back(std::move(repository));
        handoffs.push_back(std::move(handoff));
    }

    std::vector<std::optional<Domain::Result<Domain::RolloverOutcome>>> outcomes(
        ProjectCount);
    std::vector<std::thread> workers;
    workers.reserve(ProjectCount);
    for (std::size_t index = 0U; index < ProjectCount; ++index) {
        workers.emplace_back([&, index]() {
            outcomes[index].emplace(coordinator.requestRollover(
                Domain::RolloverRequest{
                    handoffs[index].project.projectId,
                    handoffs[index].operationId},
                operationContext(clock, 300U + index, "concurrent-rollover")));
        });
    }
    for (auto& worker : workers) {
        worker.join();
    }

    std::set<std::string> successorIds;
    for (std::size_t index = 0U; index < ProjectCount; ++index) {
        REQUIRE(outcomes[index].has_value());
        const auto outcome = take(std::move(*outcomes[index]));
        REQUIRE(outcome.operation.projectId == handoffs[index].project.projectId);
        REQUIRE(outcome.operation.state == Domain::ContinuityState::Completed);
        REQUIRE(outcome.operation.successorSessionId.has_value());
        REQUIRE(successorIds.insert(
                    outcome.operation.successorSessionId->value()).second);
        REQUIRE(repositories[index]->storedOperation()->projectId ==
                handoffs[index].project.projectId);
        REQUIRE(repositories[index]->storedOperation()->handoffId ==
                handoffs[index].handoffId);
    }
    REQUIRE(successorIds.size() == ProjectCount);
    REQUIRE(host.physicalCreateCount() == ProjectCount);
    REQUIRE(host.distinctBootstrapCount() == ProjectCount);
    REQUIRE(factory.openCount() == ProjectCount);
}

} // namespace

int main()
{
    const std::vector<std::pair<std::string_view, void (*)()>> tests{
        {"recover_every_committed_crash_boundary",
         recoverEveryCommittedCrashBoundary},
        {"exact_acknowledgement_and_resume_are_bound_to_the_successor",
         exactAcknowledgementAndResumeAreBoundToTheSuccessor},
        {"checkpoint_honors_caller_idempotency_and_keeps_deterministic_default",
         checkpointHonorsCallerIdempotencyAndKeepsDeterministicDefault},
        {"resume_reconciliation_precedes_mutation_and_supports_idempotent_create",
         resumeReconciliationPrecedesMutationAndSupportsIdempotentCreate},
        {"capability_failure_does_not_weaken_the_durable_checkpoint",
         capabilityFailureDoesNotWeakenTheDurableCheckpoint},
        {"recoverable_host_failure_resumes_only_after_its_durable_retry_time",
         recoverableHostFailureResumesOnlyAfterItsDurableRetryTime},
        {"cancellation_deadline_recovery_cancel_and_shutdown_own_their_boundaries",
         cancellationDeadlineRecoveryCancelAndShutdownOwnTheirBoundaries},
        {"concurrent_projects_remain_isolated_and_bounded",
         concurrentProjectsRemainIsolatedAndBounded}};

    std::size_t failures{};
    for (const auto& [name, test] : tests) {
        try {
            test();
            std::cout << "PASS " << name << '\n';
        } catch (const std::exception& error) {
            ++failures;
            std::cerr << "FAIL " << name << ": " << error.what() << '\n';
        }
    }
    std::cout << "SUMMARY passed=" << (tests.size() - failures)
              << " failed=" << failures
              << " assertions="
              << assertionCount.load(std::memory_order_relaxed) << '\n';
    return failures == 0U ? EXIT_SUCCESS : EXIT_FAILURE;
}
