#include "ForgeConductor/Manager/ManagerRequestDispatcher.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <exception>
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
#include <variant>
#include <vector>

namespace {

namespace Contracts = ForgeConductor::Contracts;
namespace Domain = ForgeConductor::Domain;
namespace Manager = ForgeConductor::Manager;

using namespace std::chrono_literals;

class FakeClock final : public Contracts::IClock {
public:
    Domain::UtcTimePoint utc{std::chrono::seconds{1'700'000'000}};
    Domain::MonotonicTimePoint monotonic{std::chrono::seconds{1'000}};

    [[nodiscard]] Domain::UtcTimePoint utcNow() const noexcept override
    {
        return utc;
    }

    [[nodiscard]] Domain::MonotonicTimePoint monotonicNow() const noexcept override
    {
        return monotonic;
    }
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

[[nodiscard]] std::string uuidText(const std::uint32_t suffix)
{
    constexpr char Digits[] = "0123456789abcdef";
    std::string value{"00000000-0000-4000-8000-000000000000"};
    auto remaining = suffix;
    for (std::size_t index{}; index < 8U; ++index) {
        value[value.size() - 1U - index] = Digits[remaining & 0xFU];
        remaining >>= 4U;
    }
    return value;
}

[[nodiscard]] Domain::OperationId operationId(const std::uint32_t suffix)
{
    return Domain::OperationId::parse(uuidText(suffix)).value();
}

[[nodiscard]] Domain::RequestId requestId(const std::uint32_t suffix)
{
    return Domain::RequestId::parse(uuidText(suffix)).value();
}

[[nodiscard]] Domain::CorrelationId correlationId(const std::uint32_t suffix)
{
    return Domain::CorrelationId::parse(
               "manager-dispatch-" + std::to_string(suffix))
        .value();
}

[[nodiscard]] Domain::Sha256Digest nonce()
{
    return Domain::Sha256Digest::parse(std::string(64U, '0')).value();
}

[[nodiscard]] std::int64_t futureWireDeadline(
    const FakeClock& clock,
    const std::chrono::milliseconds offset = 1min)
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               clock.utc.time_since_epoch())
               .count() +
        offset.count();
}

[[nodiscard]] Manager::ManagerRequest request(
    const FakeClock& clock,
    const std::uint32_t suffix,
    Manager::ManagerRequestPayload payload)
{
    return Manager::ManagerRequest{
        Manager::ManagerProtocolVersion,
        requestId(suffix),
        correlationId(suffix),
        futureWireDeadline(clock),
        nonce(),
        std::move(payload)};
}

[[nodiscard]] const Domain::Error* responseError(
    const Manager::ManagerResponse& response)
{
    return std::get_if<Domain::Error>(&response.body);
}

template <typename T>
[[nodiscard]] const T* responseValue(const Manager::ManagerResponse& response)
{
    const auto* result = std::get_if<Manager::ManagerResult>(&response.body);
    return result ? std::get_if<T>(result) : nullptr;
}

void requireError(
    const Manager::ManagerResponse& response,
    const std::string_view code,
    const std::string& message)
{
    const auto* failure = responseError(response);
    require(failure != nullptr, message + " unexpectedly succeeded");
    require(failure->code == code, message + " returned the wrong error");
}

class FakeController final : public Contracts::IManagerController {
public:
    [[nodiscard]] Domain::Result<Domain::ManagerStatus> initialize(
        const Domain::OperationContext&) noexcept override
    {
        return Domain::Result<Domain::ManagerStatus>::success(statusValue());
    }

    [[nodiscard]] Domain::Result<Domain::ManagerControllerSnapshot> snapshot(
        const Domain::OperationContext&) noexcept override
    {
        return Domain::Result<Domain::ManagerControllerSnapshot>::success(
            Domain::ManagerControllerSnapshot{statusValue(), false});
    }

    [[nodiscard]] Domain::Result<Domain::ManagerStatus> status(
        const Domain::OperationContext& context) noexcept override
    {
        if (const auto blocked = blockIfRequested("status", context); blocked) {
            return Domain::Result<Domain::ManagerStatus>::failure(*blocked);
        }
        if (failStatus_) {
            return Domain::Result<Domain::ManagerStatus>::failure(
                Domain::makeError(
                    Domain::ErrorCodes::DatabaseBusy,
                    "injected status failure",
                    true));
        }
        ++statusCalls_;
        return Domain::Result<Domain::ManagerStatus>::success(statusValue());
    }

    [[nodiscard]] Domain::Result<Domain::ManagerSettings> settings(
        const Domain::OperationContext&) noexcept override
    {
        ++settingsCalls_;
        return Domain::Result<Domain::ManagerSettings>::success(settingsValue());
    }

    [[nodiscard]] Domain::Result<Domain::ManagerStatus> control(
        const Domain::ManagerControlRequest& requestValue,
        const Domain::OperationContext&) noexcept override
    {
        ++controlCalls_;
        lastControlAction_ = requestValue.action;
        return Domain::Result<Domain::ManagerStatus>::success(statusValue());
    }

    [[nodiscard]] Domain::Result<Domain::ManagerSettingsUpdateOutcome>
    updateSettings(
        const Domain::ManagerSettingsPatch& patch,
        const bool applyImmediately,
        const Domain::OperationContext&) noexcept override
    {
        ++updateCalls_;
        lastPatch_ = patch;
        lastApplyImmediately_ = applyImmediately;
        auto settings = settingsValue();
        if (patch.dashboardPort) {
            settings.dashboardPort = *patch.dashboardPort;
        }
        auto status = statusValue();
        status.dashboardPort = settings.dashboardPort;
        return Domain::Result<Domain::ManagerSettingsUpdateOutcome>::success({
            std::move(settings),
            applyImmediately,
            patch.dashboardPort.has_value(),
            std::move(status)});
    }

    [[nodiscard]] Domain::Result<Domain::ManagerControllerSnapshot>
    requestShutdown(const Domain::OperationContext&) noexcept override
    {
        std::lock_guard lock{mutex_};
        events_.push_back("request_shutdown");
        ++requestShutdownCalls_;
        if (failRequestShutdown_) {
            return Domain::Result<Domain::ManagerControllerSnapshot>::failure(
                Domain::makeError(
                    Domain::ErrorCodes::InternalFailure,
                    "injected shutdown failure"));
        }
        return Domain::Result<Domain::ManagerControllerSnapshot>::success(
            Domain::ManagerControllerSnapshot{statusValue(), true});
    }

    void shutdown() noexcept override
    {
        std::lock_guard lock{mutex_};
        events_.push_back("controller_close");
        ++closeCalls_;
    }

    void setBlocking(const bool value) noexcept
    {
        std::lock_guard lock{mutex_};
        blocking_ = value;
        condition_.notify_all();
    }

    void setIgnoreCancellation(const bool value) noexcept
    {
        std::lock_guard lock{mutex_};
        ignoreCancellation_ = value;
    }

    [[nodiscard]] bool waitForActive(
        const std::size_t expected,
        const std::chrono::milliseconds timeout = 2s)
    {
        std::unique_lock lock{mutex_};
        return condition_.wait_for(
            lock, timeout, [&] { return active_ >= expected; });
    }

    [[nodiscard]] std::size_t closeCalls() const noexcept
    {
        std::lock_guard lock{mutex_};
        return closeCalls_;
    }

    [[nodiscard]] std::size_t requestShutdownCalls() const noexcept
    {
        std::lock_guard lock{mutex_};
        return requestShutdownCalls_;
    }

    [[nodiscard]] std::vector<std::string> events() const
    {
        std::lock_guard lock{mutex_};
        return events_;
    }

    bool failStatus_{};
    bool failRequestShutdown_{};
    std::atomic_size_t statusCalls_{};
    std::atomic_size_t settingsCalls_{};
    std::atomic_size_t controlCalls_{};
    std::atomic_size_t updateCalls_{};
    Domain::ManagerControlAction lastControlAction_{};
    Domain::ManagerSettingsPatch lastPatch_;
    bool lastApplyImmediately_{};

private:
    [[nodiscard]] static Domain::ManagerStatus statusValue()
    {
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
            0U,
            std::nullopt,
            true,
            3s,
            false,
            "127.0.0.1",
            7788U,
            8s,
            Domain::PathText::create("C:\\ManagerDispatcherTest").value(),
            "test"};
    }

    [[nodiscard]] static Domain::ManagerSettings settingsValue()
    {
        Domain::ManagerSettings value;
        value.dashboardPort = 7788;
        return value;
    }

    [[nodiscard]] std::optional<Domain::Error> blockIfRequested(
        const std::string_view name,
        const Domain::OperationContext& context) noexcept
    {
        std::unique_lock lock{mutex_};
        if (!blocking_) {
            return std::nullopt;
        }
        ++active_;
        events_.push_back(std::string{name} + "_enter");
        condition_.notify_all();
        const std::stop_callback cancellation{
            context.cancellation, [this] { condition_.notify_all(); }};
        condition_.wait(lock, [&] {
            return !blocking_ ||
                (!ignoreCancellation_ && context.isCancellationRequested());
        });
        --active_;
        events_.push_back(std::string{name} + "_exit");
        condition_.notify_all();
        if (context.isCancellationRequested()) {
            return Domain::makeError(
                Domain::ErrorCodes::Cancelled,
                "fake controller observed cancellation");
        }
        return std::nullopt;
    }

    mutable std::mutex mutex_;
    std::condition_variable condition_;
    bool blocking_{};
    bool ignoreCancellation_{};
    std::size_t active_{};
    std::size_t closeCalls_{};
    std::size_t requestShutdownCalls_{};
    std::vector<std::string> events_;
};

void testPayloadMappingAndControllerFailures()
{
    auto clock = std::make_shared<FakeClock>();
    auto controller = std::make_shared<FakeController>();
    Manager::ManagerRequestDispatcher dispatcher{controller, clock};

    const auto status = dispatcher.dispatch(request(
        *clock, 1U, Manager::ManagerStatusRequest{}));
    require(responseValue<Domain::ManagerStatus>(status) != nullptr, "status result");

    const auto settings = dispatcher.dispatch(request(
        *clock, 2U, Manager::ManagerSettingsRequest{}));
    require(
        responseValue<Domain::ManagerSettings>(settings) != nullptr,
        "settings result");

    const auto control = dispatcher.dispatch(request(
        *clock,
        3U,
        Domain::ManagerControlRequest{Domain::ManagerControlAction::Restart}));
    require(responseValue<Domain::ManagerStatus>(control) != nullptr, "control result");
    require(
        controller->lastControlAction_ == Domain::ManagerControlAction::Restart,
        "control payload forwarding");

    Domain::ManagerSettingsPatch patch;
    patch.dashboardPort = static_cast<std::uint16_t>(8888U);
    const auto update = dispatcher.dispatch(request(
        *clock,
        4U,
        Manager::ManagerSettingsUpdateRequest{patch, true}));
    const auto* updateOutcome =
        responseValue<Domain::ManagerSettingsUpdateOutcome>(update);
    require(updateOutcome != nullptr, "settings update outcome result");
    require(
        responseValue<Domain::ManagerSettings>(update) == nullptr,
        "settings update must not collapse to the settings GET result type");
    require(updateOutcome->settings.dashboardPort == 8888U, "updated settings");
    require(updateOutcome->applied, "settings update applied metadata");
    require(updateOutcome->bindingChanged, "settings binding metadata");
    require(
        updateOutcome->status.dashboardPort == 8888U,
        "settings update status metadata");
    require(
        controller->lastPatch_.dashboardPort == 8888,
        "settings patch forwarding");
    require(controller->lastApplyImmediately_, "settings apply forwarding");

    controller->failStatus_ = true;
    requireError(
        dispatcher.dispatch(request(*clock, 5U, Manager::ManagerStatusRequest{})),
        Domain::ErrorCodes::DatabaseBusy,
        "controller failure mapping");
}

void testDuplicateCapacityAndCancellationBypass()
{
    auto clock = std::make_shared<FakeClock>();
    auto controller = std::make_shared<FakeController>();
    controller->setBlocking(true);
    Manager::ManagerRequestDispatcher dispatcher{controller, clock};

    std::vector<Manager::ManagerResponse> responses;
    responses.reserve(3U);
    std::mutex responseMutex;
    std::vector<std::jthread> workers;
    workers.reserve(3U);
    for (std::uint32_t suffix = 10U; suffix < 13U; ++suffix) {
        workers.emplace_back([&, suffix] {
            auto response = dispatcher.dispatch(request(
                *clock, suffix, Manager::ManagerStatusRequest{}));
            std::lock_guard lock{responseMutex};
            responses.push_back(std::move(response));
        });
    }
    require(controller->waitForActive(3U), "three requests must become active");
    require(dispatcher.activeOperationCount() == 3U, "active count bound");

    requireError(
        dispatcher.dispatch(request(*clock, 10U, Manager::ManagerStatusRequest{})),
        Domain::ErrorCodes::Conflict,
        "duplicate request");
    requireError(
        dispatcher.dispatch(request(*clock, 13U, Manager::ManagerStatusRequest{})),
        Domain::ErrorCodes::LimitExceeded,
        "capacity request");

    const auto cancelResponse = dispatcher.dispatch(request(
        *clock, 10U, Manager::ManagerCancelRequest{operationId(10U)}));
    require(
        responseValue<Manager::ManagerAcknowledgement>(cancelResponse) != nullptr,
        "cancel acknowledgement");
    const auto absentCancel = dispatcher.dispatch(request(
        *clock, 99U, Manager::ManagerCancelRequest{operationId(99U)}));
    require(
        responseValue<Manager::ManagerAcknowledgement>(absentCancel) != nullptr,
        "absent cancel must be idempotent");

    controller->setBlocking(false);
    workers.clear();
    require(dispatcher.waitUntilIdle(2s), "cancelled requests must drain");
    require(responses.size() == 3U, "all admitted requests returned");
}

void testShutdownOrderingAndClosedAdmission()
{
    auto clock = std::make_shared<FakeClock>();
    auto controller = std::make_shared<FakeController>();
    controller->setBlocking(true);
    Manager::ManagerTransportLimits limits;
    limits.shutdownDrainTimeout = 2s;
    Manager::ManagerRequestDispatcher dispatcher{controller, clock, limits};

    std::optional<Manager::ManagerResponse> activeResponse;
    std::jthread active{[&] {
        activeResponse = dispatcher.dispatch(request(
            *clock, 20U, Manager::ManagerStatusRequest{}));
    }};
    require(controller->waitForActive(1U), "shutdown fixture active request");

    const auto shutdown = dispatcher.dispatch(request(
        *clock, 21U, Manager::ManagerShutdownRequest{}));
    require(
        responseValue<Manager::ManagerAcknowledgement>(shutdown) != nullptr,
        "shutdown acknowledgement");
    active.join();
    require(activeResponse.has_value(), "active shutdown response");
    requireError(
        *activeResponse,
        Domain::ErrorCodes::Cancelled,
        "shutdown cancellation response");
    require(!dispatcher.isAccepting(), "shutdown closes regular admission");
    require(controller->requestShutdownCalls() == 1U, "shutdown controller call");

    const auto events = controller->events();
    const auto exit = std::find(events.begin(), events.end(), "status_exit");
    const auto requested =
        std::find(events.begin(), events.end(), "request_shutdown");
    require(exit != events.end(), "active exit event");
    require(requested != events.end(), "request shutdown event");
    require(exit < requested, "active cancellation must drain before shutdown request");

    requireError(
        dispatcher.dispatch(request(*clock, 22U, Manager::ManagerStatusRequest{})),
        Domain::ErrorCodes::TransportClosed,
        "regular work after shutdown");
    const auto cancel = dispatcher.dispatch(request(
        *clock, 22U, Manager::ManagerCancelRequest{operationId(22U)}));
    require(
        responseValue<Manager::ManagerAcknowledgement>(cancel) != nullptr,
        "cancel remains available after shutdown admission closes");
}

void testShutdownFailureAndEnvelopeValidation()
{
    auto clock = std::make_shared<FakeClock>();
    auto controller = std::make_shared<FakeController>();
    controller->failRequestShutdown_ = true;
    Manager::ManagerRequestDispatcher dispatcher{controller, clock};

    auto unsupported = request(*clock, 30U, Manager::ManagerStatusRequest{});
    unsupported.version = 2U;
    requireError(
        dispatcher.dispatch(unsupported),
        Domain::ErrorCodes::UnsupportedVersion,
        "unsupported version");

    auto nonUuid = request(*clock, 31U, Manager::ManagerStatusRequest{});
    nonUuid.requestId = Domain::RequestId::parse("opaque-but-not-uuid").value();
    requireError(
        dispatcher.dispatch(nonUuid),
        Domain::ErrorCodes::InvalidRequest,
        "non-UUID request identifier");

    auto expired = request(*clock, 32U, Manager::ManagerStatusRequest{});
    expired.deadlineUtcMilliseconds =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            clock->utc.time_since_epoch())
            .count();
    requireError(
        dispatcher.dispatch(expired),
        Domain::ErrorCodes::DeadlineExceeded,
        "expired envelope");

    requireError(
        dispatcher.dispatch(request(
            *clock, 33U, Manager::ManagerShutdownRequest{})),
        Domain::ErrorCodes::InternalFailure,
        "shutdown controller failure");
    require(!dispatcher.isAccepting(), "failed shutdown still closes admission");
}

void testBoundedCloseDefersControllerShutdownUntilIdle()
{
    auto clock = std::make_shared<FakeClock>();
    auto controller = std::make_shared<FakeController>();
    const std::weak_ptr<FakeClock> weakClock = clock;
    const std::weak_ptr<FakeController> weakController = controller;
    controller->setBlocking(true);
    controller->setIgnoreCancellation(true);
    Manager::ManagerTransportLimits limits;
    limits.shutdownDrainTimeout = 1ms;
    Manager::ManagerRequestDispatcher dispatcher{controller, clock, limits};

    std::jthread active{[&] {
        static_cast<void>(dispatcher.dispatch(request(
            *clock, 40U, Manager::ManagerStatusRequest{})));
    }};
    require(controller->waitForActive(1U), "bounded close active request");
    dispatcher.shutdown();
    require(
        controller->closeCalls() == 0U,
        "controller close may not race an active callback");
    controller.reset();
    clock.reset();
    require(
        !weakController.expired() && !weakClock.expired(),
        "dispatcher must retain dependencies across a bounded shutdown");
    auto retainedController = weakController.lock();
    require(retainedController != nullptr, "retained controller lifetime");
    retainedController->setBlocking(false);
    active.join();
    require(dispatcher.waitUntilIdle(2s), "bounded close final drain");
    require(
        retainedController->closeCalls() == 1U,
        "last release must close the controller exactly once");
    dispatcher.shutdown();
    require(
        retainedController->closeCalls() == 1U,
        "close must remain idempotent");
}

void testRacingReleaseAndShutdownClosesExactlyOnce()
{
    constexpr std::size_t Iterations = 128U;
    for (std::size_t iteration{}; iteration < Iterations; ++iteration) {
        auto clock = std::make_shared<FakeClock>();
        auto controller = std::make_shared<FakeController>();
        controller->setBlocking(true);
        controller->setIgnoreCancellation(true);
        Manager::ManagerTransportLimits limits;
        limits.shutdownDrainTimeout = 0ms;
        Manager::ManagerRequestDispatcher dispatcher{controller, clock, limits};

        std::jthread active{[&] {
            static_cast<void>(dispatcher.dispatch(request(
                *clock,
                static_cast<std::uint32_t>(100U + iteration),
                Manager::ManagerStatusRequest{})));
        }};
        require(controller->waitForActive(1U), "racing close active request");

        std::atomic_bool start{};
        std::jthread shutdown{[&] {
            while (!start.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
            dispatcher.shutdown();
        }};
        std::jthread release{[&] {
            while (!start.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
            controller->setBlocking(false);
        }};
        start.store(true, std::memory_order_release);
        shutdown.join();
        release.join();
        active.join();

        require(dispatcher.waitUntilIdle(2s), "racing close final drain");
        require(
            controller->closeCalls() == 1U,
            "a release racing the bounded shutdown must close exactly once");

        std::vector<std::jthread> repeatedShutdowns;
        repeatedShutdowns.reserve(4U);
        for (std::size_t caller{}; caller < 4U; ++caller) {
            repeatedShutdowns.emplace_back([&] { dispatcher.shutdown(); });
        }
        repeatedShutdowns.clear();
        require(
            controller->closeCalls() == 1U,
            "concurrent repeated shutdown must not close more than once");
    }
}

void testConstructionRejectsNullDependencies()
{
    auto clock = std::make_shared<FakeClock>();
    auto controller = std::make_shared<FakeController>();

    bool rejectedController{};
    try {
        Manager::ManagerRequestDispatcher dispatcher{
            std::shared_ptr<Contracts::IManagerController>{}, clock};
        static_cast<void>(dispatcher.isAccepting());
    } catch (const std::invalid_argument&) {
        rejectedController = true;
    }
    require(rejectedController, "null controller construction");

    bool rejectedClock{};
    try {
        Manager::ManagerRequestDispatcher dispatcher{
            controller, std::shared_ptr<Contracts::IClock>{}};
        static_cast<void>(dispatcher.isAccepting());
    } catch (const std::invalid_argument&) {
        rejectedClock = true;
    }
    require(rejectedClock, "null clock construction");
}

} // namespace

int main()
{
    try {
        testPayloadMappingAndControllerFailures();
        testDuplicateCapacityAndCancellationBypass();
        testShutdownOrderingAndClosedAdmission();
        testShutdownFailureAndEnvelopeValidation();
        testBoundedCloseDefersControllerShutdownUntilIdle();
        testRacingReleaseAndShutdownClosesExactlyOnce();
        testConstructionRejectsNullDependencies();
        std::cout << "Manager request dispatcher tests passed: 7 groups\n";
        return 0;
    } catch (const std::exception& failure) {
        std::cerr << "Manager request dispatcher tests failed: "
                  << failure.what() << '\n';
        return 1;
    }
}
