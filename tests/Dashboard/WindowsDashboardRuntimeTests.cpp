#include "Infrastructure/Windows/Detail/WindowsDashboardRuntime.h"

#include "Infrastructure/Windows/Detail/DashboardIocpWorkerKernel.h"
#include "Infrastructure/Windows/Detail/ManagerDashboardOperationalState.h"

#include "ForgeConductor/Dashboard/DashboardResponseComposer.h"

#include <WS2tcpip.h>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <iomanip>
#include <iostream>
#include <memory>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <utility>

namespace {

namespace Contracts = ForgeConductor::Contracts;
namespace Dashboard = ForgeConductor::Dashboard;
namespace Detail = ForgeConductor::Infrastructure::Windows::Detail;
namespace Domain = ForgeConductor::Domain;

using Runtime = Detail::WindowsDashboardRuntime;
using RuntimeBinding = Detail::WindowsDashboardRuntimeBinding;
using RuntimeLifecycle = Detail::WindowsDashboardRuntimeLifecycle;

using namespace std::chrono_literals;

static_assert(std::is_final_v<Runtime>);
static_assert(!std::is_copy_constructible_v<Runtime>);
static_assert(!std::is_move_constructible_v<Runtime>);
static_assert(noexcept(std::declval<Runtime&>().pauseOperationalService()));
static_assert(noexcept(std::declval<Runtime&>().resumeOperationalService()));
static_assert(noexcept(
    std::declval<Runtime&>().requestGracefulShutdown()));

constexpr auto TestTimeout = 10s;
constexpr DWORD SocketTimeoutMilliseconds = 5'000U;
constexpr std::size_t MaximumTestResponseBytes = 64U * 1024U;

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

void takeVoid(Domain::Result<void> result)
{
    if (!result) {
        fail(result.error().code + ": " + result.error().message);
    }
}

template <typename Value>
void requireError(
    const Domain::Result<Value>& result,
    const std::string_view code,
    const bool retryable,
    const std::string_view message)
{
    require(!result, message);
    require(result.error().code == code, "wrong stable error code");
    require(result.error().retryable == retryable,
            "wrong retryable classification");
}

template <typename Predicate>
[[nodiscard]] bool waitUntil(Predicate&& predicate)
{
    const auto deadline = std::chrono::steady_clock::now() + TestTimeout;
    while (!std::invoke(std::forward<Predicate>(predicate))) {
        if (std::chrono::steady_clock::now() >= deadline) {
            return false;
        }
        std::this_thread::sleep_for(1ms);
    }
    return true;
}

class WinsockScope final {
public:
    WinsockScope()
    {
        WSADATA data{};
        if (::WSAStartup(MAKEWORD(2, 2), &data) != 0) {
            fail("could not initialize Winsock for dashboard runtime tests");
        }
        initialized_ = true;
    }

    ~WinsockScope() noexcept
    {
        if (initialized_) {
            ::WSACleanup();
        }
    }

    WinsockScope(const WinsockScope&) = delete;
    WinsockScope& operator=(const WinsockScope&) = delete;

private:
    bool initialized_{};
};

class SystemClock final : public Contracts::IClock {
public:
    [[nodiscard]] Domain::UtcTimePoint utcNow() const noexcept override
    {
        return std::chrono::system_clock::now();
    }

    [[nodiscard]] Domain::MonotonicTimePoint monotonicNow()
        const noexcept override
    {
        return std::chrono::steady_clock::now();
    }
};

class SequenceUuidGenerator final : public Contracts::IUuidGenerator {
public:
    [[nodiscard]] Domain::Result<Domain::Uuid> next() noexcept override
    {
        try {
            const auto sequence = next_.fetch_add(1U);
            std::ostringstream text;
            text << "00000000-0000-4000-8000-"
                 << std::setw(12) << std::setfill('0') << sequence;
            return Domain::Uuid::parse(text.str());
        } catch (...) {
            return Domain::Result<Domain::Uuid>::failure(
                Domain::makeError(
                    Domain::ErrorCodes::InternalFailure,
                    "The dashboard runtime test UUID could not be created."));
        }
    }

private:
    std::atomic<std::uint64_t> next_{1U};
};

class RecordingApplication final
    : public Dashboard::IDashboardConnectionApplication {
public:
    explicit RecordingApplication(std::string responseText)
        : responseText_{std::move(responseText)}
    {
    }

    [[nodiscard]] Domain::Result<Dashboard::DashboardPreparedExchange> prepare(
        Dashboard::DashboardHttpRequest,
        const bool operationalServiceActive,
        Domain::OperationContext) noexcept override
    {
        lastOperationalServiceActive_.store(operationalServiceActive);
        prepareCalls_.fetch_add(1U);
        return Dashboard::DashboardResponseComposer::completeText(
            200U,
            "text/plain; charset=utf-8",
            responseText_);
    }

    [[nodiscard]] Domain::Result<void> executePostDelivery(
        Dashboard::DashboardPostDeliveryAction,
        Domain::OperationContext) noexcept override
    {
        postDeliveryCalls_.fetch_add(1U);
        return Domain::Result<void>::success();
    }

    [[nodiscard]] std::size_t prepareCalls() const noexcept
    {
        return prepareCalls_.load();
    }

    [[nodiscard]] bool lastOperationalServiceActive() const noexcept
    {
        return lastOperationalServiceActive_.load();
    }

private:
    const std::string responseText_;
    std::atomic_size_t prepareCalls_{};
    std::atomic_size_t postDeliveryCalls_{};
    std::atomic_bool lastOperationalServiceActive_{};
};

class UniqueSocket final {
public:
    explicit UniqueSocket(const SOCKET socket = INVALID_SOCKET) noexcept
        : socket_{socket}
    {
    }

    ~UniqueSocket() noexcept { reset(); }

    UniqueSocket(const UniqueSocket&) = delete;
    UniqueSocket& operator=(const UniqueSocket&) = delete;

    UniqueSocket(UniqueSocket&& other) noexcept
        : socket_{std::exchange(other.socket_, INVALID_SOCKET)}
    {
    }

    [[nodiscard]] SOCKET get() const noexcept { return socket_; }

    void reset() noexcept
    {
        if (socket_ != INVALID_SOCKET) {
            ::closesocket(socket_);
            socket_ = INVALID_SOCKET;
        }
    }

private:
    SOCKET socket_{INVALID_SOCKET};
};

[[nodiscard]] std::uint16_t reserveUnusedIpv4Port(
    const std::optional<std::uint16_t> excluded = std::nullopt)
{
    for (std::size_t attempt{}; attempt < 32U; ++attempt) {
        UniqueSocket socket{::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP)};
        if (socket.get() == INVALID_SOCKET) {
            fail("could not create a port-reservation socket");
        }

        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address.sin_port = 0U;
        if (::bind(
                socket.get(),
                reinterpret_cast<const sockaddr*>(&address),
                static_cast<int>(sizeof(address))) == SOCKET_ERROR) {
            fail("could not reserve a loopback test port");
        }

        int length = static_cast<int>(sizeof(address));
        if (::getsockname(
                socket.get(),
                reinterpret_cast<sockaddr*>(&address),
                &length) == SOCKET_ERROR) {
            fail("could not inspect a reserved loopback test port");
        }
        const auto port = ntohs(address.sin_port);
        if (port != 0U && (!excluded.has_value() || port != *excluded)) {
            return port;
        }
    }
    fail("could not reserve a distinct loopback test port");
}

[[nodiscard]] int bindProbe(const std::uint16_t port)
{
    UniqueSocket socket{::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP)};
    if (socket.get() == INVALID_SOCKET) {
        fail("could not create a loopback bind probe");
    }
    const BOOL exclusive = TRUE;
    if (::setsockopt(
            socket.get(),
            SOL_SOCKET,
            SO_EXCLUSIVEADDRUSE,
            reinterpret_cast<const char*>(&exclusive),
            static_cast<int>(sizeof(exclusive))) == SOCKET_ERROR) {
        fail("could not configure a loopback bind probe");
    }

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons(port);
    const int result = ::bind(
        socket.get(),
        reinterpret_cast<const sockaddr*>(&address),
        static_cast<int>(sizeof(address)));
    return result == SOCKET_ERROR ? ::WSAGetLastError() : 0;
}

[[nodiscard]] bool portIsExclusivelyOwned(const std::uint16_t port)
{
    const auto error = bindProbe(port);
    return error == WSAEADDRINUSE || error == WSAEACCES;
}

[[nodiscard]] std::string request(const std::uint16_t port)
{
    UniqueSocket socket{::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP)};
    if (socket.get() == INVALID_SOCKET) {
        fail("could not create a dashboard client socket");
    }
    const DWORD timeout = SocketTimeoutMilliseconds;
    if (::setsockopt(
            socket.get(),
            SOL_SOCKET,
            SO_RCVTIMEO,
            reinterpret_cast<const char*>(&timeout),
            static_cast<int>(sizeof(timeout))) == SOCKET_ERROR ||
        ::setsockopt(
            socket.get(),
            SOL_SOCKET,
            SO_SNDTIMEO,
            reinterpret_cast<const char*>(&timeout),
            static_cast<int>(sizeof(timeout))) == SOCKET_ERROR) {
        fail("could not bound dashboard client socket I/O");
    }

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons(port);
    if (::connect(
            socket.get(),
            reinterpret_cast<const sockaddr*>(&address),
            static_cast<int>(sizeof(address))) == SOCKET_ERROR) {
        fail("could not connect to the dashboard runtime listener");
    }

    const auto wireRequest = std::string{
        "GET /runtime-test HTTP/1.1\r\nHost: 127.0.0.1:"} +
        std::to_string(port) + "\r\nConnection: close\r\n\r\n";
    std::size_t sent{};
    while (sent < wireRequest.size()) {
        const auto amount = ::send(
            socket.get(),
            wireRequest.data() + sent,
            static_cast<int>(wireRequest.size() - sent),
            0);
        if (amount == SOCKET_ERROR || amount == 0) {
            fail("could not send the bounded dashboard request");
        }
        sent += static_cast<std::size_t>(amount);
    }

    std::string response;
    response.reserve(4U * 1024U);
    char buffer[4U * 1024U]{};
    for (;;) {
        const auto amount = ::recv(
            socket.get(), buffer, static_cast<int>(sizeof(buffer)), 0);
        if (amount == 0) {
            break;
        }
        if (amount == SOCKET_ERROR) {
            fail("dashboard response did not complete within its socket bound");
        }
        if (response.size() + static_cast<std::size_t>(amount) >
            MaximumTestResponseBytes) {
            fail("dashboard response exceeded the test byte bound");
        }
        response.append(buffer, static_cast<std::size_t>(amount));
    }
    return response;
}

[[nodiscard]] Domain::DashboardConfig configuration(
    const std::uint16_t port,
    const std::chrono::seconds refreshInterval = 8s)
{
    return Domain::DashboardConfig{
        "127.0.0.1", port, refreshInterval};
}

[[nodiscard]] RuntimeBinding binding(
    const std::uint16_t port,
    std::shared_ptr<Dashboard::IDashboardConnectionApplication> application,
    const std::chrono::seconds refreshInterval = 8s)
{
    return RuntimeBinding{
        configuration(port, refreshInterval), std::move(application)};
}

struct RuntimeFixture final {
    RuntimeFixture()
        : clock{std::make_shared<SystemClock>()},
          uuid{std::make_shared<SequenceUuidGenerator>()},
          operationalState{
              std::make_shared<Detail::ManagerDashboardOperationalState>()},
          runtime{take(Runtime::create(clock, uuid, operationalState))}
    {
    }

    ~RuntimeFixture() noexcept
    {
        runtime->requestGracefulShutdown();
        static_cast<void>(runtime->wait());
    }

    std::shared_ptr<SystemClock> clock;
    std::shared_ptr<SequenceUuidGenerator> uuid;
    std::shared_ptr<Detail::ManagerDashboardOperationalState>
        operationalState;
    std::unique_ptr<Runtime> runtime;
};

void rejectsInvalidDependenciesAndWaitBeforeShutdown()
{
    auto clock = std::make_shared<SystemClock>();
    auto uuid = std::make_shared<SequenceUuidGenerator>();
    auto operational =
        std::make_shared<Detail::ManagerDashboardOperationalState>();
    requireError(
        Runtime::create(nullptr, uuid, operational),
        Domain::ErrorCodes::InvalidRequest,
        false,
        "runtime creation accepted a null clock");
    requireError(
        Runtime::create(clock, nullptr, operational),
        Domain::ErrorCodes::InvalidRequest,
        false,
        "runtime creation accepted a null UUID generator");
    requireError(
        Runtime::create(clock, uuid, nullptr),
        Domain::ErrorCodes::InvalidRequest,
        false,
        "runtime creation accepted a null operational-state owner");

    RuntimeFixture fixture;
    requireError(
        fixture.runtime->wait(),
        Domain::ErrorCodes::Conflict,
        false,
        "runtime wait blocked or succeeded before shutdown");
    const auto ready = take(fixture.runtime->snapshot());
    require(ready.lifecycle() == RuntimeLifecycle::Ready &&
                ready.shutdownDrainInstalled() &&
                ready.fixedCompletionTargetCount() == 1U &&
                ready.registeredAuxiliaryDeadlineTargetCount() == 2U &&
                ready.startedWorkerCount() ==
                    Detail::DashboardIocpWorkerKernel::WorkerCount &&
                ready.exitedWorkerCount() == 0U,
            "ready runtime did not expose the installed bounded owner graph");

    fixture.runtime->requestGracefulShutdown();
    fixture.runtime->requestGracefulShutdown();
    takeVoid(fixture.runtime->wait());
    takeVoid(fixture.runtime->wait());
    const auto drained = take(fixture.runtime->snapshot());
    require(drained.lifecycle() == RuntimeLifecycle::Drained &&
                drained.fixedCompletionTargetCount() == 0U &&
                drained.registeredAuxiliaryDeadlineTargetCount() == 0U &&
                drained.registeredConnectionCount() == 0U &&
                drained.startedWorkerCount() ==
                    Detail::DashboardIocpWorkerKernel::WorkerCount &&
                drained.exitedWorkerCount() ==
                    Detail::DashboardIocpWorkerKernel::WorkerCount &&
                !drained.configuration().has_value() &&
                drained.applicationIdentity() == nullptr,
            "idempotent empty shutdown retained routes, workers, or binding ownership");
}

void startsServesPauseResumeAndShutsDown()
{
    RuntimeFixture fixture;
    const auto port = reserveUnusedIpv4Port();
    auto application =
        std::make_shared<RecordingApplication>("runtime-live");

    auto invalid = binding(port, application);
    invalid.configuration.host = "localhost";
    requireError(
        fixture.runtime->start(std::move(invalid)),
        Domain::ErrorCodes::InvalidRequest,
        false,
        "initial start accepted a nonliteral loopback host");
    const auto afterInvalid = take(fixture.runtime->snapshot());
    require(afterInvalid.lifecycle() == RuntimeLifecycle::Ready &&
                !afterInvalid.configuration().has_value() &&
                !afterInvalid.activeRegistrationId().has_value() &&
                afterInvalid.listenerPublicationCount() == 0U,
            "failed initial start retained a future binding or listener owner");

    fixture.runtime->resumeOperationalService();
    takeVoid(fixture.runtime->start(binding(port, application)));
    const auto listening = take(fixture.runtime->snapshot());
    require(listening.lifecycle() == RuntimeLifecycle::Listening &&
                listening.configuration() == configuration(port) &&
                listening.applicationIdentity() == application.get() &&
                listening.bindingPublicationSequence() == 1U &&
                listening.activeRegistrationId().has_value() &&
                listening.listenerPublicationCount() == 1U &&
                listening.listenerRetirementCount() == 0U &&
                listening.fixedCompletionTargetCount() == 2U &&
                listening.registeredAuxiliaryDeadlineTargetCount() == 3U &&
                listening.operationalServiceActive() &&
                portIsExclusivelyOwned(port),
            "live runtime did not own one exact listener and bounded routes");

    const auto activeResponse = request(port);
    require(activeResponse.starts_with("HTTP/1.1 200 OK\r\n") &&
                activeResponse.find("runtime-live") != std::string::npos &&
                application->prepareCalls() == 1U &&
                application->lastOperationalServiceActive(),
            "real loopback request did not reach the active application policy");

    fixture.runtime->pauseOperationalService();
    const auto pausedResponse = request(port);
    require(pausedResponse.starts_with("HTTP/1.1 200 OK\r\n") &&
                application->prepareCalls() == 2U &&
                !application->lastOperationalServiceActive() &&
                !take(fixture.runtime->snapshot()).operationalServiceActive(),
            "pause did not reach the next immutable request context");

    std::weak_ptr<RecordingApplication> weakApplication = application;
    application.reset();
    fixture.runtime->requestGracefulShutdown();
    fixture.runtime->requestGracefulShutdown();
    require(
        waitUntil([&fixture] {
            const auto observed = fixture.runtime->snapshot();
            return observed &&
                observed.value().fixedCompletionTargetCount() == 0U &&
                observed.value().exitedWorkerCount() ==
                    Detail::DashboardIocpWorkerKernel::WorkerCount;
        }),
        "shutdown graph did not reach background drain before final wait");
    const auto awaitingWait = take(fixture.runtime->snapshot());
    require(
        awaitingWait.lifecycle() == RuntimeLifecycle::ShuttingDown &&
            awaitingWait.configuration() == configuration(port) &&
            awaitingWait.applicationIdentity() != nullptr &&
            !weakApplication.expired(),
        "background drain claimed final ownership release before wait");
    takeVoid(fixture.runtime->wait());
    takeVoid(fixture.runtime->wait());
    const auto drained = take(fixture.runtime->snapshot());
    require(drained.lifecycle() == RuntimeLifecycle::Drained &&
                drained.registeredConnectionCount() == 0U &&
                drained.registeredAuxiliaryDeadlineTargetCount() == 0U &&
                drained.fixedCompletionTargetCount() == 0U &&
                drained.exitedWorkerCount() ==
                    Detail::DashboardIocpWorkerKernel::WorkerCount &&
                !drained.operationalServiceActive() &&
                !drained.configuration().has_value() &&
                weakApplication.expired(),
            "live shutdown did not release all routes, workers, and application ownership");
}

void collisionFailureClearsBindingAndAllowsRetryAndPortReacquisition()
{
    RuntimeFixture owner;
    RuntimeFixture contender;
    const auto port = reserveUnusedIpv4Port();
    auto ownerApplication =
        std::make_shared<RecordingApplication>("owner");
    auto contenderApplication =
        std::make_shared<RecordingApplication>("contender");

    takeVoid(owner.runtime->start(binding(port, ownerApplication)));
    require(portIsExclusivelyOwned(port),
            "first runtime did not retain exclusive loopback ownership");
    auto collided = contender.runtime->start(
        binding(port, contenderApplication));
    require(!collided,
            "second runtime unexpectedly started on an owned endpoint");
    const auto afterCollision = take(contender.runtime->snapshot());
    require(afterCollision.lifecycle() == RuntimeLifecycle::Ready &&
                !afterCollision.configuration().has_value() &&
                !afterCollision.activeRegistrationId().has_value() &&
                afterCollision.listenerPublicationCount() == 0U &&
                afterCollision.fixedCompletionTargetCount() == 1U &&
                afterCollision.registeredAuxiliaryDeadlineTargetCount() == 2U,
            "colliding initial start retained binding or generation ownership");

    owner.runtime->requestGracefulShutdown();
    takeVoid(owner.runtime->wait());
    require(waitUntil([port] { return bindProbe(port) == 0; }),
            "shutdown did not make the initially owned port reacquirable");

    takeVoid(contender.runtime->start(
        binding(port, contenderApplication)));
    require(portIsExclusivelyOwned(port) &&
                take(contender.runtime->snapshot()).
                    activeRegistrationId().has_value(),
            "colliding runtime could not retry after exact owner shutdown");
    contender.runtime->requestGracefulShutdown();
    takeVoid(contender.runtime->wait());
    require(waitUntil([port] { return bindProbe(port) == 0; }),
            "retrying runtime did not release the reacquired port");
}

void rebindsDistinctEndpointsAndRestoresAfterFailedReplacement()
{
    RuntimeFixture fixture;
    const auto portA = reserveUnusedIpv4Port();
    const auto portB = reserveUnusedIpv4Port(portA);
    auto applicationA =
        std::make_shared<RecordingApplication>("binding-a");
    auto applicationB =
        std::make_shared<RecordingApplication>("binding-b");
    takeVoid(fixture.runtime->start(binding(portA, applicationA)));
    const auto initial = take(fixture.runtime->snapshot());
    const auto initialRegistration = initial.activeRegistrationId();
    require(initialRegistration.has_value(),
            "initial rebind fixture did not publish a listener");

    requireError(
        fixture.runtime->rebind(binding(portA, applicationA, 13s)),
        Domain::ErrorCodes::Conflict,
        false,
        "same-endpoint rebind did not fail closed for deferred cutover");
    requireError(
        fixture.runtime->rebind(binding(portA, applicationB)),
        Domain::ErrorCodes::Conflict,
        false,
        "same-endpoint application replacement did not fail closed");
    const auto afterSameEndpointConflict =
        take(fixture.runtime->snapshot());
    require(afterSameEndpointConflict.lifecycle() ==
                RuntimeLifecycle::Listening &&
                afterSameEndpointConflict.configuration() ==
                    configuration(portA) &&
                afterSameEndpointConflict.applicationIdentity() ==
                    applicationA.get() &&
                afterSameEndpointConflict.bindingPublicationSequence() ==
                    1U &&
                afterSameEndpointConflict.activeRegistrationId() ==
                    initialRegistration &&
                afterSameEndpointConflict.listenerPublicationCount() == 1U &&
                afterSameEndpointConflict.listenerRetirementCount() == 0U &&
                portIsExclusivelyOwned(portA),
            "same-endpoint conflict mutated binding or listener ownership");

    takeVoid(fixture.runtime->rebind(binding(portB, applicationB, 11s)));
    const auto rebound = take(fixture.runtime->snapshot());
    require(rebound.lifecycle() == RuntimeLifecycle::Listening &&
                rebound.configuration() == configuration(portB, 11s) &&
                rebound.applicationIdentity() == applicationB.get() &&
                rebound.bindingPublicationSequence() == 2U &&
                rebound.activeRegistrationId().has_value() &&
                rebound.activeRegistrationId() != initialRegistration &&
                rebound.listenerPublicationCount() == 2U &&
                rebound.listenerRetirementCount() == 1U &&
                portIsExclusivelyOwned(portB),
            "A/B rebind did not publish the replacement binding and generation");
    require(waitUntil([&fixture] {
                return !take(fixture.runtime->snapshot()).
                    retiringRegistrationId().has_value();
            }) &&
                waitUntil([portA] { return bindProbe(portA) == 0; }),
            "A/B rebind did not retire and release the previous listener");
    const auto response = request(portB);
    require(response.find("binding-b") != std::string::npos &&
                applicationB->prepareCalls() == 1U,
            "A/B rebind did not route new requests to the replacement policy");

    const auto conflictingPort = reserveUnusedIpv4Port(portB);
    RuntimeFixture blocker;
    auto blockerApplication =
        std::make_shared<RecordingApplication>("blocker");
    takeVoid(blocker.runtime->start(
        binding(conflictingPort, blockerApplication)));
    auto failedRebind = fixture.runtime->rebind(
        binding(conflictingPort, applicationA));
    require(!failedRebind,
            "rebind unexpectedly replaced a listener on an owned endpoint");
    const auto restored = take(fixture.runtime->snapshot());
    require(restored.lifecycle() == RuntimeLifecycle::Listening &&
                restored.configuration() == configuration(portB, 11s) &&
                restored.applicationIdentity() == applicationB.get() &&
                restored.bindingPublicationSequence() == 4U &&
                restored.activeRegistrationId() ==
                    rebound.activeRegistrationId() &&
                restored.listenerPublicationCount() == 2U &&
                restored.listenerRetirementCount() == 1U &&
                request(portB).find("binding-b") != std::string::npos,
            "failed rebind did not restore a coherent active binding snapshot");

    blocker.runtime->requestGracefulShutdown();
    takeVoid(blocker.runtime->wait());
    fixture.runtime->requestGracefulShutdown();
    takeVoid(fixture.runtime->wait());
}

} // namespace

int main()
{
    try {
        const WinsockScope winsock;
        rejectsInvalidDependenciesAndWaitBeforeShutdown();
        startsServesPauseResumeAndShutsDown();
        collisionFailureClearsBindingAndAllowsRetryAndPortReacquisition();
        rebindsDistinctEndpointsAndRestoresAfterFailedReplacement();
        std::cout << "Windows dashboard runtime tests passed ("
                  << assertionCount << " assertions).\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "Windows dashboard runtime tests failed: "
                  << exception.what() << '\n';
        return 1;
    }
}
