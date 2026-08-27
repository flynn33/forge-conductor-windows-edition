#include "ManagerProcessHost.h"

#include "ForgeConductor/Contracts/IFoundationServices.h"
#include "ForgeConductor/Domain/Error.h"
#include "ForgeConductor/Manager/ManagerRequestDispatcher.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace {

namespace Contracts = ForgeConductor::Contracts;
namespace Domain = ForgeConductor::Domain;
namespace Host = ForgeConductor::Hosts::Manager;
namespace Manager = ForgeConductor::Manager;

using namespace std::chrono_literals;

class EventLog final {
public:
    void append(std::string event)
    {
        const std::lock_guard lock{mutex_};
        events_.push_back(std::move(event));
    }

    [[nodiscard]] std::vector<std::string> snapshot() const
    {
        const std::lock_guard lock{mutex_};
        return events_;
    }

private:
    mutable std::mutex mutex_;
    std::vector<std::string> events_;
};

class FakeClock final : public Contracts::IClock {
public:
    [[nodiscard]] Domain::UtcTimePoint utcNow() const noexcept override
    {
        return Domain::UtcTimePoint{};
    }

    [[nodiscard]] Domain::MonotonicTimePoint monotonicNow() const noexcept override
    {
        return Domain::MonotonicTimePoint{};
    }
};

class RecordingController final : public Contracts::IManagerController {
public:
    explicit RecordingController(std::shared_ptr<EventLog> events)
        : events_{std::move(events)}
    {
    }

    void failInitialization(Domain::Error error)
    {
        initializationFailure_ = std::move(error);
    }

    void blockInitialization()
    {
        const std::lock_guard lock{initializationMutex_};
        blockInitialization_ = true;
    }

    [[nodiscard]] bool waitUntilInitialization(
        const std::chrono::milliseconds timeout)
    {
        std::unique_lock lock{initializationMutex_};
        return initializationChanged_.wait_for(
            lock, timeout, [this] { return initializationEntered_; });
    }

    [[nodiscard]] Domain::Result<Domain::ManagerStatus> initialize(
        const Domain::OperationContext& context) noexcept override
    {
        events_->append("controller.initialize");
        const std::stop_callback cancellation{
            context.cancellation,
            [this]() noexcept { initializationChanged_.notify_all(); }};
        {
            std::unique_lock lock{initializationMutex_};
            ++initializeCalls_;
            initializationEntered_ = true;
            initializationChanged_.notify_all();
            if (blockInitialization_) {
                initializationChanged_.wait(lock, [&context] {
                    return context.isCancellationRequested();
                });
            }
        }
        if (context.isCancellationRequested()) {
            return Domain::Result<Domain::ManagerStatus>::failure(
                Domain::makeError(
                    Domain::ErrorCodes::Cancelled,
                    "The recording initialization was cancelled."));
        }
        if (initializationFailure_) {
            return Domain::Result<Domain::ManagerStatus>::failure(
                *initializationFailure_);
        }
        return Domain::Result<Domain::ManagerStatus>::success(
            Domain::ManagerStatus{
                true,
                true,
                Domain::ManagerServiceState::Running,
                true,
                true,
                true,
                42U,
                std::nullopt,
                std::nullopt,
                0U,
                std::nullopt,
                true,
                3s,
                false,
                "127.0.0.1",
                7788U,
                8s,
                Domain::PathText::create("C:\\ManagerProcessHostTest").value(),
                "test"});
    }

    [[nodiscard]] Domain::Result<Domain::ManagerControllerSnapshot> snapshot(
        const Domain::OperationContext&) noexcept override
    {
        return unavailable<Domain::ManagerControllerSnapshot>();
    }

    [[nodiscard]] Domain::Result<Domain::ManagerStatus> status(
        const Domain::OperationContext&) noexcept override
    {
        return unavailable<Domain::ManagerStatus>();
    }

    [[nodiscard]] Domain::Result<Domain::ManagerSettings> settings(
        const Domain::OperationContext&) noexcept override
    {
        return unavailable<Domain::ManagerSettings>();
    }

    [[nodiscard]] Domain::Result<Domain::ManagerStatus> control(
        const Domain::ManagerControlRequest&,
        const Domain::OperationContext&) noexcept override
    {
        return unavailable<Domain::ManagerStatus>();
    }

    [[nodiscard]] Domain::Result<Domain::ManagerSettings> updateSettings(
        const Domain::ManagerSettingsPatch&,
        bool,
        const Domain::OperationContext&) noexcept override
    {
        return unavailable<Domain::ManagerSettings>();
    }

    [[nodiscard]] Domain::Result<Domain::ManagerControllerSnapshot>
    requestShutdown(const Domain::OperationContext&) noexcept override
    {
        return unavailable<Domain::ManagerControllerSnapshot>();
    }

    void shutdown() noexcept override
    {
        if (!shutdown_) {
            shutdown_ = true;
            ++shutdownCalls_;
            events_->append("controller.shutdown");
        }
    }

    [[nodiscard]] std::size_t initializeCalls() const noexcept
    {
        const std::lock_guard lock{initializationMutex_};
        return initializeCalls_;
    }

    [[nodiscard]] std::size_t shutdownCalls() const noexcept
    {
        return shutdownCalls_;
    }

private:
    template <typename T>
    [[nodiscard]] static Domain::Result<T> unavailable()
    {
        return Domain::Result<T>::failure(Domain::makeError(
            Domain::ErrorCodes::InternalFailure,
            "The recording controller method is not used by this test."));
    }

    std::shared_ptr<EventLog> events_;
    std::optional<Domain::Error> initializationFailure_;
    mutable std::mutex initializationMutex_;
    std::condition_variable initializationChanged_;
    std::size_t initializeCalls_{};
    std::size_t shutdownCalls_{};
    bool blockInitialization_{};
    bool initializationEntered_{};
    bool shutdown_{};
};

enum class ServerBehavior {
    BlockUntilShutdown,
    ObservePreclosed,
    ReturnSuccess,
    ReturnFailure
};

class RecordingServer final : public Contracts::IManagerServer {
public:
    RecordingServer(
        std::shared_ptr<EventLog> events,
        const ServerBehavior behavior)
        : events_{std::move(events)}, behavior_{behavior}
    {
    }

    [[nodiscard]] Domain::Result<void> run(
        const Domain::OperationContext&) noexcept override
    {
        {
            const std::lock_guard lock{mutex_};
            ++runCalls_;
            runEntered_ = true;
        }
        events_->append("server.run");
        changed_.notify_all();

        if (behavior_ == ServerBehavior::BlockUntilShutdown) {
            std::unique_lock lock{mutex_};
            changed_.wait(lock, [this] { return shutdown_; });
        }
        if (behavior_ == ServerBehavior::ObservePreclosed) {
            std::unique_lock lock{mutex_};
            changed_.wait(lock, [this] { return startCheckReleased_; });
            if (shutdown_) {
                return Domain::Result<void>::failure(Domain::makeError(
                    Domain::ErrorCodes::TransportClosed,
                    "The recording server was closed before startup."));
            }
        }
        if (behavior_ == ServerBehavior::ReturnFailure) {
            return Domain::Result<void>::failure(Domain::makeError(
                Domain::ErrorCodes::TransportClosed,
                "The recording server failed."));
        }
        return Domain::Result<void>::success();
    }

    void cancel(const Domain::OperationId&) noexcept override {}

    void shutdown() noexcept override
    {
        bool record = false;
        {
            const std::lock_guard lock{mutex_};
            if (!shutdown_) {
                shutdown_ = true;
                ++shutdownCalls_;
                record = true;
            }
        }
        if (record) {
            events_->append("server.shutdown");
        }
        changed_.notify_all();
    }

    [[nodiscard]] bool waitUntilRun(const std::chrono::milliseconds timeout)
    {
        std::unique_lock lock{mutex_};
        return changed_.wait_for(lock, timeout, [this] { return runEntered_; });
    }

    void releaseStartCheck()
    {
        {
            const std::lock_guard lock{mutex_};
            startCheckReleased_ = true;
        }
        changed_.notify_all();
    }

    [[nodiscard]] std::size_t runCalls() const noexcept
    {
        const std::lock_guard lock{mutex_};
        return runCalls_;
    }

    [[nodiscard]] std::size_t shutdownCalls() const noexcept
    {
        const std::lock_guard lock{mutex_};
        return shutdownCalls_;
    }

private:
    std::shared_ptr<EventLog> events_;
    ServerBehavior behavior_;
    mutable std::mutex mutex_;
    std::condition_variable changed_;
    std::size_t runCalls_{};
    std::size_t shutdownCalls_{};
    bool runEntered_{};
    bool shutdown_{};
    bool startCheckReleased_{};
};

[[noreturn]] void fail(const std::string& message)
{
    throw std::runtime_error{message};
}

void require(const bool condition, const std::string& message)
{
    if (!condition) {
        fail(message);
    }
}

template <typename T>
void requireError(
    const Domain::Result<T>& result,
    const std::string_view code,
    const std::string& message)
{
    require(!result, message + " unexpectedly succeeded");
    require(result.error().code == code, message + " returned the wrong error");
}

[[nodiscard]] Domain::OperationContext context()
{
    return Domain::OperationContext{
        Domain::OperationId::parse("11111111-1111-4111-8111-111111111111")
            .value(),
        Domain::MonotonicTimePoint::max(),
        {},
        Domain::CorrelationId::parse("manager-process-host-test").value()};
}

[[nodiscard]] std::size_t eventIndex(
    const std::vector<std::string>& events,
    const std::string_view event)
{
    const auto found = std::find(events.begin(), events.end(), event);
    require(found != events.end(), "Missing lifecycle event: " + std::string{event});
    return static_cast<std::size_t>(std::distance(events.begin(), found));
}

struct Fixture final {
    Fixture(const ServerBehavior behavior)
        : events{std::make_shared<EventLog>()},
          clock{std::make_shared<FakeClock>()},
          controller{std::make_shared<RecordingController>(events)},
          dispatcher{std::make_shared<Manager::ManagerRequestDispatcher>(
              controller, clock)},
          server{std::make_unique<RecordingServer>(events, behavior)},
          serverObserver{server.get()},
          host{controller, dispatcher, std::move(server)}
    {
    }

    std::shared_ptr<EventLog> events;
    std::shared_ptr<FakeClock> clock;
    std::shared_ptr<RecordingController> controller;
    std::shared_ptr<Manager::ManagerRequestDispatcher> dispatcher;
    std::unique_ptr<RecordingServer> server;
    RecordingServer* serverObserver;
    Host::ManagerProcessHost host;
};

void shutdownClosesIngressBeforeController()
{
    Fixture fixture{ServerBehavior::BlockUntilShutdown};
    std::optional<Domain::Result<void>> runResult;
    std::jthread worker{[&fixture, &runResult] {
        runResult.emplace(fixture.host.run(context(), context()));
    }};

    if (!fixture.serverObserver->waitUntilRun(2s)) {
        fixture.host.shutdown();
        worker.join();
        fail("server run entry");
    }
    fixture.host.shutdown();
    worker.join();

    require(runResult.has_value() && runResult->hasValue(), "host run result");
    require(fixture.serverObserver->shutdownCalls() == 1U, "server shutdown once");
    require(fixture.controller->shutdownCalls() == 1U, "controller shutdown once");
    const auto events = fixture.events->snapshot();
    require(
        eventIndex(events, "controller.initialize") <
            eventIndex(events, "server.run"),
        "the controller must initialize before ingress is exposed");
    require(
        eventIndex(events, "server.shutdown") <
            eventIndex(events, "controller.shutdown"),
        "ingress must close before the controller");

    fixture.host.shutdown();
    require(fixture.serverObserver->shutdownCalls() == 1U, "idempotent server close");
    require(fixture.controller->shutdownCalls() == 1U, "idempotent controller close");
}

void initializationFailureSkipsIngressAndClosesOwners()
{
    Fixture fixture{ServerBehavior::ReturnSuccess};
    fixture.controller->failInitialization(Domain::makeError(
        Domain::ErrorCodes::IntegrityFailure,
        "The recording initialization failed."));

    const auto result = fixture.host.run(context(), context());
    requireError(result, Domain::ErrorCodes::IntegrityFailure, "initialization failure");
    require(fixture.serverObserver->runCalls() == 0U, "failed startup skipped ingress");
    require(fixture.serverObserver->shutdownCalls() == 1U, "failed startup closed server");
    require(fixture.controller->shutdownCalls() == 1U, "failed startup closed controller");
}

void ingressFailureIsPropagatedAfterCleanup()
{
    Fixture fixture{ServerBehavior::ReturnFailure};
    const auto result = fixture.host.run(context(), context());

    requireError(result, Domain::ErrorCodes::TransportClosed, "ingress failure");
    require(fixture.controller->initializeCalls() == 1U, "controller initialized once");
    require(fixture.serverObserver->runCalls() == 1U, "server ran once");
    require(fixture.serverObserver->shutdownCalls() == 1U, "failed server closed once");
    require(fixture.controller->shutdownCalls() == 1U, "failed server closed controller");
}

void runIsSingleUseAndPreShutdownRejectsRun()
{
    Fixture singleUse{ServerBehavior::ReturnSuccess};
    require(singleUse.host.run(context(), context()).hasValue(), "first run");
    requireError(
        singleUse.host.run(context(), context()),
        Domain::ErrorCodes::Conflict,
        "second run");
    require(singleUse.controller->initializeCalls() == 1U, "single initialization");
    require(singleUse.serverObserver->runCalls() == 1U, "single server run");

    Fixture closed{ServerBehavior::ReturnSuccess};
    closed.host.shutdown();
    requireError(
        closed.host.run(context(), context()),
        Domain::ErrorCodes::TransportClosed,
        "run after shutdown");
    require(closed.controller->initializeCalls() == 0U, "closed host skipped initialization");
    require(closed.serverObserver->runCalls() == 0U, "closed host skipped server");
}

void startupShutdownRaceIsCancelledAndBounded()
{
    constexpr std::size_t Iterations = 64U;
    for (std::size_t iteration = 0U; iteration < Iterations; ++iteration) {
        Fixture fixture{ServerBehavior::ReturnSuccess};
        fixture.controller->blockInitialization();
        std::optional<Domain::Result<void>> runResult;
        std::jthread worker{[&fixture, &runResult] {
            runResult.emplace(fixture.host.run(context(), context()));
        }};

        if (!fixture.controller->waitUntilInitialization(2s)) {
            fixture.host.shutdown();
            worker.join();
            fail("blocked initialization entry");
        }
        const auto started = std::chrono::steady_clock::now();
        fixture.host.shutdown();
        const auto elapsed = std::chrono::steady_clock::now() - started;
        require(elapsed < 250ms, "shutdown blocked on manager initialization");

        worker.join();
        require(runResult.has_value(), "startup race run result");
        requireError(
            *runResult,
            Domain::ErrorCodes::Cancelled,
            "startup race cancellation");
        require(
            fixture.serverObserver->runCalls() == 0U,
            "ingress started after shutdown claimed startup");
        require(
            fixture.serverObserver->shutdownCalls() == 1U,
            "startup race server shutdown once");
        require(
            fixture.controller->shutdownCalls() == 1U,
            "startup race controller shutdown once");
    }
}

void shutdownDuringIngressTransitionIsClean()
{
    Fixture fixture{ServerBehavior::ObservePreclosed};
    std::optional<Domain::Result<void>> runResult;
    std::jthread worker{[&fixture, &runResult] {
        runResult.emplace(fixture.host.run(context(), context()));
    }};

    if (!fixture.serverObserver->waitUntilRun(2s)) {
        fixture.host.shutdown();
        fixture.serverObserver->releaseStartCheck();
        worker.join();
        fail("transition server run entry");
    }
    fixture.host.shutdown();
    fixture.serverObserver->releaseStartCheck();
    worker.join();

    require(
        runResult.has_value() && runResult->hasValue(),
        "orderly transition shutdown was reported as an ingress failure");
    require(fixture.serverObserver->runCalls() == 1U, "transition server run once");
    require(fixture.serverObserver->shutdownCalls() == 1U, "transition shutdown once");
    require(fixture.controller->shutdownCalls() == 1U, "transition controller close once");
}

} // namespace

int main()
{
    try {
        shutdownClosesIngressBeforeController();
        initializationFailureSkipsIngressAndClosesOwners();
        ingressFailureIsPropagatedAfterCleanup();
        runIsSingleUseAndPreShutdownRejectsRun();
        startupShutdownRaceIsCancelledAndBounded();
        shutdownDuringIngressTransitionIsClean();
        std::cout << "Manager process host tests passed.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Manager process host tests failed: " << error.what() << '\n';
        return 1;
    } catch (...) {
        std::cerr << "Manager process host tests failed with an unknown error.\n";
        return 1;
    }
}
