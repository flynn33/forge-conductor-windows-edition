#include "ForgeConductor/Application/ContinuityAutomation.h"
#include "Fakes/FoundationFakes.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <map>
#include <mutex>
#include <optional>
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
        parse<Domain::OperationId>(uuidText(90'000U + identifier)),
        clock.monotonicNow() + (expired ? 0s : 5min),
        cancellation,
        parse<Domain::CorrelationId>(correlation)};
}

[[nodiscard]] Domain::ContinuityHandoff handoffFor(
    const std::uint64_t identifier,
    const Domain::UtcTimePoint createdAt)
{
    return Domain::ContinuityHandoff{
        parse<Domain::ContinuityHandoffId>(uuidText(20'000U + identifier)),
        parse<Domain::ContinuityOperationId>(uuidText(10'000U + identifier)),
        createdAt,
        Domain::ContinuityProject{
            parse<Domain::ProjectId>(uuidText(1'000U + identifier)),
            "Automation project " + std::to_string(identifier),
            take(Domain::PathText::create(
                "D:/continuity/automation-" + std::to_string(identifier))),
            "main",
            "0123456789abcdef",
            {}},
        Domain::ContinuitySession{
            parse<Domain::SessionId>(uuidText(30'000U + identifier)),
            std::nullopt,
            std::optional<std::string>{"test-model"},
            std::optional<std::string>{"test-provider"}},
        std::nullopt,
        "Autonomously preserve and resume the project context",
        {"No operator action"},
        Domain::ContinuityCurrentWork{
            "P12",
            "continuity-automation",
            "Observe provider context budget",
            {take(Domain::PathText::create(
                "tests/Continuity/ContinuityAutomationTests.cpp"))}},
        {},
        {{std::optional<std::string>{"rollover"},
          "Activate the acknowledged successor",
          std::optional<std::string>{"open"}}},
        {{"Resume is an idempotent activation proof", std::nullopt}},
        Domain::ContinuityValidation{{"G11"}, {"G12"}, {}},
        {},
        {},
        {{1U,
          "Activate the successor",
          "",
          "The exact acknowledged successor is active"}},
        Domain::ContinuityHostState{
            parse<Domain::AdapterId>("automation-test-adapter"),
            Domain::ContinuityState::Idle,
            "provider-observation",
            {},
            std::nullopt},
        parse<Domain::Sha256Digest>(
            "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"),
        true};
}

[[nodiscard]] Domain::ContextBudgetSignals signalsFor(
    const Domain::ContextBudgetAction action)
{
    std::optional<std::uint64_t> providerUsed;
    bool providerOverflow{};
    switch (action) {
    case Domain::ContextBudgetAction::Normal:
        providerUsed = 5'000U;
        break;
    case Domain::ContextBudgetAction::Checkpoint:
        providerUsed = 7'500U;
        break;
    case Domain::ContextBudgetAction::Rollover:
        providerUsed = 8'500U;
        break;
    case Domain::ContextBudgetAction::Emergency:
        providerOverflow = true;
        break;
    }
    return Domain::ContextBudgetSignals{
        10'000U,
        1'000U,
        std::nullopt,
        providerUsed,
        std::nullopt,
        std::nullopt,
        providerOverflow};
}

[[nodiscard]] Domain::ContinuityOperation operationFor(
    const Domain::ContinuityHandoff& handoff,
    const Domain::ContinuityState state,
    const std::optional<Domain::SessionId>& successor = std::nullopt)
{
    const bool completed = state == Domain::ContinuityState::Completed;
    return Domain::ContinuityOperation{
        handoff.operationId,
        handoff.project.projectId,
        handoff.predecessorSession.sessionId,
        successor,
        handoff.handoffId,
        state,
        completed ? 8U : 2U,
        handoff.hostState.adapterId,
        take(Domain::IdempotencyKey::create(handoff.operationId.value())),
        completed ? successor : std::nullopt,
        completed
            ? std::optional<Domain::ContinuityHandoffId>{handoff.handoffId}
            : std::nullopt,
        handoff.createdAt,
        handoff.createdAt,
        std::nullopt,
        std::nullopt,
        handoff.contentSha256,
        std::nullopt};
}

class ScriptedCoordinator final
    : public Contracts::IContinuityCoordinator {
public:
    [[nodiscard]] Domain::Result<Domain::CheckpointOutcome> checkpoint(
        const Domain::CheckpointRequest& request,
        const Domain::OperationContext& context) noexcept override
    {
        try {
            std::unique_lock lock{mutex_};
            ++checkpointCalls_;
            lastCheckpoint_ = request;
            if (blockCheckpoint_) {
                checkpointEntered_ = true;
                condition_.notify_all();
                condition_.wait(lock, [&] {
                    return !blockCheckpoint_ || shutdown_ ||
                           containsCancelled(context.operationId);
                });
            }
            if (shutdown_ || containsCancelled(context.operationId) ||
                context.isCancellationRequested()) {
                return cancelled<Domain::CheckpointOutcome>();
            }
            if (checkpointError_) {
                return Domain::Result<Domain::CheckpointOutcome>::failure(
                    *checkpointError_);
            }
            remember(request.handoff);
            auto operation = operationFor(
                request.handoff,
                Domain::ContinuityState::CheckpointPersisted);
            if (corruptCheckpointBinding_) {
                operation.handoffId = parse<Domain::ContinuityHandoffId>(
                    uuidText(88'888U));
            }
            return Domain::Result<Domain::CheckpointOutcome>::success(
                Domain::CheckpointOutcome{operation, request.handoff});
        } catch (...) {
            return internal<Domain::CheckpointOutcome>();
        }
    }

    [[nodiscard]] Domain::Result<Domain::CheckpointOutcome> prepareHandoff(
        const Domain::CheckpointRequest& request,
        const Domain::OperationContext& context) noexcept override
    {
        return checkpoint(request, context);
    }

    [[nodiscard]] Domain::Result<std::optional<Domain::ContinuityHandoff>>
    getPendingHandoff(
        const Domain::ProjectId& projectId,
        const Domain::OperationContext&) noexcept override
    {
        try {
            std::lock_guard lock{mutex_};
            const auto found = find(projectId);
            return Domain::Result<
                std::optional<Domain::ContinuityHandoff>>::success(
                    found == handoffs_.end()
                        ? std::nullopt
                        : std::optional<Domain::ContinuityHandoff>{*found});
        } catch (...) {
            return internal<std::optional<Domain::ContinuityHandoff>>();
        }
    }

    [[nodiscard]] Domain::Result<Domain::ContinuityOperation>
    acknowledgeHandoff(
        const Domain::ProjectId&,
        const Domain::ContinuityOperationId&,
        const Domain::HandoffAcknowledgement&,
        const Domain::OperationContext&) noexcept override
    {
        return unsupported<Domain::ContinuityOperation>();
    }

    [[nodiscard]] Domain::Result<Domain::HandoffResumeOutcome> resume(
        const Domain::HandoffResumeRequest& request,
        const Domain::OperationContext& context) noexcept override
    {
        try {
            std::lock_guard lock{mutex_};
            ++resumeCalls_;
            lastResume_ = request;
            if (shutdown_ || containsCancelled(context.operationId) ||
                context.isCancellationRequested()) {
                return cancelled<Domain::HandoffResumeOutcome>();
            }
            if (resumeError_) {
                return Domain::Result<Domain::HandoffResumeOutcome>::failure(
                    *resumeError_);
            }
            const auto found = find(request.projectId);
            if (found == handoffs_.end()) {
                return missing<Domain::HandoffResumeOutcome>();
            }
            auto handoff = *found;
            auto successorId = request.successorSessionId;
            auto sessionId = wrongResumeSession_
                ? parse<Domain::SessionId>(uuidText(77'777U))
                : successorId;
            handoff.successorSession = Domain::ContinuitySession{
                successorId, std::nullopt, std::nullopt, std::nullopt};
            auto operation = operationFor(
                handoff, Domain::ContinuityState::Completed, successorId);
            return Domain::Result<Domain::HandoffResumeOutcome>::success(
                Domain::HandoffResumeOutcome{
                    operation,
                    handoff,
                    Domain::HostSession{
                        sessionId,
                        handoff.project.projectId,
                        handoff.operationId,
                        handoff.predecessorSession.sessionId,
                        operation.idempotencyKey,
                        std::nullopt,
                        std::optional<std::string>{"test-model"},
                        Domain::HostSessionStatus::Ready}});
        } catch (...) {
            return internal<Domain::HandoffResumeOutcome>();
        }
    }

    [[nodiscard]] Domain::Result<Domain::ContinuityStatus> status(
        const Domain::ProjectId&,
        const Domain::OperationContext&) noexcept override
    {
        return unsupported<Domain::ContinuityStatus>();
    }

    [[nodiscard]] Domain::Result<Domain::RolloverOutcome> requestRollover(
        const Domain::RolloverRequest& request,
        const Domain::OperationContext& context) noexcept override
    {
        try {
            std::lock_guard lock{mutex_};
            ++rolloverCalls_;
            lastRollover_ = request;
            if (shutdown_ || containsCancelled(context.operationId) ||
                context.isCancellationRequested()) {
                return cancelled<Domain::RolloverOutcome>();
            }
            if (rolloverError_) {
                return Domain::Result<Domain::RolloverOutcome>::failure(
                    *rolloverError_);
            }
            const auto found = find(request.projectId);
            if (found == handoffs_.end() || found->operationId != request.operationId) {
                return missing<Domain::RolloverOutcome>();
            }
            const auto successorId = parse<Domain::SessionId>(
                request.operationId.value());
            auto operation = operationFor(
                *found, Domain::ContinuityState::Completed, successorId);
            if (!rolloverAcknowledged_) {
                operation.acknowledgedSessionId.reset();
                operation.acknowledgedHandoffId.reset();
            }
            std::optional<Domain::HostSession> successor{
                Domain::HostSession{
                    successorId,
                    found->project.projectId,
                    found->operationId,
                    found->predecessorSession.sessionId,
                    operation.idempotencyKey,
                    std::nullopt,
                    std::optional<std::string>{"test-model"},
                    Domain::HostSessionStatus::Ready}};
            if (omitSuccessorAfterFirst_ && rolloverCalls_ > 1U) {
                successor.reset();
            }
            return Domain::Result<Domain::RolloverOutcome>::success(
                Domain::RolloverOutcome{
                    operation,
                    std::move(successor),
                    rolloverAcknowledged_,
                    rolloverAcknowledged_});
        } catch (...) {
            return internal<Domain::RolloverOutcome>();
        }
    }

    [[nodiscard]] Domain::Result<Domain::ContinuityRecoveryReport>
    recoverIncompleteOperations(
        const Domain::ContinuityRecoveryRequest&,
        const Domain::OperationContext&) noexcept override
    {
        return unsupported<Domain::ContinuityRecoveryReport>();
    }

    [[nodiscard]] Domain::Result<Domain::ContinuityResetReport>
    resetProjectContinuity(
        const Domain::ContinuityResetRequest&,
        const Domain::OperationContext&) noexcept override
    {
        return unsupported<Domain::ContinuityResetReport>();
    }

    void cancel(const Domain::OperationId& operationId) noexcept override
    {
        try {
            std::lock_guard lock{mutex_};
            cancelled_.push_back(operationId);
            condition_.notify_all();
        } catch (...) {
        }
    }

    void shutdown() noexcept override
    {
        try {
            std::lock_guard lock{mutex_};
            shutdown_ = true;
            condition_.notify_all();
        } catch (...) {
        }
    }

    void setCorruptCheckpointBinding(const bool value) noexcept
    {
        std::lock_guard lock{mutex_};
        corruptCheckpointBinding_ = value;
    }

    void setRolloverAcknowledged(const bool value) noexcept
    {
        std::lock_guard lock{mutex_};
        rolloverAcknowledged_ = value;
    }

    void setWrongResumeSession(const bool value) noexcept
    {
        std::lock_guard lock{mutex_};
        wrongResumeSession_ = value;
    }

    void setOmitSuccessorAfterFirst(const bool value) noexcept
    {
        std::lock_guard lock{mutex_};
        omitSuccessorAfterFirst_ = value;
    }

    void setCheckpointError(Domain::Error error)
    {
        std::lock_guard lock{mutex_};
        checkpointError_ = std::move(error);
    }

    void blockCheckpoint() noexcept
    {
        std::lock_guard lock{mutex_};
        blockCheckpoint_ = true;
        checkpointEntered_ = false;
    }

    [[nodiscard]] bool waitForCheckpointEntry()
    {
        std::unique_lock lock{mutex_};
        return condition_.wait_for(
            lock, 5s, [&] { return checkpointEntered_; });
    }

    [[nodiscard]] std::size_t checkpointCalls() const noexcept
    {
        std::lock_guard lock{mutex_};
        return checkpointCalls_;
    }

    [[nodiscard]] std::size_t rolloverCalls() const noexcept
    {
        std::lock_guard lock{mutex_};
        return rolloverCalls_;
    }

    [[nodiscard]] std::size_t resumeCalls() const noexcept
    {
        std::lock_guard lock{mutex_};
        return resumeCalls_;
    }

    [[nodiscard]] std::size_t cancelCalls() const noexcept
    {
        std::lock_guard lock{mutex_};
        return cancelled_.size();
    }

    [[nodiscard]] std::optional<Domain::RolloverRequest>
    lastRollover() const
    {
        std::lock_guard lock{mutex_};
        return lastRollover_;
    }

    [[nodiscard]] std::optional<Domain::HandoffResumeRequest>
    lastResume() const
    {
        std::lock_guard lock{mutex_};
        return lastResume_;
    }

private:
    template <typename T>
    [[nodiscard]] static Domain::Result<T> unsupported() noexcept
    {
        return Domain::Result<T>::failure(Domain::makeError(
            Domain::ErrorCodes::InvalidRequest,
            "The scripted continuity operation is not used by this test."));
    }

    template <typename T>
    [[nodiscard]] static Domain::Result<T> cancelled() noexcept
    {
        return Domain::Result<T>::failure(Domain::makeError(
            Domain::ErrorCodes::Cancelled,
            "The scripted continuity operation was cancelled."));
    }

    template <typename T>
    [[nodiscard]] static Domain::Result<T> missing() noexcept
    {
        return Domain::Result<T>::failure(Domain::makeError(
            Domain::ErrorCodes::RecordNotFound,
            "The scripted continuity handoff was not found."));
    }

    template <typename T>
    [[nodiscard]] static Domain::Result<T> internal() noexcept
    {
        return Domain::Result<T>::failure(Domain::makeError(
            Domain::ErrorCodes::InternalFailure,
            "The scripted continuity coordinator failed safely."));
    }

    [[nodiscard]] bool containsCancelled(
        const Domain::OperationId& operationId) const
    {
        return std::find(cancelled_.begin(), cancelled_.end(), operationId) !=
               cancelled_.end();
    }

    void remember(const Domain::ContinuityHandoff& handoff)
    {
        const auto found = find(handoff.project.projectId);
        if (found == handoffs_.end()) {
            handoffs_.push_back(handoff);
        } else {
            *found = handoff;
        }
    }

    [[nodiscard]] auto find(const Domain::ProjectId& projectId)
    {
        return std::find_if(
            handoffs_.begin(), handoffs_.end(), [&](const auto& handoff) {
                return handoff.project.projectId == projectId;
            });
    }

    [[nodiscard]] auto find(const Domain::ProjectId& projectId) const
    {
        return std::find_if(
            handoffs_.cbegin(), handoffs_.cend(), [&](const auto& handoff) {
                return handoff.project.projectId == projectId;
            });
    }

    mutable std::mutex mutex_;
    std::condition_variable condition_;
    std::vector<Domain::ContinuityHandoff> handoffs_;
    std::vector<Domain::OperationId> cancelled_;
    std::optional<Domain::CheckpointRequest> lastCheckpoint_;
    std::optional<Domain::RolloverRequest> lastRollover_;
    std::optional<Domain::HandoffResumeRequest> lastResume_;
    std::optional<Domain::Error> checkpointError_;
    std::optional<Domain::Error> rolloverError_;
    std::optional<Domain::Error> resumeError_;
    std::size_t checkpointCalls_{};
    std::size_t rolloverCalls_{};
    std::size_t resumeCalls_{};
    bool corruptCheckpointBinding_{};
    bool rolloverAcknowledged_{true};
    bool wrongResumeSession_{};
    bool omitSuccessorAfterFirst_{};
    bool blockCheckpoint_{};
    bool checkpointEntered_{};
    bool shutdown_{};
};

void normalAndCheckpointActionsAreNarrow()
{
    const auto now = Domain::UtcTimePoint{1'800'000'000s};
    Fakes::FakeClock clock{now, Domain::MonotonicTimePoint{10s}};
    ScriptedCoordinator coordinator;
    Application::ContinuityAutomation automation{coordinator, clock};
    const auto handoff = handoffFor(1U, now);

    const auto normal = take(automation.observe(
        Domain::ContinuityAutomationObservation{
            handoff, signalsFor(Domain::ContextBudgetAction::Normal)},
        operationContext(clock, 1U, "automation-normal")));
    REQUIRE(normal.action == Domain::ContextBudgetAction::Normal);
    REQUIRE(!normal.checkpointPersisted);
    REQUIRE(!normal.rolloverRequested);
    REQUIRE(!normal.successorActivated);
    REQUIRE(!normal.operationId.has_value());
    REQUIRE(coordinator.checkpointCalls() == 0U);
    REQUIRE(coordinator.rolloverCalls() == 0U);
    REQUIRE(coordinator.resumeCalls() == 0U);
    REQUIRE(automation.trackedProjectCount() == 1U);

    const auto checkpoint = take(automation.observe(
        Domain::ContinuityAutomationObservation{
            handoff, signalsFor(Domain::ContextBudgetAction::Checkpoint)},
        operationContext(clock, 2U, "automation-checkpoint")));
    REQUIRE(checkpoint.action == Domain::ContextBudgetAction::Checkpoint);
    REQUIRE(checkpoint.checkpointPersisted);
    REQUIRE(!checkpoint.rolloverRequested);
    REQUIRE(!checkpoint.successorActivated);
    REQUIRE(checkpoint.operationId == handoff.operationId);
    REQUIRE(coordinator.checkpointCalls() == 1U);
    REQUIRE(coordinator.rolloverCalls() == 0U);
    REQUIRE(coordinator.resumeCalls() == 0U);
    REQUIRE(automation.trackedProjectCount() == 1U);

    const auto replay = take(automation.observe(
        Domain::ContinuityAutomationObservation{
            handoff, signalsFor(Domain::ContextBudgetAction::Checkpoint)},
        operationContext(clock, 3U, "automation-checkpoint-replay")));
    REQUIRE(replay.operationId == checkpoint.operationId);
    REQUIRE(coordinator.checkpointCalls() == 2U);
}

void rolloverAndEmergencyActivateWithoutOperatorAction()
{
    for (const auto [identifier, action] : {
             std::pair{10U, Domain::ContextBudgetAction::Rollover},
             std::pair{11U, Domain::ContextBudgetAction::Emergency}}) {
        const auto now = Domain::UtcTimePoint{1'800'000'000s};
        Fakes::FakeClock clock{now, Domain::MonotonicTimePoint{10s}};
        ScriptedCoordinator coordinator;
        coordinator.setOmitSuccessorAfterFirst(true);
        Application::ContinuityAutomation automation{coordinator, clock};
        const auto handoff = handoffFor(identifier, now);
        const Domain::ContinuityAutomationObservation observation{
            handoff, signalsFor(action)};

        const auto first = take(automation.observe(
            observation,
            operationContext(clock, identifier, "automation-rollover-first")));
        REQUIRE(first.action == action);
        REQUIRE(first.checkpointPersisted);
        REQUIRE(first.rolloverRequested);
        REQUIRE(first.successorActivated);
        REQUIRE(first.operationId == handoff.operationId);
        REQUIRE(first.successorSessionId ==
                std::optional<Domain::SessionId>{
                    parse<Domain::SessionId>(handoff.operationId.value())});

        const auto second = take(automation.observe(
            observation,
            operationContext(
                clock, identifier + 100U, "automation-rollover-replay")));
        REQUIRE(second.operationId == first.operationId);
        REQUIRE(second.successorSessionId == first.successorSessionId);
        REQUIRE(second.successorActivated);
        REQUIRE(coordinator.checkpointCalls() == 2U);
        REQUIRE(coordinator.rolloverCalls() == 2U);
        REQUIRE(coordinator.resumeCalls() == 2U);
        const auto rollover = coordinator.lastRollover();
        const auto resume = coordinator.lastResume();
        REQUIRE(rollover.has_value());
        REQUIRE(rollover->projectId == handoff.project.projectId);
        REQUIRE(rollover->operationId == handoff.operationId);
        REQUIRE(resume.has_value());
        REQUIRE(resume->projectId == handoff.project.projectId);
        REQUIRE(resume->handoffId == handoff.handoffId);
        REQUIRE(resume->successorSessionId == *first.successorSessionId);
    }
}

void bindingAndCoordinatorFailuresStopTheChain()
{
    const auto now = Domain::UtcTimePoint{1'800'000'000s};
    const auto handoff = handoffFor(20U, now);
    const Domain::ContinuityAutomationObservation observation{
        handoff, signalsFor(Domain::ContextBudgetAction::Rollover)};

    {
        Fakes::FakeClock clock{now, Domain::MonotonicTimePoint{10s}};
        ScriptedCoordinator coordinator;
        coordinator.setCorruptCheckpointBinding(true);
        Application::ContinuityAutomation automation{coordinator, clock};
        requireError(
            automation.observe(
                observation,
                operationContext(clock, 20U, "automation-corrupt-checkpoint")),
            Domain::ErrorCodes::IntegrityFailure);
        REQUIRE(coordinator.checkpointCalls() == 1U);
        REQUIRE(coordinator.rolloverCalls() == 0U);
        REQUIRE(coordinator.resumeCalls() == 0U);
    }

    {
        Fakes::FakeClock clock{now, Domain::MonotonicTimePoint{10s}};
        ScriptedCoordinator coordinator;
        coordinator.setRolloverAcknowledged(false);
        Application::ContinuityAutomation automation{coordinator, clock};
        requireError(
            automation.observe(
                observation,
                operationContext(clock, 21U, "automation-missing-ack")),
            Domain::ErrorCodes::IntegrityFailure);
        REQUIRE(coordinator.checkpointCalls() == 1U);
        REQUIRE(coordinator.rolloverCalls() == 1U);
        REQUIRE(coordinator.resumeCalls() == 0U);
    }

    {
        Fakes::FakeClock clock{now, Domain::MonotonicTimePoint{10s}};
        ScriptedCoordinator coordinator;
        coordinator.setWrongResumeSession(true);
        Application::ContinuityAutomation automation{coordinator, clock};
        requireError(
            automation.observe(
                observation,
                operationContext(clock, 22U, "automation-wrong-resume")),
            Domain::ErrorCodes::IntegrityFailure);
        REQUIRE(coordinator.checkpointCalls() == 1U);
        REQUIRE(coordinator.rolloverCalls() == 1U);
        REQUIRE(coordinator.resumeCalls() == 1U);
    }

    {
        Fakes::FakeClock clock{now, Domain::MonotonicTimePoint{10s}};
        ScriptedCoordinator coordinator;
        coordinator.setCheckpointError(Domain::makeError(
            Domain::ErrorCodes::DatabaseBusy,
            "The deterministic checkpoint is busy.",
            true));
        Application::ContinuityAutomation automation{coordinator, clock};
        const auto result = automation.observe(
            observation,
            operationContext(clock, 23U, "automation-checkpoint-error"));
        requireError(result, Domain::ErrorCodes::DatabaseBusy);
        REQUIRE(result.error().retryable);
        REQUIRE(coordinator.rolloverCalls() == 0U);
        REQUIRE(coordinator.resumeCalls() == 0U);
    }
}

void cancellationDeadlineBudgetAndShutdownFailBeforeEffects()
{
    const auto now = Domain::UtcTimePoint{1'800'000'000s};
    Fakes::FakeClock clock{now, Domain::MonotonicTimePoint{10s}};
    ScriptedCoordinator coordinator;
    Application::ContinuityAutomation automation{coordinator, clock};
    const auto handoff = handoffFor(30U, now);
    const Domain::ContinuityAutomationObservation observation{
        handoff, signalsFor(Domain::ContextBudgetAction::Checkpoint)};

    std::stop_source cancellation;
    cancellation.request_stop();
    requireError(
        automation.observe(
            observation,
            operationContext(
                clock,
                30U,
                "automation-cancelled",
                cancellation.get_token())),
        Domain::ErrorCodes::Cancelled);
    requireError(
        automation.observe(
            observation,
            operationContext(
                clock, 31U, "automation-expired", {}, true)),
        Domain::ErrorCodes::DeadlineExceeded);

    auto invalidBudget = signalsFor(Domain::ContextBudgetAction::Checkpoint);
    invalidBudget.providerRemaining = invalidBudget.capacity + 1U;
    requireError(
        automation.observe(
            Domain::ContinuityAutomationObservation{handoff, invalidBudget},
            operationContext(clock, 32U, "automation-invalid-budget")),
        Domain::ErrorCodes::InvalidRequest);
    REQUIRE(coordinator.checkpointCalls() == 0U);
    REQUIRE(automation.trackedProjectCount() == 0U);

    const auto explicitCancel = operationContext(
        clock, 33U, "automation-explicit-cancel").operationId;
    automation.cancel(explicitCancel);
    REQUIRE(coordinator.cancelCalls() == 1U);
    automation.shutdown();
    automation.shutdown();
    REQUIRE(automation.trackedProjectCount() == 0U);
    requireError(
        automation.observe(
            observation,
            operationContext(clock, 34U, "automation-after-shutdown")),
        Domain::ErrorCodes::TransportClosed);
    REQUIRE(coordinator.checkpointCalls() == 0U);
}

void trackedProjectsAreCappedAt128()
{
    const auto now = Domain::UtcTimePoint{1'800'000'000s};
    Fakes::FakeClock clock{now, Domain::MonotonicTimePoint{10s}};
    ScriptedCoordinator coordinator;
    Application::ContinuityAutomation automation{coordinator, clock};
    for (std::uint64_t index = 1U; index <= 128U; ++index) {
        const auto result = automation.observe(
            Domain::ContinuityAutomationObservation{
                handoffFor(1'000U + index, now),
                signalsFor(Domain::ContextBudgetAction::Checkpoint)},
            operationContext(
                clock,
                1'000U + index,
                "automation-bounded-project"));
        REQUIRE(result);
    }
    REQUIRE(automation.trackedProjectCount() == 128U);
    REQUIRE(coordinator.checkpointCalls() == 128U);

    const auto overflow = automation.observe(
        Domain::ContinuityAutomationObservation{
            handoffFor(2'000U, now),
            signalsFor(Domain::ContextBudgetAction::Checkpoint)},
        operationContext(clock, 2'000U, "automation-project-overflow"));
    requireError(overflow, Domain::ErrorCodes::LimitExceeded);
    REQUIRE(automation.trackedProjectCount() == 128U);
    REQUIRE(coordinator.checkpointCalls() == 128U);
}

void sameProjectContentionIsImmediateAndShutdownCancelsActiveWork()
{
    const auto now = Domain::UtcTimePoint{1'800'000'000s};
    Fakes::FakeClock clock{now, Domain::MonotonicTimePoint{10s}};
    ScriptedCoordinator coordinator;
    coordinator.blockCheckpoint();
    Application::ContinuityAutomation automation{coordinator, clock};
    const auto handoff = handoffFor(40U, now);
    const Domain::ContinuityAutomationObservation observation{
        handoff, signalsFor(Domain::ContextBudgetAction::Checkpoint)};
    const auto firstContext = operationContext(
        clock, 40U, "automation-blocked-first");
    std::optional<Domain::Result<Domain::ContinuityAutomationOutcome>> first;
    std::jthread worker{[&] {
        first.emplace(automation.observe(observation, firstContext));
    }};
    REQUIRE(coordinator.waitForCheckpointEntry());

    const auto contender = automation.observe(
        observation,
        operationContext(clock, 41U, "automation-busy-contender"));
    requireError(contender, Domain::ErrorCodes::DatabaseBusy);
    REQUIRE(contender.error().retryable);
    REQUIRE(coordinator.checkpointCalls() == 1U);

    automation.shutdown();
    worker.join();
    REQUIRE(first.has_value());
    requireError(*first, Domain::ErrorCodes::Cancelled);
    REQUIRE(coordinator.cancelCalls() == 1U);
    REQUIRE(automation.trackedProjectCount() == 0U);
    requireError(
        automation.observe(
            observation,
            operationContext(clock, 42U, "automation-shutdown-closed")),
        Domain::ErrorCodes::TransportClosed);
}

void budgetSourcePrecedenceIsDeterministic()
{
    Domain::ContextBudgetSignals signals{
        1'000U,
        100U,
        700U,
        800U,
        850U,
        3'500U,
        false};

    auto budget = take(Domain::resolveContextBudget(signals));
    REQUIRE(budget.source == Domain::ContextBudgetSource::ProviderRemaining);
    REQUIRE(budget.used == 300U);
    REQUIRE(budget.remaining == 600U);
    REQUIRE(budget.action == Domain::ContextBudgetAction::Normal);

    signals.providerRemaining.reset();
    budget = take(Domain::resolveContextBudget(signals));
    REQUIRE(budget.source == Domain::ContextBudgetSource::ProviderUsage);
    REQUIRE(budget.action == Domain::ContextBudgetAction::Checkpoint);

    signals.providerUsed.reset();
    budget = take(Domain::resolveContextBudget(signals));
    REQUIRE(
        budget.source ==
        Domain::ContextBudgetSource::ConfiguredModelEstimate);
    REQUIRE(budget.action == Domain::ContextBudgetAction::Rollover);

    signals.configuredModelUsed.reset();
    budget = take(Domain::resolveContextBudget(signals));
    REQUIRE(budget.source == Domain::ContextBudgetSource::SerializedEstimate);
    REQUIRE(budget.action == Domain::ContextBudgetAction::Rollover);

    signals.providerOverflow = true;
    budget = take(Domain::resolveContextBudget(signals));
    REQUIRE(budget.source == Domain::ContextBudgetSource::ProviderOverflow);
    REQUIRE(budget.action == Domain::ContextBudgetAction::Emergency);

    signals.providerOverflow = false;
    signals.serializedBytes.reset();
    requireError(
        Domain::resolveContextBudget(signals),
        Domain::ErrorCodes::InvalidRequest);
}

void progressAndTimePoliciesTriggerWithoutCallerSelectedActions()
{
    const auto now = Domain::UtcTimePoint{1'800'000'000s};
    {
        Fakes::FakeClock clock{now, Domain::MonotonicTimePoint{10s}};
        ScriptedCoordinator coordinator;
        const Domain::ContinuityAutomationPolicy policy{
            2U, 4U, 3'600U, 7'200U, 0.20, 0.10};
        Application::ContinuityAutomation automation{
            coordinator, clock, policy};
        const auto handoff = handoffFor(50U, now);
        auto observation = Domain::ContinuityAutomationObservation{
            handoff,
            signalsFor(Domain::ContextBudgetAction::Normal),
            1U,
            false};

        const auto first = take(automation.observe(
            observation,
            operationContext(clock, 50U, "automation-progress-first")));
        REQUIRE(first.action == Domain::ContextBudgetAction::Normal);
        REQUIRE(coordinator.checkpointCalls() == 0U);

        const auto second = take(automation.observe(
            observation,
            operationContext(clock, 51U, "automation-progress-checkpoint")));
        REQUIRE(second.action == Domain::ContextBudgetAction::Checkpoint);
        REQUIRE(second.checkpointPersisted);
        REQUIRE(coordinator.checkpointCalls() == 1U);

        observation.completedProgressUnits = 2U;
        const auto fourth = take(automation.observe(
            observation,
            operationContext(clock, 52U, "automation-progress-rollover")));
        REQUIRE(fourth.action == Domain::ContextBudgetAction::Rollover);
        REQUIRE(fourth.rolloverRequested);
        REQUIRE(fourth.successorActivated);
        REQUIRE(coordinator.checkpointCalls() == 2U);
        REQUIRE(coordinator.rolloverCalls() == 1U);
        REQUIRE(coordinator.resumeCalls() == 1U);
    }

    {
        Fakes::FakeClock clock{now, Domain::MonotonicTimePoint{10s}};
        ScriptedCoordinator coordinator;
        const Domain::ContinuityAutomationPolicy policy{
            1'000U, 2'000U, 10U, 20U, 0.20, 0.10};
        Application::ContinuityAutomation automation{
            coordinator, clock, policy};
        const auto handoff = handoffFor(51U, now);
        const Domain::ContinuityAutomationObservation observation{
            handoff,
            signalsFor(Domain::ContextBudgetAction::Normal),
            0U,
            false};

        REQUIRE(take(automation.observe(
                    observation,
                    operationContext(clock, 53U, "automation-time-anchor")))
                    .action == Domain::ContextBudgetAction::Normal);
        clock.advance(10s);
        REQUIRE(take(automation.observe(
                    observation,
                    operationContext(clock, 54U, "automation-time-checkpoint")))
                    .action == Domain::ContextBudgetAction::Checkpoint);
        clock.advance(10s);
        const auto rollover = take(automation.observe(
            observation,
            operationContext(clock, 55U, "automation-time-rollover")));
        REQUIRE(rollover.action == Domain::ContextBudgetAction::Rollover);
        REQUIRE(rollover.successorActivated);
    }

    {
        Fakes::FakeClock clock{now, Domain::MonotonicTimePoint{10s}};
        ScriptedCoordinator coordinator;
        const Domain::ContinuityAutomationPolicy invalid{
            5U, 4U, 10U, 20U, 0.20, 0.10};
        Application::ContinuityAutomation automation{
            coordinator, clock, invalid};
        requireError(
            automation.observe(
                Domain::ContinuityAutomationObservation{
                    handoffFor(52U, now),
                    signalsFor(Domain::ContextBudgetAction::Normal),
                    1U,
                    false},
                operationContext(clock, 56U, "automation-invalid-policy")),
            Domain::ErrorCodes::InvalidRequest);

        auto oversized = Domain::ContinuityAutomationObservation{
            handoffFor(53U, now),
            signalsFor(Domain::ContextBudgetAction::Normal),
            Domain::MaximumProgressUnitsPerObservation + 1U,
            false};
        Application::ContinuityAutomation bounded{coordinator, clock};
        requireError(
            bounded.observe(
                oversized,
                operationContext(clock, 57U, "automation-progress-bound")),
            Domain::ErrorCodes::LimitExceeded);
    }
}

} // namespace

int main()
{
    try {
        normalAndCheckpointActionsAreNarrow();
        std::cout << "PASS continuity_automation.normal_checkpoint\n";
        rolloverAndEmergencyActivateWithoutOperatorAction();
        std::cout << "PASS continuity_automation.rollover_emergency_activation\n";
        bindingAndCoordinatorFailuresStopTheChain();
        std::cout << "PASS continuity_automation.binding_fail_closed\n";
        cancellationDeadlineBudgetAndShutdownFailBeforeEffects();
        std::cout << "PASS continuity_automation.context_shutdown\n";
        trackedProjectsAreCappedAt128();
        std::cout << "PASS continuity_automation.project_bound\n";
        sameProjectContentionIsImmediateAndShutdownCancelsActiveWork();
        std::cout << "PASS continuity_automation.contention_cancellation\n";
        budgetSourcePrecedenceIsDeterministic();
        std::cout << "PASS continuity_automation.budget_source_precedence\n";
        progressAndTimePoliciesTriggerWithoutCallerSelectedActions();
        std::cout << "PASS continuity_automation.progress_time_policy\n";
        std::cout << "SUMMARY passed=8 failed=0 assertions="
                  << assertionCount.load(std::memory_order_relaxed) << '\n';
        return EXIT_SUCCESS;
    } catch (const std::exception& error) {
        std::cerr << "FAIL " << error.what() << '\n';
        return EXIT_FAILURE;
    }
}
