#include "../Infrastructure/TestSupport.h"

#include "ManagerProcessRestartSignal.h"
#include "ManagerTransitionWorker.h"
#include "ManagerWatchdogPolicy.h"

#include "ForgeConductor/Contracts/IFoundationServices.h"
#include "ForgeConductor/Contracts/IManagerRuntime.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

namespace ForgeConductor::Tests {
namespace {

using namespace std::chrono_literals;
namespace Host = ForgeConductor::Hosts::Manager;

static_assert(std::is_final_v<Host::ManagerTransitionWorker>);
static_assert(std::is_final_v<Host::ManagerWatchdogPolicy>);

[[nodiscard]] Domain::Error injectedFailure(const std::string_view message)
{
    return Domain::makeError(
        Domain::ErrorCodes::InternalFailure, std::string{message}, true);
}

class RealtimeClock final : public Contracts::IClock {
public:
    [[nodiscard]] Domain::UtcTimePoint utcNow() const noexcept override
    {
        return std::chrono::system_clock::now();
    }

    [[nodiscard]] Domain::MonotonicTimePoint monotonicNow() const noexcept override
    {
        return std::chrono::steady_clock::now();
    }
};

class SequentialUuidGenerator final : public Contracts::IUuidGenerator {
public:
    [[nodiscard]] Domain::Result<Domain::Uuid> next() noexcept override
    {
        try {
            const auto sequence = next_.fetch_add(1U, std::memory_order_acq_rel);
            std::ostringstream text;
            text << "00000000-0000-4000-8000-" << std::hex
                 << std::setfill('0') << std::setw(12) << sequence;
            return Domain::Uuid::parse(text.str());
        } catch (...) {
            return Domain::Result<Domain::Uuid>::failure(
                injectedFailure("UUID generation failed."));
        }
    }

private:
    std::atomic_uint64_t next_{1U};
};

[[nodiscard]] Domain::ManagerControllerSnapshot healthySnapshot()
{
    return Domain::ManagerControllerSnapshot{
        Domain::ManagerStatus{
            true,
            true,
            Domain::ManagerServiceState::Running,
            true,
            true,
            true,
            4242U,
            std::chrono::system_clock::now(),
            std::nullopt,
            0U,
            std::nullopt,
            true,
            1s,
            false,
            "127.0.0.1",
            Domain::DefaultManagerDashboardPort,
            8s,
            take(Domain::PathText::create("C:\\ForgeTransitionFixture")),
            "0.9.0-test"},
        false};
}

[[nodiscard]] Domain::ManagerSettings settingsFrom(
    const Domain::ManagerStatus& status)
{
    return Domain::ManagerSettings{
        status.dashboardHost,
        status.dashboardPort,
        status.dashboardRefreshInterval,
        status.autoRestart,
        status.watchdogInterval,
        status.openBrowserOnStart,
        14'400s,
        30s,
        Domain::LogLevel::Info};
}

struct ControllerCall final {
    Domain::ManagerControlAction action{Domain::ManagerControlAction::Start};
    std::string operationId;
    std::string correlationId;
    Domain::MonotonicTimePoint deadline;
    Domain::MonotonicTimePoint enteredAt;
    bool cancellationPossible{};
    bool cancellationRequested{};
};

class RecordingController final : public Contracts::IManagerController {
public:
    explicit RecordingController(Domain::ManagerControllerSnapshot snapshot)
        : snapshot_{std::move(snapshot)}
    {
    }

    [[nodiscard]] Domain::Result<Domain::ManagerStatus> initialize(
        const Domain::OperationContext&) noexcept override
    {
        std::lock_guard lock{mutex_};
        return Domain::Result<Domain::ManagerStatus>::success(snapshot_.status);
    }

    [[nodiscard]] Domain::Result<Domain::ManagerControllerSnapshot> snapshot(
        const Domain::OperationContext& context) noexcept override
    {
        try {
            std::lock_guard lock{mutex_};
            snapshotOperationIds_.push_back(context.operationId.value());
            condition_.notify_all();
            return Domain::Result<Domain::ManagerControllerSnapshot>::success(
                snapshot_);
        } catch (...) {
            return Domain::Result<Domain::ManagerControllerSnapshot>::failure(
                injectedFailure("Snapshot recording failed."));
        }
    }

    [[nodiscard]] Domain::Result<Domain::ManagerStatus> status(
        const Domain::OperationContext&) noexcept override
    {
        std::lock_guard lock{mutex_};
        return Domain::Result<Domain::ManagerStatus>::success(snapshot_.status);
    }

    [[nodiscard]] Domain::Result<Domain::ManagerSettings> settings(
        const Domain::OperationContext&) noexcept override
    {
        std::lock_guard lock{mutex_};
        return Domain::Result<Domain::ManagerSettings>::success(
            settingsFrom(snapshot_.status));
    }

    [[nodiscard]] Domain::Result<Domain::ManagerStatus> control(
        const Domain::ManagerControlRequest& request,
        const Domain::OperationContext& context) noexcept override
    {
        try {
            std::unique_lock lock{mutex_};
            calls_.push_back(ControllerCall{
                request.action,
                context.operationId.value(),
                context.correlationId.value(),
                context.deadline,
                std::chrono::steady_clock::now(),
                context.cancellation.stop_possible(),
                context.cancellation.stop_requested()});
            controlEntered_ = true;
            condition_.notify_all();
            condition_.wait(lock, [this]() noexcept { return !blockControl_; });

            if (nextControlFailure_) {
                auto failure = std::move(*nextControlFailure_);
                nextControlFailure_.reset();
                return Domain::Result<Domain::ManagerStatus>::failure(
                    std::move(failure));
            }

            snapshot_.status.httpListening = true;
            snapshot_.status.serviceActive = snapshot_.status.desiredRunning;
            snapshot_.status.state = snapshot_.status.desiredRunning
                ? Domain::ManagerServiceState::Running
                : Domain::ManagerServiceState::Stopped;
            snapshot_.status.ok = true;
            snapshot_.status.lastError.reset();
            if (request.action == Domain::ManagerControlAction::Restart) {
                ++snapshot_.status.restartCount;
            }
            return Domain::Result<Domain::ManagerStatus>::success(
                snapshot_.status);
        } catch (...) {
            return Domain::Result<Domain::ManagerStatus>::failure(
                injectedFailure("Control recording failed."));
        }
    }

    [[nodiscard]] Domain::Result<Domain::ManagerSettingsUpdateOutcome>
    updateSettings(
        const Domain::ManagerSettingsPatch&,
        const bool,
        const Domain::OperationContext&) noexcept override
    {
        std::lock_guard lock{mutex_};
        return Domain::Result<Domain::ManagerSettingsUpdateOutcome>::success({
            settingsFrom(snapshot_.status),
            true,
            false,
            snapshot_.status});
    }

    [[nodiscard]] Domain::Result<Domain::ManagerControllerSnapshot>
    requestShutdown(const Domain::OperationContext&) noexcept override
    {
        std::lock_guard lock{mutex_};
        snapshot_.shutdownRequested = true;
        snapshot_.status.desiredRunning = false;
        snapshot_.status.state = Domain::ManagerServiceState::Stopping;
        return Domain::Result<Domain::ManagerControllerSnapshot>::success(
            snapshot_);
    }

    void shutdown() noexcept override
    {
        std::lock_guard lock{mutex_};
        blockControl_ = false;
        condition_.notify_all();
    }

    void failNextControl(Domain::Error error)
    {
        std::lock_guard lock{mutex_};
        nextControlFailure_ = std::move(error);
    }

    void blockControl()
    {
        std::lock_guard lock{mutex_};
        blockControl_ = true;
        controlEntered_ = false;
    }

    void releaseControl() noexcept
    {
        std::lock_guard lock{mutex_};
        blockControl_ = false;
        condition_.notify_all();
    }

    [[nodiscard]] bool waitForControlCount(
        const std::size_t count,
        const std::chrono::milliseconds timeout = 2s) const
    {
        std::unique_lock lock{mutex_};
        return condition_.wait_for(
            lock, timeout, [this, count]() noexcept {
                return calls_.size() >= count;
            });
    }

    [[nodiscard]] bool waitUntilControlEntered(
        const std::chrono::milliseconds timeout = 2s) const
    {
        std::unique_lock lock{mutex_};
        return condition_.wait_for(
            lock, timeout,
            [this]() noexcept { return controlEntered_; });
    }

    [[nodiscard]] std::vector<ControllerCall> calls() const
    {
        std::lock_guard lock{mutex_};
        return calls_;
    }

    [[nodiscard]] std::vector<std::string> snapshotOperationIds() const
    {
        std::lock_guard lock{mutex_};
        return snapshotOperationIds_;
    }

private:
    mutable std::mutex mutex_;
    mutable std::condition_variable condition_;
    Domain::ManagerControllerSnapshot snapshot_;
    std::vector<ControllerCall> calls_;
    std::vector<std::string> snapshotOperationIds_;
    std::optional<Domain::Error> nextControlFailure_;
    bool blockControl_{};
    bool controlEntered_{};
};

struct WorkerFixture final {
    WorkerFixture(
        Domain::ManagerControllerSnapshot snapshot = healthySnapshot(),
        const Host::ManagerTransitionWorkerTiming timing = {10ms, 250ms})
        : clock{std::make_shared<RealtimeClock>()},
          uuids{std::make_shared<SequentialUuidGenerator>()},
          controller{std::make_shared<RecordingController>(
              std::move(snapshot))},
          worker{controller, clock, uuids, signal, timing}
    {
    }

    std::shared_ptr<RealtimeClock> clock;
    std::shared_ptr<SequentialUuidGenerator> uuids;
    std::shared_ptr<RecordingController> controller;
    Host::ManagerProcessRestartSignal signal;
    Host::ManagerTransitionWorker worker;
};

[[nodiscard]] bool waitForIdle(
    const Host::ManagerProcessRestartSignal& signal,
    const std::chrono::milliseconds timeout = 2s)
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while ((signal.pending() || signal.inFlight()) &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::yield();
    }
    return !signal.pending() && !signal.inFlight();
}

void watchdogPolicyIsExactAndBounded()
{
    auto snapshot = healthySnapshot();
    require(
        Host::ManagerWatchdogPolicy::decide(snapshot) ==
            Host::ManagerWatchdogAction::None,
        "healthy manager requested watchdog work");

    snapshot.status.httpListening = false;
    snapshot.status.autoRestart = false;
    require(
        Host::ManagerWatchdogPolicy::decide(snapshot) ==
            Host::ManagerWatchdogAction::Repair,
        "missing control listener was not repaired with auto-restart disabled");

    snapshot = healthySnapshot();
    snapshot.status.serviceActive = false;
    snapshot.status.autoRestart = false;
    require(
        Host::ManagerWatchdogPolicy::decide(snapshot) ==
            Host::ManagerWatchdogAction::None,
        "disabled automatic recovery repaired desired-running service drift");
    snapshot.status.autoRestart = true;
    require(
        Host::ManagerWatchdogPolicy::decide(snapshot) ==
            Host::ManagerWatchdogAction::Repair,
        "enabled automatic recovery ignored desired-running service drift");

    snapshot = healthySnapshot();
    snapshot.status.desiredRunning = false;
    snapshot.status.serviceActive = true;
    snapshot.status.autoRestart = false;
    require(
        Host::ManagerWatchdogPolicy::decide(snapshot) ==
            Host::ManagerWatchdogAction::Repair,
        "desired-stopped service drift was not reconciled fail closed");

    snapshot.shutdownRequested = true;
    snapshot.status.httpListening = false;
    require(
        Host::ManagerWatchdogPolicy::decide(snapshot) ==
            Host::ManagerWatchdogAction::None,
        "shutdown intent admitted watchdog recovery");

    snapshot = healthySnapshot();
    snapshot.status.watchdogInterval = 0s;
    require(
        Host::ManagerWatchdogPolicy::normalizedInterval(snapshot) == 1s,
        "watchdog interval was not clamped to its minimum");
    snapshot.status.watchdogInterval = 61s;
    require(
        Host::ManagerWatchdogPolicy::normalizedInterval(snapshot) == 60s,
        "watchdog interval was not clamped to its maximum");
}

void timedRestartSignalPreservesCapacityOneState()
{
    Host::ManagerProcessRestartSignal signal;
    const auto started = std::chrono::steady_clock::now();
    require(
        signal.waitAndBeginUntil({}, started + 20ms) ==
            Host::ManagerProcessRestartWaitResult::WatchdogDue,
        "timed restart wait did not distinguish watchdog expiry");
    require(std::chrono::steady_clock::now() - started >= 10ms,
            "timed restart wait returned before its deadline");
    require(!signal.pending() && !signal.inFlight() && !signal.closed(),
            "watchdog expiry mutated restart signal state");

    require(
        signal.requestRestart() ==
            Host::ManagerProcessRestartRequestResult::Published,
        "timed restart fixture could not publish work");
    require(
        signal.waitAndBeginUntil(
            {}, std::chrono::steady_clock::now() + 1s) ==
            Host::ManagerProcessRestartWaitResult::RestartRequested,
        "timed wait did not claim a pending restart");
    require(signal.completeRestart(),
            "timed wait did not retain in-flight completion ownership");

    std::stop_source cancelled;
    static_cast<void>(cancelled.request_stop());
    require(
        signal.waitAndBeginUntil(
            cancelled.get_token(), std::chrono::steady_clock::now() + 1s) ==
            Host::ManagerProcessRestartWaitResult::Cancelled,
        "timed wait did not distinguish cancellation");
    signal.close();
    require(
        signal.waitAndBeginUntil(
            {}, std::chrono::steady_clock::now() + 1s) ==
            Host::ManagerProcessRestartWaitResult::Closed,
        "timed wait did not distinguish permanent close");
}

void explicitRestartCompletesAfterTypedFailureAndUsesFreshContexts()
{
    WorkerFixture fixture;
    fixture.controller->failNextControl(
        injectedFailure("injected restart failure"));
    require(
        fixture.signal.requestRestart() ==
            Host::ManagerProcessRestartRequestResult::Published,
        "explicit restart was not published before worker start");
    static_cast<void>(take(fixture.worker.start()));
    require(fixture.controller->waitForControlCount(1U),
            "explicit restart did not reach the controller");
    require(waitForIdle(fixture.signal),
            "typed restart failure retained in-flight signal ownership");

    require(
        fixture.signal.requestRestart() ==
            Host::ManagerProcessRestartRequestResult::Published,
        "signal was not reusable after typed restart failure");
    require(fixture.controller->waitForControlCount(2U),
            "second explicit restart did not reach the controller");
    require(waitForIdle(fixture.signal),
            "successful restart retained in-flight signal ownership");
    fixture.worker.shutdown();

    const auto calls = fixture.controller->calls();
    require(calls.size() == 2U &&
                calls[0].action == Domain::ManagerControlAction::Restart &&
                calls[1].action == Domain::ManagerControlAction::Restart,
            "explicit requests were not serialized as restart actions");
    require(calls[0].operationId != calls[1].operationId &&
                calls[0].correlationId != calls[1].correlationId,
            "explicit restarts reused operation or correlation identity");
    for (const auto& call : calls) {
        require(call.cancellationPossible && !call.cancellationRequested,
                "worker context did not carry live cancellation ownership");
        require(call.deadline > call.enteredAt &&
                    call.deadline - call.enteredAt <= 250ms,
                "worker context did not carry its bounded deadline");
    }
    const auto observations = fixture.controller->snapshotOperationIds();
    require(!observations.empty() &&
                observations.front() != calls.front().operationId,
            "initial observation and restart reused an operation identity");
}

void watchdogRepairsListenerOnceWithoutRestartCounting()
{
    auto missing = healthySnapshot();
    missing.status.httpListening = false;
    missing.status.serviceActive = false;
    missing.status.state = Domain::ManagerServiceState::Failed;
    missing.status.autoRestart = false;
    WorkerFixture fixture{std::move(missing)};

    static_cast<void>(take(fixture.worker.start()));
    require(fixture.controller->waitForControlCount(1U),
            "watchdog did not repair a missing listener");
    require(!fixture.controller->waitForControlCount(2U, 80ms),
            "healthy successor triggered repeated watchdog repair");
    fixture.worker.shutdown();

    const auto calls = fixture.controller->calls();
    require(calls.size() == 1U &&
                calls.front().action == Domain::ManagerControlAction::Repair,
            "watchdog used a counted explicit restart instead of repair");
}

void disabledRecoveryAndShutdownSuppressWatchdogMutation()
{
    auto disabled = healthySnapshot();
    disabled.status.serviceActive = false;
    disabled.status.state = Domain::ManagerServiceState::Failed;
    disabled.status.autoRestart = false;
    WorkerFixture disabledFixture{std::move(disabled)};
    static_cast<void>(take(disabledFixture.worker.start()));
    require(!disabledFixture.controller->waitForControlCount(1U, 80ms),
            "disabled automatic recovery mutated healthy control ownership");
    disabledFixture.worker.shutdown();

    auto shuttingDown = healthySnapshot();
    shuttingDown.shutdownRequested = true;
    shuttingDown.status.httpListening = false;
    shuttingDown.status.serviceActive = false;
    WorkerFixture shutdownFixture{std::move(shuttingDown)};
    static_cast<void>(take(shutdownFixture.worker.start()));
    require(!shutdownFixture.controller->waitForControlCount(1U, 80ms),
            "shutdown intent admitted watchdog mutation");
    shutdownFixture.worker.shutdown();
}

void capacityOneWorkerNeverOverlapsAndShutdownJoins()
{
    auto missing = healthySnapshot();
    missing.status.httpListening = false;
    missing.status.serviceActive = false;
    WorkerFixture fixture{std::move(missing)};
    fixture.controller->blockControl();
    require(
        fixture.signal.requestRestart() ==
            Host::ManagerProcessRestartRequestResult::Published,
        "blocked fixture could not publish its restart");
    static_cast<void>(take(fixture.worker.start()));
    require(fixture.controller->waitUntilControlEntered(),
            "blocked restart did not enter the controller");

    for (std::size_t index{}; index < 32U; ++index) {
        require(
            fixture.signal.requestRestart() ==
                Host::ManagerProcessRestartRequestResult::Coalesced,
            "in-flight restart accepted an overlapping successor");
    }
    require(!fixture.controller->waitForControlCount(2U, 80ms),
            "watchdog or restart work overlapped the active controller call");

    std::atomic_bool shutdownReturned{};
    std::jthread shutdownThread{[&]() {
        fixture.worker.shutdown();
        shutdownReturned.store(true, std::memory_order_release);
    }};
    const auto retentionDeadline = std::chrono::steady_clock::now() + 80ms;
    while (std::chrono::steady_clock::now() < retentionDeadline &&
           !shutdownReturned.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }
    require(!shutdownReturned.load(std::memory_order_acquire),
            "worker shutdown returned while controller work could still execute");

    fixture.controller->releaseControl();
    shutdownThread.join();
    fixture.worker.shutdown();
    require(shutdownReturned.load(std::memory_order_acquire) &&
                fixture.signal.closed() && !fixture.signal.inFlight(),
            "worker shutdown did not join and close exact transition ownership");
}

void beginShutdownSignalsWithoutJoining()
{
    WorkerFixture fixture;
    fixture.controller->blockControl();
    require(
        fixture.signal.requestRestart() ==
            Host::ManagerProcessRestartRequestResult::Published,
        "nonblocking-stop fixture could not publish its restart");
    static_cast<void>(take(fixture.worker.start()));
    require(fixture.controller->waitUntilControlEntered(),
            "nonblocking-stop restart did not enter the controller");

    const auto started = std::chrono::steady_clock::now();
    fixture.worker.beginShutdown();
    const auto elapsed = std::chrono::steady_clock::now() - started;
    require(elapsed < 250ms,
            "transition begin-shutdown waited for controller quiescence");
    require(fixture.signal.closed(),
            "transition begin-shutdown did not close successor admission");

    std::atomic_bool exactShutdownReturned{};
    std::jthread shutdownThread{[&]() {
        fixture.worker.shutdown();
        exactShutdownReturned.store(true, std::memory_order_release);
    }};
    const auto retentionDeadline = std::chrono::steady_clock::now() + 80ms;
    while (std::chrono::steady_clock::now() < retentionDeadline &&
           !exactShutdownReturned.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }
    require(!exactShutdownReturned.load(std::memory_order_acquire),
            "exact transition shutdown did not retain worker ownership");

    fixture.controller->releaseControl();
    shutdownThread.join();
    require(exactShutdownReturned.load(std::memory_order_acquire),
            "exact transition shutdown did not finish after quiescence");
}

void lifecycleAndTimingAdmissionAreBounded()
{
    Host::ManagerProcessRestartSignal signal;
    auto controller = std::make_shared<RecordingController>(healthySnapshot());
    auto clock = std::make_shared<RealtimeClock>();
    auto uuids = std::make_shared<SequentialUuidGenerator>();

    const auto requireInvalidTiming = [&](const auto timing) {
        bool rejected{};
        try {
            Host::ManagerTransitionWorker worker{
                controller, clock, uuids, signal, timing};
        } catch (const std::invalid_argument&) {
            rejected = true;
        }
        require(rejected, "transition worker accepted unbounded timing");
    };
    requireInvalidTiming(Host::ManagerTransitionWorkerTiming{0ms, 1s});
    requireInvalidTiming(Host::ManagerTransitionWorkerTiming{1ms, 0ms});
    requireInvalidTiming(Host::ManagerTransitionWorkerTiming{1001ms, 1s});
    requireInvalidTiming(
        Host::ManagerTransitionWorkerTiming{1ms, 60s + 1ms});

    Host::ManagerProcessRestartSignal lifecycleSignal;
    Host::ManagerTransitionWorker worker{
        controller, clock, uuids, lifecycleSignal, {10ms, 250ms}};
    static_cast<void>(take(worker.start()));
    requireError(
        worker.start(),
        Domain::ErrorCodes::Conflict,
        "transition worker started twice");
    worker.shutdown();
    worker.shutdown();
    requireError(
        worker.start(),
        Domain::ErrorCodes::TransportClosed,
        "transition worker restarted after terminal shutdown");

    Host::ManagerProcessRestartSignal inertSignal;
    Host::ManagerTransitionWorker inertWorker{
        controller, clock, uuids, inertSignal, {10ms, 250ms}};
    inertWorker.beginShutdown();
    requireError(
        inertWorker.start(),
        Domain::ErrorCodes::TransportClosed,
        "transition worker started after a pre-start stop signal");
    inertWorker.shutdown();
}

} // namespace
} // namespace ForgeConductor::Tests

int main()
{
    using namespace ForgeConductor::Tests;
    try {
        watchdogPolicyIsExactAndBounded();
        std::cout << "PASS manager_transition.policy\n";
        timedRestartSignalPreservesCapacityOneState();
        std::cout << "PASS manager_transition.timed_signal\n";
        explicitRestartCompletesAfterTypedFailureAndUsesFreshContexts();
        std::cout << "PASS manager_transition.explicit_restart\n";
        watchdogRepairsListenerOnceWithoutRestartCounting();
        std::cout << "PASS manager_transition.watchdog_repair\n";
        disabledRecoveryAndShutdownSuppressWatchdogMutation();
        std::cout << "PASS manager_transition.suppression\n";
        capacityOneWorkerNeverOverlapsAndShutdownJoins();
        std::cout << "PASS manager_transition.ownership\n";
        beginShutdownSignalsWithoutJoining();
        std::cout << "PASS manager_transition.nonblocking_stop\n";
        lifecycleAndTimingAdmissionAreBounded();
        std::cout << "PASS manager_transition.lifecycle\n";
        std::cout << "SUMMARY passed=8 failed=0\n";
        return EXIT_SUCCESS;
    } catch (const std::exception& error) {
        std::cerr << "FAIL " << error.what() << '\n';
        return EXIT_FAILURE;
    } catch (...) {
        std::cerr << "FAIL unknown exception\n";
        return EXIT_FAILURE;
    }
}
