#include "ManagerControllerClient.h"
#include "ManagerProcessRestartSignal.h"
#include "ManagerProcessStopSignal.h"

#include "Infrastructure/Windows/Detail/ManagerDashboardOperationalState.h"

#include "ForgeConductor/Domain/Error.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <utility>

namespace {

namespace Contracts = ForgeConductor::Contracts;
namespace Domain = ForgeConductor::Domain;
namespace Host = ForgeConductor::Hosts::Manager;
namespace WindowsDetail =
    ForgeConductor::Infrastructure::Windows::Detail;

using namespace std::chrono_literals;

static_assert(std::is_final_v<Host::ManagerProcessStopSignal>);
static_assert(std::is_final_v<Host::ManagerProcessRestartSignal>);
static_assert(std::is_final_v<Host::ManagerControllerClient>);
static_assert(std::is_final_v<
              WindowsDetail::ManagerDashboardOperationalState>);
static_assert(std::is_base_of_v<
              Contracts::IManagerClient,
              Host::ManagerControllerClient>);
static_assert(std::is_base_of_v<
              WindowsDetail::IDashboardOperationalStateSource,
              WindowsDetail::ManagerDashboardOperationalState>);

class TestFailure final : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

void require(const bool condition, const std::string& message)
{
    if (!condition) {
        throw TestFailure{message};
    }
}

template <typename T>
void requireError(
    const Domain::Result<T>& result,
    const std::string_view code,
    const std::string& message)
{
    require(!result, message);
    require(result.error().code == code, message + " (error code)");
}

[[nodiscard]] Domain::OperationContext context()
{
    auto operation = Domain::OperationId::parse(
        "11111111-1111-4111-8111-111111111111");
    auto correlation = Domain::CorrelationId::parse(
        "manager-composition-support");
    if (!operation || !correlation) {
        throw TestFailure{"could not construct the test operation context"};
    }
    return Domain::OperationContext{
        std::move(operation).value(),
        std::chrono::steady_clock::now() + 5min,
        {},
        std::move(correlation).value()};
}

[[nodiscard]] Domain::ManagerStatus statusValue(
    const std::uint32_t restartCount)
{
    auto home = Domain::PathText::create("C:\\ManagerCompositionSupportTests");
    if (!home) {
        throw TestFailure{"could not construct the test manager home"};
    }
    return Domain::ManagerStatus{
        true,
        true,
        Domain::ManagerServiceState::Running,
        true,
        true,
        true,
        42U,
        std::nullopt,
        std::nullopt,
        restartCount,
        std::nullopt,
        true,
        3s,
        false,
        "127.0.0.1",
        7788U,
        8s,
        std::move(home).value(),
        "test"};
}

struct StatusGate final {
    std::mutex mutex;
    std::condition_variable condition;
    bool enabled{};
    bool entered{};
    bool released{};
    std::atomic_bool controllerDestroyed{};
    std::atomic_size_t controllerShutdownCalls{};
};

class RecordingController final : public Contracts::IManagerController {
public:
    explicit RecordingController(std::shared_ptr<StatusGate> gate = {})
        : gate_{std::move(gate)},
          status_{statusValue(17U)},
          controlStatus_{statusValue(18U)},
          settings_{},
          updateOutcome_{settings_, true, true, controlStatus_}
    {
        settings_.dashboardPort = 7799U;
        updateOutcome_.settings = settings_;
    }

    ~RecordingController() noexcept override
    {
        if (gate_) {
            gate_->controllerDestroyed.store(true, std::memory_order_release);
        }
    }

    void failStatus(Domain::Error error)
    {
        const std::lock_guard lock{mutex_};
        statusFailure_ = std::move(error);
    }

    [[nodiscard]] std::size_t statusCalls() const noexcept
    {
        return statusCalls_.load(std::memory_order_acquire);
    }

    [[nodiscard]] std::size_t settingsCalls() const noexcept
    {
        return settingsCalls_.load(std::memory_order_acquire);
    }

    [[nodiscard]] std::size_t controlCalls() const noexcept
    {
        return controlCalls_.load(std::memory_order_acquire);
    }

    [[nodiscard]] std::size_t updateCalls() const noexcept
    {
        return updateCalls_.load(std::memory_order_acquire);
    }

    [[nodiscard]] std::size_t requestShutdownCalls() const noexcept
    {
        return requestShutdownCalls_.load(std::memory_order_acquire);
    }

    [[nodiscard]] std::size_t shutdownCalls() const noexcept
    {
        return shutdownCalls_.load(std::memory_order_acquire);
    }

    [[nodiscard]] Domain::ManagerControlAction lastControlAction() const
    {
        const std::lock_guard lock{mutex_};
        return lastControlAction_;
    }

    [[nodiscard]] bool lastApplyImmediately() const
    {
        const std::lock_guard lock{mutex_};
        return lastApplyImmediately_;
    }

    [[nodiscard]] std::optional<std::uint16_t> lastDashboardPort() const
    {
        const std::lock_guard lock{mutex_};
        return lastPatch_.dashboardPort;
    }

    [[nodiscard]] std::string lastOperationId() const
    {
        const std::lock_guard lock{mutex_};
        return lastOperationId_;
    }

    [[nodiscard]] Domain::Result<Domain::ManagerStatus> initialize(
        const Domain::OperationContext& value) noexcept override
    {
        recordContext(value);
        return Domain::Result<Domain::ManagerStatus>::success(status_);
    }

    [[nodiscard]] Domain::Result<Domain::ManagerControllerSnapshot> snapshot(
        const Domain::OperationContext& value) noexcept override
    {
        recordContext(value);
        return Domain::Result<Domain::ManagerControllerSnapshot>::success(
            Domain::ManagerControllerSnapshot{status_, false});
    }

    [[nodiscard]] Domain::Result<Domain::ManagerStatus> status(
        const Domain::OperationContext& value) noexcept override
    {
        recordContext(value);
        statusCalls_.fetch_add(1U, std::memory_order_acq_rel);
        if (gate_) {
            std::unique_lock lock{gate_->mutex};
            if (gate_->enabled) {
                gate_->entered = true;
                gate_->condition.notify_all();
                gate_->condition.wait(
                    lock, [this]() noexcept { return gate_->released; });
            }
        }

        const std::lock_guard lock{mutex_};
        if (statusFailure_) {
            return Domain::Result<Domain::ManagerStatus>::failure(
                *statusFailure_);
        }
        return Domain::Result<Domain::ManagerStatus>::success(status_);
    }

    [[nodiscard]] Domain::Result<Domain::ManagerSettings> settings(
        const Domain::OperationContext& value) noexcept override
    {
        recordContext(value);
        settingsCalls_.fetch_add(1U, std::memory_order_acq_rel);
        return Domain::Result<Domain::ManagerSettings>::success(settings_);
    }

    [[nodiscard]] Domain::Result<Domain::ManagerStatus> control(
        const Domain::ManagerControlRequest& request,
        const Domain::OperationContext& value) noexcept override
    {
        recordContext(value);
        controlCalls_.fetch_add(1U, std::memory_order_acq_rel);
        {
            const std::lock_guard lock{mutex_};
            lastControlAction_ = request.action;
        }
        return Domain::Result<Domain::ManagerStatus>::success(controlStatus_);
    }

    [[nodiscard]] Domain::Result<Domain::ManagerSettingsUpdateOutcome>
    updateSettings(
        const Domain::ManagerSettingsPatch& patch,
        const bool applyImmediately,
        const Domain::OperationContext& value) noexcept override
    {
        recordContext(value);
        updateCalls_.fetch_add(1U, std::memory_order_acq_rel);
        {
            const std::lock_guard lock{mutex_};
            lastPatch_ = patch;
            lastApplyImmediately_ = applyImmediately;
        }
        return Domain::Result<Domain::ManagerSettingsUpdateOutcome>::success(
            updateOutcome_);
    }

    [[nodiscard]] Domain::Result<Domain::ManagerControllerSnapshot>
    requestShutdown(
        const Domain::OperationContext& value) noexcept override
    {
        recordContext(value);
        requestShutdownCalls_.fetch_add(1U, std::memory_order_acq_rel);
        return Domain::Result<Domain::ManagerControllerSnapshot>::success(
            Domain::ManagerControllerSnapshot{status_, true});
    }

    void shutdown() noexcept override
    {
        shutdownCalls_.fetch_add(1U, std::memory_order_acq_rel);
        if (gate_) {
            gate_->controllerShutdownCalls.fetch_add(
                1U, std::memory_order_acq_rel);
        }
    }

private:
    void recordContext(const Domain::OperationContext& value) noexcept
    {
        try {
            const std::lock_guard lock{mutex_};
            lastOperationId_ = value.operationId.value();
        } catch (...) {
        }
    }

    const std::shared_ptr<StatusGate> gate_;
    const Domain::ManagerStatus status_;
    const Domain::ManagerStatus controlStatus_;
    Domain::ManagerSettings settings_;
    Domain::ManagerSettingsUpdateOutcome updateOutcome_;

    mutable std::mutex mutex_;
    std::optional<Domain::Error> statusFailure_;
    Domain::ManagerSettingsPatch lastPatch_;
    Domain::ManagerControlAction lastControlAction_{
        Domain::ManagerControlAction::Start};
    bool lastApplyImmediately_{};
    std::string lastOperationId_;

    std::atomic_size_t statusCalls_{};
    std::atomic_size_t settingsCalls_{};
    std::atomic_size_t controlCalls_{};
    std::atomic_size_t updateCalls_{};
    std::atomic_size_t requestShutdownCalls_{};
    std::atomic_size_t shutdownCalls_{};
};

void processStopSignalPublishesOneEdgeAndSupportsWatcherTeardown()
{
    Host::ManagerProcessStopSignal signal;
    require(!signal.requested(), "new process stop signal was requested");
    require(!signal.token().stop_requested(), "new process token was stopped");

    std::optional<bool> observed;
    std::jthread watcher{[&signal, &observed](const std::stop_token stop) {
        observed = signal.wait(stop);
    }};

    require(signal.requestStop(), "first process stop request was not published");
    require(!signal.requestStop(), "process stop edge was published twice");
    watcher.join();

    require(observed == true, "watcher did not observe the process stop edge");
    require(signal.requested(), "process stop state was not retained");
    require(signal.token().stop_requested(), "process stop token was not stopped");

    Host::ManagerProcessStopSignal cancellationOnly;
    std::optional<bool> cancellationObserved;
    std::jthread cancelledWatcher{
        [&cancellationOnly, &cancellationObserved](const std::stop_token stop) {
            cancellationObserved = cancellationOnly.wait(stop);
        }};
    cancelledWatcher.request_stop();
    cancelledWatcher.join();

    require(
        cancellationObserved == false,
        "watcher teardown was reported as a process stop request");
    require(
        !cancellationOnly.requested(),
        "watcher teardown changed process stop state");
    require(
        !cancellationOnly.token().stop_requested(),
        "watcher teardown stopped the process token");
}

void controllerClientBindsOnceForwardsExactlyAndClosesOnlyItself()
{
    Host::ManagerProcessRestartSignal restartSignal;
    Host::ManagerControllerClient client{restartSignal};
    const auto operation = context();

    requireError(
        client.status(operation),
        Domain::ErrorCodes::TransportClosed,
        "unbound local controller client status");
    requireError(
        client.bind({}),
        Domain::ErrorCodes::Conflict,
        "null controller bind");

    auto controller = std::make_shared<RecordingController>();
    require(client.bind(controller).hasValue(), "valid controller bind failed");
    requireError(
        client.bind(controller),
        Domain::ErrorCodes::Conflict,
        "duplicate controller bind");

    const auto status = client.status(operation);
    require(status && status.value().restartCount == 17U, "status forwarding");

    const auto settings = client.settings(operation);
    require(
        settings && settings.value().dashboardPort == 7799U,
        "settings forwarding");

    const auto controlled = client.control(
        Domain::ManagerControlRequest{Domain::ManagerControlAction::Restart},
        operation);
    require(
        controlled && controlled.value().restartCount == 18U,
        "control result forwarding");
    require(
        controller->lastControlAction() ==
            Domain::ManagerControlAction::Restart,
        "control request forwarding");

    Domain::ManagerSettingsPatch patch;
    patch.dashboardPort = std::uint16_t{8899U};
    const auto updated = client.updateSettings(patch, true, operation);
    require(
        updated && updated.value().applied && updated.value().bindingChanged,
        "settings update result forwarding");
    require(
        controller->lastDashboardPort() == 8899U &&
            controller->lastApplyImmediately(),
        "settings update argument forwarding");

    require(
        client.requestRestart(operation).hasValue(),
        "restart request publication");
    require(restartSignal.pending(), "restart request was not pending");
    require(
        client.requestRestart(operation).hasValue(),
        "restart request coalescing");
    require(
        controller->controlCalls() == 1U,
        "restart request called the controller inline");
    require(
        restartSignal.waitAndBegin({}) ==
            Host::ManagerProcessRestartWaitResult::RestartRequested,
        "restart worker could not claim the request");
    require(restartSignal.completeRestart(),
            "restart worker could not complete the request");

    require(
        client.requestShutdown(operation).hasValue(),
        "shutdown request forwarding");
    require(
        controller->requestShutdownCalls() == 1U,
        "shutdown request call count");

    const auto forwardedFailure = Domain::makeError(
        Domain::ErrorCodes::DeadlineExceeded,
        "forwarded controller failure",
        true,
        "manager-composition-support-evidence");
    controller->failStatus(forwardedFailure);
    const auto failed = client.status(operation);
    require(!failed, "controller failure was converted to success");
    require(
        failed.error() == forwardedFailure,
        "controller failure was not forwarded exactly");

    require(controller->statusCalls() == 2U, "status call count");
    require(controller->settingsCalls() == 1U, "settings call count");
    require(controller->controlCalls() == 1U, "control call count");
    require(controller->updateCalls() == 1U, "update call count");
    require(
        controller->lastOperationId() == operation.operationId.value(),
        "operation context forwarding");

    client.shutdown();
    client.shutdown();
    require(
        controller->shutdownCalls() == 0U,
        "adapter shutdown closed the controller");
    requireError(
        client.status(operation),
        Domain::ErrorCodes::TransportClosed,
        "closed local controller client status");
    requireError(
        client.requestRestart(operation),
        Domain::ErrorCodes::TransportClosed,
        "closed local controller client restart");
    requireError(
        client.bind(controller),
        Domain::ErrorCodes::TransportClosed,
        "bind after local controller client shutdown");
}

void controllerClientReportsExpiredControllerAsTransportClosed()
{
    Host::ManagerProcessRestartSignal restartSignal;
    Host::ManagerControllerClient client{restartSignal};
    auto controller = std::make_shared<RecordingController>();
    require(client.bind(controller).hasValue(), "expiry controller bind failed");
    controller.reset();

    requireError(
        client.status(context()),
        Domain::ErrorCodes::TransportClosed,
        "expired local controller");
}

void pinnedControllerCallSurvivesConcurrentAdapterShutdown()
{
    auto gate = std::make_shared<StatusGate>();
    gate->enabled = true;
    auto controller = std::make_shared<RecordingController>(gate);
    Host::ManagerProcessRestartSignal restartSignal;
    Host::ManagerControllerClient client{restartSignal};
    require(client.bind(controller).hasValue(), "pinned controller bind failed");

    std::optional<Domain::Result<Domain::ManagerStatus>> result;
    std::jthread caller{[&client, &result] { result.emplace(client.status(context())); }};

    {
        std::unique_lock lock{gate->mutex};
        require(
            gate->condition.wait_for(
                lock, 2s, [&gate]() noexcept { return gate->entered; }),
            "controller call did not reach its pinning gate");
    }

    controller.reset();
    client.shutdown();
    client.shutdown();
    const bool destroyedBeforeRelease =
        gate->controllerDestroyed.load(std::memory_order_acquire);
    const auto shutdownCallsBeforeRelease =
        gate->controllerShutdownCalls.load(std::memory_order_acquire);

    {
        const std::lock_guard lock{gate->mutex};
        gate->released = true;
    }
    gate->condition.notify_all();
    caller.join();

    require(
        !destroyedBeforeRelease,
        "adapter shutdown invalidated an in-flight pinned controller");
    require(
        shutdownCallsBeforeRelease == 0U,
        "adapter shutdown propagated to the pinned controller");
    require(result.has_value() && result->hasValue(), "pinned call failed");
    require(
        gate->controllerDestroyed.load(std::memory_order_acquire),
        "pinned controller lifetime was retained after call completion");
}

void operationalStatePublishesAtomicSnapshots()
{
    WindowsDetail::ManagerDashboardOperationalState state;
    require(!state.operationalServiceActive(), "default operational state");

    state.setOperationalServiceActive(true);
    require(state.operationalServiceActive(), "active operational state");

    std::atomic_bool start{};
    std::atomic_bool stop{};
    std::jthread reader{[&state, &start, &stop] {
        while (!start.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
        while (!stop.load(std::memory_order_acquire)) {
            static_cast<void>(state.operationalServiceActive());
        }
    }};

    start.store(true, std::memory_order_release);
    for (std::size_t index = 0U; index < 10'000U; ++index) {
        state.setOperationalServiceActive((index % 2U) == 0U);
    }
    state.setOperationalServiceActive(false);
    stop.store(true, std::memory_order_release);
    reader.join();

    require(!state.operationalServiceActive(), "final operational state");
}

} // namespace

int main()
{
    try {
        processStopSignalPublishesOneEdgeAndSupportsWatcherTeardown();
        controllerClientBindsOnceForwardsExactlyAndClosesOnlyItself();
        controllerClientReportsExpiredControllerAsTransportClosed();
        pinnedControllerCallSurvivesConcurrentAdapterShutdown();
        operationalStatePublishesAtomicSnapshots();
        std::cout << "Manager composition support tests passed.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Manager composition support tests failed: "
                  << error.what() << '\n';
        return 1;
    } catch (...) {
        std::cerr << "Manager composition support tests failed with an unknown "
                     "error.\n";
        return 1;
    }
}
