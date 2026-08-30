#include "Infrastructure/Windows/Detail/DashboardShutdownDrain.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

namespace {

namespace Detail = ForgeConductor::Infrastructure::Windows::Detail;
namespace Domain = ForgeConductor::Domain;
namespace Windows = ForgeConductor::Infrastructure::Windows;

using Deadline = Windows::WindowsDashboardDeadline;
using DeadlineKind = Windows::WindowsDashboardDeadlineKind;
using DeadlineRequest = Windows::WindowsDashboardDeadlineRequest;
using Drain = Detail::DashboardShutdownDrain;
using DrainHost = Detail::IDashboardShutdownDrainHost;
using DrainLifecycle = Detail::DashboardShutdownDrainLifecycle;
using Failure = Detail::DashboardDeadlineIocpFailure;
using FailureKind = Detail::DashboardDeadlineIocpFailureKind;
using SchedulerFailure =
    Windows::WindowsDashboardDeadlineSchedulerFailure;
using SchedulerFailureKind =
    Windows::WindowsDashboardDeadlineSchedulerFailureKind;
using FinalizeDisposition =
    Detail::DashboardDeadlineRoutingFinalizeDisposition;

static_assert(std::is_final_v<Drain>);
static_assert(std::is_base_of_v<
              Detail::IDashboardDeadlineIocpBridgeFailureObserver,
              Drain>);
static_assert(std::is_base_of_v<
              Windows::IWindowsDashboardDeadlineSchedulerFailureObserver,
              Drain>);
static_assert(std::is_base_of_v<
              Detail::IDashboardAuxiliaryDeadlineTarget,
              Drain>);
static_assert(!std::is_copy_constructible_v<Drain>);
static_assert(!std::is_move_constructible_v<Drain>);
static_assert(Drain::GraceLifetime == std::chrono::seconds{5});

constexpr auto TestTimeout = std::chrono::seconds{5};
constexpr std::uint64_t RegistrationId = 7'301U;
constexpr std::uint64_t ArmSequence = 41U;

std::size_t assertionCount{};

[[noreturn]] void fail(const std::string_view message)
{
    throw std::runtime_error{std::string{message}};
}

void require(const bool condition, const std::string_view message)
{
    ++assertionCount;
    if (!condition) {
        fail(message);
    }
}

template <typename Value>
[[nodiscard]] Value take(Domain::Result<Value> result)
{
    if (!result) {
        fail(result.error().code + ": " + result.error().message);
    }
    return std::move(result).value();
}

void take(Domain::Result<void> result)
{
    if (!result) {
        fail(result.error().code + ": " + result.error().message);
    }
}

[[nodiscard]] Domain::Error testError(const std::string_view message)
{
    return Domain::makeError(
        Domain::ErrorCodes::IntegrityFailure, std::string{message});
}

class FailFastRecorder final
    : public Detail::IDashboardShutdownDrainFailFast {
public:
    void failFast() noexcept override
    {
        const std::scoped_lock lock{mutex_};
        ++count_;
        thread_ = std::this_thread::get_id();
        changed_.notify_all();
    }

    [[nodiscard]] std::size_t count() const noexcept
    {
        const std::scoped_lock lock{mutex_};
        return count_;
    }

    [[nodiscard]] bool waitForCount(const std::size_t expected) noexcept
    {
        std::unique_lock lock{mutex_};
        return changed_.wait_for(lock, TestTimeout, [this, expected] {
            return count_ >= expected;
        });
    }

    [[nodiscard]] std::thread::id thread() const noexcept
    {
        const std::scoped_lock lock{mutex_};
        return thread_;
    }

private:
    mutable std::mutex mutex_;
    std::condition_variable changed_;
    std::size_t count_{};
    std::thread::id thread_{};
};

class FakeDrainHost final : public DrainHost {
public:
    enum class FinalizeMode : std::uint8_t {
        Immediate,
        ProgressDuringFirstCall,
        External,
    };

    struct Action final {
        std::string name;
        std::thread::id thread;
    };

    void failBindingAt(std::string name)
    {
        const std::scoped_lock lock{mutex_};
        failedBinding_ = std::move(name);
    }

    void setBlockExecutorJoin(const bool blocked) noexcept
    {
        const std::scoped_lock lock{mutex_};
        blockExecutorJoin_ = blocked;
        if (!blocked) {
            executorJoinReleased_ = true;
            changed_.notify_all();
        }
    }

    void releaseExecutorJoin() noexcept
    {
        const std::scoped_lock lock{mutex_};
        executorJoinReleased_ = true;
        changed_.notify_all();
    }

    void setDispatchDeadlineBeforeScheduleReturns(const bool enabled) noexcept
    {
        const std::scoped_lock lock{mutex_};
        dispatchBeforeScheduleReturns_ = enabled;
    }

    void setCancelResult(const bool result) noexcept
    {
        const std::scoped_lock lock{mutex_};
        cancelResult_ = result;
    }

    void setFinalizeMode(const FinalizeMode mode) noexcept
    {
        const std::scoped_lock lock{mutex_};
        finalizeMode_ = mode;
        routingFinalized_ = mode != FinalizeMode::External;
    }

    [[nodiscard]] Domain::Result<void> bindBridgeFailureObserver(
        std::weak_ptr<Detail::IDashboardDeadlineIocpBridgeFailureObserver>
            observer) noexcept override
    {
        return bind(
            "bind-bridge", bridgeFailureObserver_, std::move(observer));
    }

    [[nodiscard]] Domain::Result<void> bindHandlerDrainObserver(
        std::weak_ptr<Windows::IDashboardHandlerExecutorDrainObserver>
            observer) noexcept override
    {
        return bind(
            "bind-handler", handlerObserver_, std::move(observer));
    }

    [[nodiscard]] Domain::Result<void>
    bindDeadlineSchedulerFailureObserver(
        std::weak_ptr<
            Windows::IWindowsDashboardDeadlineSchedulerFailureObserver>
            observer) noexcept override
    {
        return bind(
            "bind-scheduler", schedulerFailureObserver_,
            std::move(observer));
    }

    [[nodiscard]] Domain::Result<void> bindListenerDrainObserver(
        std::weak_ptr<
            Detail::IDashboardListenerGenerationCoordinatorDrainObserver>
            observer) noexcept override
    {
        return bind(
            "bind-listener", listenerObserver_, std::move(observer));
    }

    [[nodiscard]] Domain::Result<void>
    bindRegistryConnectionDrainObserver(
        std::weak_ptr<Detail::IDashboardConnectionRegistryDrainObserver>
            observer) noexcept override
    {
        return bind(
            "bind-registry-connections",
            registryConnectionObserver_,
            std::move(observer));
    }

    [[nodiscard]] Domain::Result<void>
    bindRegistryRoutingProgressObserver(
        std::weak_ptr<
            Detail::IDashboardConnectionRegistryRoutingProgressObserver>
            observer) noexcept override
    {
        return bind(
            "bind-registry-routing",
            registryRoutingObserver_,
            std::move(observer));
    }

    [[nodiscard]] Domain::Result<void> bindOverloadDrainObserver(
        std::weak_ptr<Detail::IDashboardOverloadResponderSetDrainObserver>
            observer) noexcept override
    {
        return bind(
            "bind-overload", overloadObserver_, std::move(observer));
    }

    [[nodiscard]] Domain::Result<void> registerShutdownDeadlineTarget(
        std::shared_ptr<Detail::IDashboardAuxiliaryDeadlineTarget> target)
        noexcept override
    {
        const std::scoped_lock lock{mutex_};
        recordLocked("register-shutdown-deadline");
        if (failedBinding_ == "register-shutdown-deadline") {
            return Domain::Result<void>::failure(testError(
                "configured shutdown deadline registration failure"));
        }
        shutdownTarget_ = std::move(target);
        registeredAuxiliaryTargetCount_ = 2U;
        return Domain::Result<void>::success();
    }

    [[nodiscard]] Domain::MonotonicTimePoint monotonicNow()
        const noexcept override
    {
        return now_;
    }

    [[nodiscard]] Domain::Result<Deadline> scheduleShutdownDeadline(
        const DeadlineRequest request) noexcept override
    {
        std::shared_ptr<Detail::IDashboardAuxiliaryDeadlineTarget> target;
        Deadline armed{
            request.registrationId,
            ArmSequence,
            request.kind,
            request.deadline};
        bool dispatchEarly{};
        {
            const std::scoped_lock lock{mutex_};
            recordLocked("schedule-deadline");
            scheduledRequest_ = request;
            scheduledDeadline_ = armed;
            dispatchEarly = dispatchBeforeScheduleReturns_;
            target = shutdownTarget_.lock();
        }
        if (dispatchEarly && target != nullptr) {
            target->dispatchDeadline(armed);
        }
        return Domain::Result<Deadline>::success(armed);
    }

    [[nodiscard]] bool cancelShutdownDeadline(
        const std::uint64_t registrationId,
        const std::uint64_t armSequence) noexcept override
    {
        const std::scoped_lock lock{mutex_};
        recordLocked("cancel-deadline");
        cancelledRegistrationId_ = registrationId;
        cancelledArmSequence_ = armSequence;
        return cancelResult_;
    }

    void closeRuntimeAdmission() noexcept override
    {
        record("close-runtime-admission");
    }

    void closeHandlerAdmission() noexcept override
    {
        record("close-handler-admission");
    }

    void beginGracefulListenerShutdown() noexcept override
    {
        std::weak_ptr<
            Detail::IDashboardListenerGenerationCoordinatorDrainObserver>
            observer;
        {
            const std::scoped_lock lock{mutex_};
            recordLocked("listener-graceful");
            listenerRoutingDrained_ = true;
            observer = listenerObserver_;
        }
        if (const auto owner = observer.lock(); owner != nullptr) {
            owner->listenerGenerationsMayHaveDrained();
        }
    }

    void beginOverloadShutdown() noexcept override
    {
        std::weak_ptr<Detail::IDashboardOverloadResponderSetDrainObserver>
            observer;
        {
            const std::scoped_lock lock{mutex_};
            recordLocked("overload-shutdown");
            overloadFullyDrained_ = true;
            observer = overloadObserver_;
        }
        if (const auto owner = observer.lock(); owner != nullptr) {
            owner->overloadRespondersMayHaveDrained();
        }
    }

    void beginGracefulRegistryShutdown() noexcept override
    {
        publishRegistryDrain("registry-graceful");
    }

    void beginHardListenerShutdown() noexcept override
    {
        std::weak_ptr<
            Detail::IDashboardListenerGenerationCoordinatorDrainObserver>
            observer;
        {
            const std::scoped_lock lock{mutex_};
            recordLocked("listener-hard");
            listenerRoutingDrained_ = true;
            observer = listenerObserver_;
        }
        if (const auto owner = observer.lock(); owner != nullptr) {
            owner->listenerGenerationsMayHaveDrained();
        }
    }

    void beginHardRegistryShutdown() noexcept override
    {
        publishRegistryDrain("registry-hard");
    }

    void beginCompletionRouterShutdown() noexcept override
    {
        record("router-shutdown");
    }

    void joinHandlerExecutor() noexcept override
    {
        std::weak_ptr<Windows::IDashboardHandlerExecutorDrainObserver>
            observer;
        {
            std::unique_lock lock{mutex_};
            recordLocked("handler-join");
            if (blockExecutorJoin_ && !executorJoinReleased_) {
                static_cast<void>(changed_.wait_for(
                    lock, TestTimeout, [this] {
                        return executorJoinReleased_;
                    }));
            }
            executorFullyDrained_ = true;
            observer = handlerObserver_;
            recordLocked("handler-join-return");
        }
        if (const auto owner = observer.lock(); owner != nullptr) {
            owner->handlerExecutorMayHaveDrained();
        }
    }

    [[nodiscard]] Detail::DashboardShutdownDrainHostSnapshot snapshot()
        const noexcept override
    {
        const std::scoped_lock lock{mutex_};
        return Detail::DashboardShutdownDrainHostSnapshot{
            true,
            executorFullyDrained_,
            listenerRoutingDrained_,
            overloadFullyDrained_,
            registeredConnectionCount_,
            registeredAuxiliaryTargetCount_,
            fixedCompletionTargetCount_,
            routingProgressRevision_,
            !routingFinalized_,
            fatal_};
    }

    [[nodiscard]] bool unregisterOverloadDeadlineTarget() noexcept override
    {
        const std::scoped_lock lock{mutex_};
        recordLocked("unregister-overload-deadline");
        if (registeredAuxiliaryTargetCount_ > 0U) {
            --registeredAuxiliaryTargetCount_;
        }
        return true;
    }

    [[nodiscard]] bool unregisterOverloadCompletionTarget()
        noexcept override
    {
        const std::scoped_lock lock{mutex_};
        recordLocked("unregister-overload-completion");
        fixedCompletionTargetCount_ = 0U;
        return true;
    }

    [[nodiscard]] bool unregisterShutdownDeadlineTarget(
        const std::shared_ptr<Detail::IDashboardAuxiliaryDeadlineTarget>&
            target) noexcept override
    {
        const std::scoped_lock lock{mutex_};
        recordLocked("unregister-shutdown-deadline");
        const auto registered = shutdownTarget_.lock();
        if (registered == nullptr || registered.get() != target.get()) {
            return false;
        }
        shutdownTarget_.reset();
        registeredAuxiliaryTargetCount_ = 0U;
        return true;
    }

    void shutdownDeadlineScheduler() noexcept override
    {
        record("scheduler-shutdown");
    }

    [[nodiscard]] Domain::Result<FinalizeDisposition>
    finalizeDeadlineRouting() noexcept override
    {
        std::weak_ptr<
            Detail::IDashboardConnectionRegistryRoutingProgressObserver>
            observer;
        std::uint64_t revision{};
        FinalizeDisposition disposition{};
        {
            const std::scoped_lock lock{mutex_};
            recordLocked("finalize-routing");
            ++finalizeCallCount_;
            if (finalizeMode_ == FinalizeMode::ProgressDuringFirstCall &&
                finalizeCallCount_ == 1U) {
                ++routingProgressRevision_;
                revision = routingProgressRevision_;
                observer = registryRoutingObserver_;
                disposition = FinalizeDisposition::Pending;
            } else if (finalizeMode_ == FinalizeMode::External &&
                       !routingFinalized_) {
                disposition = FinalizeDisposition::Pending;
            } else {
                routingFinalized_ = true;
                disposition = FinalizeDisposition::Finalized;
            }
        }
        if (const auto owner = observer.lock(); owner != nullptr) {
            owner->registryRoutingMayHaveProgressed(revision);
        }
        return Domain::Result<FinalizeDisposition>::success(disposition);
    }

    void shutdownIocpKernel() noexcept override
    {
        record("kernel-shutdown");
    }

    void emitBridgeFailure(const Failure failure) noexcept
    {
        std::weak_ptr<Detail::IDashboardDeadlineIocpBridgeFailureObserver>
            observer;
        {
            const std::scoped_lock lock{mutex_};
            observer = bridgeFailureObserver_;
        }
        if (const auto owner = observer.lock(); owner != nullptr) {
            owner->dashboardDeadlineIocpBridgeFailed(failure);
        }
    }

    void emitSchedulerFailure(const SchedulerFailure failure) noexcept
    {
        std::weak_ptr<
            Windows::IWindowsDashboardDeadlineSchedulerFailureObserver>
            observer;
        {
            const std::scoped_lock lock{mutex_};
            observer = schedulerFailureObserver_;
        }
        if (const auto owner = observer.lock(); owner != nullptr) {
            owner->dashboardDeadlineSchedulerFailed(failure);
        }
    }

    void emitScheduledDeadline() noexcept
    {
        std::shared_ptr<Detail::IDashboardAuxiliaryDeadlineTarget> target;
        std::optional<Deadline> deadline;
        {
            const std::scoped_lock lock{mutex_};
            target = shutdownTarget_.lock();
            deadline = scheduledDeadline_;
        }
        if (target != nullptr && deadline.has_value()) {
            target->dispatchDeadline(*deadline);
        }
    }

    void finalizeExternallyAndPublishProgress() noexcept
    {
        std::weak_ptr<
            Detail::IDashboardConnectionRegistryRoutingProgressObserver>
            observer;
        std::uint64_t revision{};
        {
            const std::scoped_lock lock{mutex_};
            routingFinalized_ = true;
            ++routingProgressRevision_;
            revision = routingProgressRevision_;
            observer = registryRoutingObserver_;
        }
        if (const auto owner = observer.lock(); owner != nullptr) {
            owner->registryRoutingMayHaveProgressed(revision);
        }
    }

    [[nodiscard]] bool waitForAction(
        const std::string_view name,
        const std::size_t count = 1U) noexcept
    {
        std::unique_lock lock{mutex_};
        return changed_.wait_for(lock, TestTimeout, [this, name, count] {
            return actionCountLocked(name) >= count;
        });
    }

    [[nodiscard]] std::size_t actionCount(
        const std::string_view name) const noexcept
    {
        const std::scoped_lock lock{mutex_};
        return actionCountLocked(name);
    }

    [[nodiscard]] std::size_t actionIndex(
        const std::string_view name) const noexcept
    {
        const std::scoped_lock lock{mutex_};
        for (std::size_t index{}; index < actions_.size(); ++index) {
            if (actions_[index].name == name) {
                return index;
            }
        }
        return std::numeric_limits<std::size_t>::max();
    }

    [[nodiscard]] std::thread::id actionThread(
        const std::string_view name) const noexcept
    {
        const std::scoped_lock lock{mutex_};
        for (const auto& action : actions_) {
            if (action.name == name) {
                return action.thread;
            }
        }
        return {};
    }

    [[nodiscard]] std::optional<DeadlineRequest> scheduledRequest()
        const noexcept
    {
        const std::scoped_lock lock{mutex_};
        return scheduledRequest_;
    }

    [[nodiscard]] Domain::MonotonicTimePoint fixedNow() const noexcept
    {
        return now_;
    }

    [[nodiscard]] bool hasLiveShutdownTarget() const noexcept
    {
        const std::scoped_lock lock{mutex_};
        return !shutdownTarget_.expired();
    }

private:
    template <typename Observer>
    [[nodiscard]] Domain::Result<void> bind(
        const std::string_view name,
        std::weak_ptr<Observer>& destination,
        std::weak_ptr<Observer> observer) noexcept
    {
        const std::scoped_lock lock{mutex_};
        recordLocked(name);
        if (failedBinding_ == name) {
            return Domain::Result<void>::failure(
                testError("configured observer binding failure"));
        }
        destination = std::move(observer);
        return Domain::Result<void>::success();
    }

    void publishRegistryDrain(const std::string_view name) noexcept
    {
        std::weak_ptr<Detail::IDashboardConnectionRegistryDrainObserver>
            observer;
        {
            const std::scoped_lock lock{mutex_};
            recordLocked(name);
            registeredConnectionCount_ = 0U;
            observer = registryConnectionObserver_;
        }
        if (const auto owner = observer.lock(); owner != nullptr) {
            owner->registryConnectionsMayHaveDrained();
        }
    }

    void record(const std::string_view name) noexcept
    {
        const std::scoped_lock lock{mutex_};
        recordLocked(name);
    }

    void recordLocked(const std::string_view name)
    {
        actions_.push_back(Action{
            std::string{name}, std::this_thread::get_id()});
        changed_.notify_all();
    }

    [[nodiscard]] std::size_t actionCountLocked(
        const std::string_view name) const noexcept
    {
        return static_cast<std::size_t>(std::count_if(
            actions_.cbegin(), actions_.cend(), [&name](const Action& action) {
                return action.name == name;
            }));
    }

    mutable std::mutex mutex_;
    std::condition_variable changed_;
    std::vector<Action> actions_;
    std::string failedBinding_;
    std::weak_ptr<Detail::IDashboardDeadlineIocpBridgeFailureObserver>
        bridgeFailureObserver_;
    std::weak_ptr<
        Windows::IWindowsDashboardDeadlineSchedulerFailureObserver>
        schedulerFailureObserver_;
    std::weak_ptr<Windows::IDashboardHandlerExecutorDrainObserver>
        handlerObserver_;
    std::weak_ptr<
        Detail::IDashboardListenerGenerationCoordinatorDrainObserver>
        listenerObserver_;
    std::weak_ptr<Detail::IDashboardConnectionRegistryDrainObserver>
        registryConnectionObserver_;
    std::weak_ptr<
        Detail::IDashboardConnectionRegistryRoutingProgressObserver>
        registryRoutingObserver_;
    std::weak_ptr<Detail::IDashboardOverloadResponderSetDrainObserver>
        overloadObserver_;
    std::weak_ptr<Detail::IDashboardAuxiliaryDeadlineTarget>
        shutdownTarget_;
    const Domain::MonotonicTimePoint now_{
        std::chrono::milliseconds{9'000}};
    std::optional<DeadlineRequest> scheduledRequest_;
    std::optional<Deadline> scheduledDeadline_;
    std::uint64_t cancelledRegistrationId_{};
    std::uint64_t cancelledArmSequence_{};
    std::size_t registeredConnectionCount_{1U};
    std::size_t registeredAuxiliaryTargetCount_{1U};
    std::size_t fixedCompletionTargetCount_{1U};
    std::uint64_t routingProgressRevision_{};
    std::size_t finalizeCallCount_{};
    FinalizeMode finalizeMode_{FinalizeMode::Immediate};
    bool executorFullyDrained_{};
    bool listenerRoutingDrained_{};
    bool overloadFullyDrained_{};
    bool routingFinalized_{true};
    bool fatal_{};
    bool blockExecutorJoin_{};
    bool executorJoinReleased_{};
    bool dispatchBeforeScheduleReturns_{};
    bool cancelResult_{true};
};

[[nodiscard]] std::shared_ptr<Drain> createInstalled(
    const std::shared_ptr<FakeDrainHost>& host,
    const std::shared_ptr<FailFastRecorder>& failFast)
{
    auto drain = take(Drain::create(RegistrationId, host, failFast));
    take(drain->install());
    return drain;
}

void requireBefore(
    const FakeDrainHost& host,
    const std::string_view first,
    const std::string_view second)
{
    const auto firstIndex = host.actionIndex(first);
    const auto secondIndex = host.actionIndex(second);
    require(
        firstIndex != std::numeric_limits<std::size_t>::max(),
        "the expected first action was absent");
    require(
        secondIndex != std::numeric_limits<std::size_t>::max(),
        "the expected second action was absent");
    require(firstIndex < secondIndex, "shutdown action order was inverted");
}

void gracefulDrainUsesExactArmAndTeardownOrder()
{
    auto host = std::make_shared<FakeDrainHost>();
    auto failFast = std::make_shared<FailFastRecorder>();
    auto drain = createInstalled(host, failFast);

    drain->requestGracefulShutdown();
    take(drain->wait());

    const auto snapshot = drain->snapshot();
    require(snapshot.lifecycle() == DrainLifecycle::Drained,
            "graceful shutdown did not reach Drained");
    require(snapshot.hardEscalationCount() == 0U,
            "graceful shutdown escalated unexpectedly");
    require(failFast->count() == 0U,
            "graceful shutdown invoked fail-fast");

    const auto request = host->scheduledRequest();
    require(request.has_value(), "shutdown grace was not armed");
    require(request->registrationId == RegistrationId &&
                request->kind == DeadlineKind::ShutdownDrain,
            "shutdown grace used the wrong identity or deadline kind");
    require(request->deadline == host->fixedNow() + Drain::GraceLifetime,
            "shutdown grace did not use an independent exact five-second deadline");

    constexpr std::array<std::string_view, 16U> OrderedActions{
        "schedule-deadline",
        "close-runtime-admission",
        "close-handler-admission",
        "listener-graceful",
        "overload-shutdown",
        "registry-graceful",
        "handler-join",
        "handler-join-return",
        "cancel-deadline",
        "unregister-overload-deadline",
        "unregister-overload-completion",
        "unregister-shutdown-deadline",
        "router-shutdown",
        "scheduler-shutdown",
        "finalize-routing",
        "kernel-shutdown"};
    for (std::size_t index{1U}; index < OrderedActions.size(); ++index) {
        requireBefore(
            *host, OrderedActions[index - 1U], OrderedActions[index]);
    }
    require(host->actionIndex("bind-bridge") == 0U,
            "bridge failure observer was not bound first");
    requireBefore(
        *host, "bind-overload", "register-shutdown-deadline");
}

void hardDeadlineEscalatesWhileExecutorJoinIsBlocked()
{
    auto host = std::make_shared<FakeDrainHost>();
    host->setBlockExecutorJoin(true);
    auto failFast = std::make_shared<FailFastRecorder>();
    auto drain = createInstalled(host, failFast);

    drain->requestGracefulShutdown();
    const bool joinStarted = host->waitForAction("handler-join");
    host->emitScheduledDeadline();
    const bool hardListenerStarted = host->waitForAction("listener-hard");
    const bool joinStillBlocked =
        host->actionCount("handler-join-return") == 0U;
    host->releaseExecutorJoin();
    const auto result = drain->wait();

    require(joinStarted, "executor finalizer did not begin");
    require(hardListenerStarted,
            "deadline did not drive hard fanout while executor join blocked");
    require(joinStillBlocked,
            "executor join returned before the independent hard fanout");
    take(result);
    require(drain->snapshot().hardEscalationCount() == 1U,
            "hard deadline did not produce exactly one escalation");
    require(failFast->count() == 0U,
            "successful hard drain invoked fail-fast");
}

void routingRevisionClosesTheFinalizeLostWakeWindow()
{
    auto host = std::make_shared<FakeDrainHost>();
    host->setFinalizeMode(
        FakeDrainHost::FinalizeMode::ProgressDuringFirstCall);
    auto failFast = std::make_shared<FailFastRecorder>();
    auto drain = createInstalled(host, failFast);

    drain->requestGracefulShutdown();
    take(drain->wait());

    require(host->actionCount("finalize-routing") == 2U,
            "routing progress emitted inside finalization was lost");
    require(drain->snapshot().routingProgressRevision() == 1U,
            "routing progress revision was not retained");
    requireBefore(*host, "finalize-routing", "kernel-shutdown");
}

void bridgeFailureLatchesAndLetsOnlyTheDriverFailClosed()
{
    auto host = std::make_shared<FakeDrainHost>();
    auto failFast = std::make_shared<FailFastRecorder>();
    auto drain = createInstalled(host, failFast);
    const auto callbackThread = std::this_thread::get_id();

    host->emitBridgeFailure(Failure{
        FailureKind::IntegrityFailure, false});
    const auto result = drain->wait();

    require(!result, "bridge failure unexpectedly drained successfully");
    require(failFast->count() == 1U,
            "bridge failure did not invoke fail-fast exactly once");
    require(failFast->thread() != callbackThread,
            "bridge callback performed driver-owned fail-fast work");
    require(host->actionThread("listener-hard") == failFast->thread(),
            "hard fanout and fail-fast did not remain driver-owned");
    const auto snapshot = drain->snapshot();
    require(snapshot.lifecycle() == DrainLifecycle::Fatal &&
                snapshot.bridgeFailure() != nullptr,
            "bridge failure was not latched in Fatal state");
    requireBefore(*host, "handler-join-return", "scheduler-shutdown");
    requireBefore(*host, "scheduler-shutdown", "kernel-shutdown");
}

void bridgeFatalWaitCannotReturnWhileExecutorJoinIsBlocked()
{
    auto host = std::make_shared<FakeDrainHost>();
    host->setBlockExecutorJoin(true);
    auto failFast = std::make_shared<FailFastRecorder>();
    auto drain = createInstalled(host, failFast);

    host->emitBridgeFailure(Failure{
        FailureKind::IntegrityFailure, false});
    require(failFast->waitForCount(1U),
            "bridge fatal did not reach fail-fast");
    require(host->waitForAction("handler-join"),
            "bridge fatal did not start executor finalization");

    std::atomic_bool waitReturned{};
    std::atomic_bool waitSucceeded{};
    std::jthread waiter{[&] {
        const auto result = drain->wait();
        waitSucceeded.store(
            static_cast<bool>(result), std::memory_order_release);
        waitReturned.store(true, std::memory_order_release);
    }};
    require(host->actionCount("handler-join-return") == 0U,
            "configured executor join was not blocked");
    require(!waitReturned.load(std::memory_order_acquire),
            "wait returned while the executor finalizer was blocked");

    host->releaseExecutorJoin();
    waiter.join();
    require(waitReturned.load(std::memory_order_acquire) &&
                !waitSucceeded.load(std::memory_order_acquire),
            "bridge fatal wait did not return its retained failure");
    requireBefore(*host, "handler-join-return", "scheduler-shutdown");
}

void schedulerFailureBeforeGracefulCutoffFailsClosedExactly()
{
    auto host = std::make_shared<FakeDrainHost>();
    auto failFast = std::make_shared<FailFastRecorder>();
    auto drain = createInstalled(host, failFast);

    host->emitSchedulerFailure(SchedulerFailure{
        SchedulerFailureKind::DeadlineSinkUnavailable});
    const auto result = drain->wait();

    require(!result,
            "unexpected scheduler stop before cutoff reported success");
    require(failFast->count() == 1U,
            "unexpected scheduler stop did not fail-fast exactly once");
    require(host->actionCount("schedule-deadline") == 0U,
            "pre-cutoff scheduler failure attempted to arm grace");
    const auto snapshot = drain->snapshot();
    require(snapshot.lifecycle() == DrainLifecycle::Fatal &&
                snapshot.deadlineSchedulerFailure() != nullptr &&
                snapshot.deadlineSchedulerFailure()->kind ==
                    SchedulerFailureKind::DeadlineSinkUnavailable,
            "scheduler failure was not retained in the terminal snapshot");
    requireBefore(*host, "handler-join-return", "scheduler-shutdown");
    requireBefore(*host, "scheduler-shutdown", "kernel-shutdown");
}

void partialInstallFailureSynchronizesWithDriverOwnedHardFanout()
{
    auto host = std::make_shared<FakeDrainHost>();
    host->failBindingAt("bind-handler");
    auto failFast = std::make_shared<FailFastRecorder>();
    auto drain = take(Drain::create(RegistrationId, host, failFast));
    const auto installerThread = std::this_thread::get_id();

    const auto installed = drain->install();

    require(!installed, "configured partial installation failure succeeded");
    require(failFast->count() == 1U,
            "partial installation failure returned before fail-fast");
    require(host->actionThread("listener-hard") != installerThread,
            "installer thread dispatched hard teardown work");
    require(host->actionThread("listener-hard") == failFast->thread(),
            "partial installation hard work had multiple owners");
    require(host->actionIndex("bind-bridge") == 0U,
            "partial installation did not bind bridge failure first");
}

void everyPartialInstallFailureCompletesExactTerminalTeardown()
{
    constexpr std::array<std::string_view, 8U> FailurePoints{
        "bind-bridge",
        "bind-scheduler",
        "bind-handler",
        "bind-listener",
        "bind-registry-connections",
        "bind-registry-routing",
        "bind-overload",
        "register-shutdown-deadline"};

    for (const auto failurePoint : FailurePoints) {
        auto host = std::make_shared<FakeDrainHost>();
        host->failBindingAt(std::string{failurePoint});
        auto failFast = std::make_shared<FailFastRecorder>();
        auto drain = take(Drain::create(
            RegistrationId, host, failFast));

        const auto installed = drain->install();

        require(!installed,
                "configured partial installation failure succeeded");
        require(failFast->count() == 1U,
                "partial installation did not fail-fast exactly once");
        require(host->actionCount("handler-join-return") == 1U,
                "partial installation returned before executor join");
        require(host->actionCount("scheduler-shutdown") == 1U &&
                    host->actionCount("kernel-shutdown") == 1U,
                "partial installation returned before exact platform teardown");
        require(!host->hasLiveShutdownTarget(),
                "partial installation retained a live routing target");
        requireBefore(*host, "handler-join-return", "scheduler-shutdown");
        requireBefore(*host, "scheduler-shutdown", "kernel-shutdown");

        drain.reset();
        require(!host->hasLiveShutdownTarget(),
                "destroying a failed drain exposed a poisoned live target");
    }
}

void naturalClaimMakesTheConcurrentDeadlineStale()
{
    auto host = std::make_shared<FakeDrainHost>();
    host->setCancelResult(false);
    host->setFinalizeMode(FakeDrainHost::FinalizeMode::External);
    auto failFast = std::make_shared<FailFastRecorder>();
    auto drain = createInstalled(host, failFast);

    drain->requestGracefulShutdown();
    const bool reachedFinalize = host->waitForAction("finalize-routing");
    const auto request = host->scheduledRequest();
    if (request.has_value()) {
        drain->dispatchDeadline(Deadline{
            request->registrationId,
            ArmSequence,
            request->kind,
            request->deadline});
    }
    host->finalizeExternallyAndPublishProgress();
    const auto result = drain->wait();

    require(reachedFinalize,
            "natural drain did not reach the routing finalization barrier");
    require(request.has_value(),
            "natural drain did not retain its scheduled deadline evidence");
    take(result);
    const auto snapshot = drain->snapshot();
    require(snapshot.staleDeadlineCount() == 1U,
            "deadline after natural claim was not classified stale");
    require(snapshot.hardEscalationCount() == 0U,
            "late deadline overtook an atomic natural teardown claim");
    require(host->actionCount("listener-hard") == 0U,
            "late deadline performed a hard fanout");
}

void deadlineDeliveredBeforeScheduleReturnWinsHard()
{
    auto host = std::make_shared<FakeDrainHost>();
    host->setDispatchDeadlineBeforeScheduleReturns(true);
    auto failFast = std::make_shared<FailFastRecorder>();
    auto drain = createInstalled(host, failFast);

    drain->requestGracefulShutdown();
    take(drain->wait());

    require(drain->snapshot().hardEscalationCount() == 1U,
            "deadline delivered during arming did not win hard escalation");
    require(host->actionCount("listener-hard") == 1U,
            "early deadline did not execute exact hard fanout once");
}

struct TestCase final {
    std::string_view name;
    void (*run)();
};

constexpr std::array TestCases{
    TestCase{"graceful exact teardown", gracefulDrainUsesExactArmAndTeardownOrder},
    TestCase{"blocked executor hard deadline", hardDeadlineEscalatesWhileExecutorJoinIsBlocked},
    TestCase{"routing lost wake", routingRevisionClosesTheFinalizeLostWakeWindow},
    TestCase{"bridge failure latch", bridgeFailureLatchesAndLetsOnlyTheDriverFailClosed},
    TestCase{"blocked bridge fatal join", bridgeFatalWaitCannotReturnWhileExecutorJoinIsBlocked},
    TestCase{"scheduler pre-cutoff fatal", schedulerFailureBeforeGracefulCutoffFailsClosedExactly},
    TestCase{"partial install failure", partialInstallFailureSynchronizesWithDriverOwnedHardFanout},
    TestCase{"every partial install failure", everyPartialInstallFailureCompletesExactTerminalTeardown},
    TestCase{"natural deadline winner", naturalClaimMakesTheConcurrentDeadlineStale},
    TestCase{"early deadline winner", deadlineDeliveredBeforeScheduleReturnWinsHard},
};

} // namespace

int main()
{
    try {
        for (const auto& test : TestCases) {
            test.run();
        }
        std::cout << "DashboardShutdownDrainTests passed: "
                  << TestCases.size() << " cases, " << assertionCount
                  << " assertions\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "DashboardShutdownDrainTests failed after "
                  << assertionCount << " assertions: " << error.what()
                  << '\n';
        return 1;
    }
}
