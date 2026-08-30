#include "WindowsManagerRuntime.h"

#include "ForgeConductor/Dashboard/DashboardResponseComposer.h"
#include "ForgeConductor/Domain/ConfigurationModels.h"

#include <WS2tcpip.h>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <iomanip>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

namespace {

namespace Contracts = ForgeConductor::Contracts;
namespace Dashboard = ForgeConductor::Dashboard;
namespace Domain = ForgeConductor::Domain;
namespace Manager = ForgeConductor::Hosts::Manager;

using namespace std::chrono_literals;

constexpr auto TestTimeout = 10s;
constexpr DWORD SocketTimeoutMilliseconds = 5'000U;
constexpr std::size_t MaximumResponseBytes = 64U * 1024U;

std::size_t assertions{};

[[noreturn]] void fail(const std::string_view message)
{
    throw std::runtime_error{std::string{message}};
}

void require(const bool condition, const std::string_view message)
{
    ++assertions;
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

template <typename Value>
void requireError(
    const Domain::Result<Value>& result,
    const std::string_view code,
    const std::string_view message)
{
    require(!result, message);
    require(result.error().code == code, "wrong stable error code");
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
            fail("could not initialize Winsock for manager runtime tests");
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

    UniqueSocket& operator=(UniqueSocket&& other) noexcept
    {
        if (this != &other) {
            reset();
            socket_ = std::exchange(other.socket_, INVALID_SOCKET);
        }
        return *this;
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
            return Domain::Result<Domain::Uuid>::failure(Domain::makeError(
                Domain::ErrorCodes::InternalFailure,
                "The manager runtime test UUID could not be created."));
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
        prepareCalls_.fetch_add(1U);
        lastOperationalServiceActive_.store(operationalServiceActive);
        return Dashboard::DashboardResponseComposer::completeText(
            200U,
            "text/plain; charset=utf-8",
            responseText_);
    }

    [[nodiscard]] Domain::Result<void> executePostDelivery(
        Dashboard::DashboardPostDeliveryAction,
        Domain::OperationContext) noexcept override
    {
        return Domain::Result<void>::success();
    }

    [[nodiscard]] bool lastOperationalServiceActive() const noexcept
    {
        return lastOperationalServiceActive_.load();
    }

    [[nodiscard]] std::size_t prepareCalls() const noexcept
    {
        return prepareCalls_.load();
    }

private:
    const std::string responseText_;
    std::atomic_size_t prepareCalls_{};
    std::atomic_bool lastOperationalServiceActive_{};
};

class RecordingApplicationFactory final
    : public Dashboard::IDashboardConnectionApplicationFactory {
public:
    [[nodiscard]] Domain::Result<std::shared_ptr<
        Dashboard::IDashboardConnectionApplication>> create(
        const Domain::DashboardConfig& configuration) noexcept override
    {
        try {
            const std::lock_guard lock{mutex_};
            ++createCalls_;
            if (failNext_) {
                failNext_ = false;
                return Domain::Result<std::shared_ptr<
                    Dashboard::IDashboardConnectionApplication>>::failure(
                    Domain::makeError(
                        Domain::ErrorCodes::InternalFailure,
                        "scripted application factory failure"));
            }

            const auto generation = applications_.size() + 1U;
            auto application = std::make_shared<RecordingApplication>(
                "generation=" + std::to_string(generation) +
                ";port=" + std::to_string(configuration.port));
            applications_.push_back(application);
            return Domain::Result<std::shared_ptr<
                Dashboard::IDashboardConnectionApplication>>::success(
                application);
        } catch (...) {
            return Domain::Result<std::shared_ptr<
                Dashboard::IDashboardConnectionApplication>>::failure(
                Domain::makeError(
                    Domain::ErrorCodes::InternalFailure,
                    "The manager runtime test factory failed."));
        }
    }

    void failNext() noexcept
    {
        try {
            const std::lock_guard lock{mutex_};
            failNext_ = true;
        } catch (...) {
        }
    }

    [[nodiscard]] std::size_t createCalls() const noexcept
    {
        try {
            const std::lock_guard lock{mutex_};
            return createCalls_;
        } catch (...) {
            return 0U;
        }
    }

    [[nodiscard]] std::shared_ptr<RecordingApplication> application(
        const std::size_t index) const
    {
        const std::lock_guard lock{mutex_};
        if (index >= applications_.size()) {
            fail("manager runtime test application index is out of range");
        }
        return applications_[index];
    }

private:
    mutable std::mutex mutex_;
    std::vector<std::shared_ptr<RecordingApplication>> applications_;
    std::size_t createCalls_{};
    bool failNext_{};
};

[[nodiscard]] Domain::OperationContext context(
    const Contracts::IClock& clock)
{
    static std::atomic_uint64_t next{1U};
    const auto sequence = next.fetch_add(1U);
    std::ostringstream operation;
    operation << "10000000-0000-4000-8000-"
              << std::setw(12) << std::setfill('0') << sequence;
    return Domain::OperationContext{
        take(Domain::OperationId::parse(operation.str())),
        clock.monotonicNow() + 15s,
        {},
        take(Domain::CorrelationId::parse(
            "manager-runtime-" + std::to_string(sequence)))};
}

[[nodiscard]] std::uint16_t reserveUnusedIpv4Port(
    const std::optional<std::uint16_t> excluded = std::nullopt)
{
    for (std::size_t attempt{}; attempt < 32U; ++attempt) {
        UniqueSocket socket{::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP)};
        if (socket.get() == INVALID_SOCKET) {
            fail("could not create a manager port-reservation socket");
        }

        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address.sin_port = 0U;
        if (::bind(
                socket.get(),
                reinterpret_cast<const sockaddr*>(&address),
                static_cast<int>(sizeof(address))) == SOCKET_ERROR) {
            fail("could not reserve a manager loopback test port");
        }

        int length = static_cast<int>(sizeof(address));
        if (::getsockname(
                socket.get(),
                reinterpret_cast<sockaddr*>(&address),
                &length) == SOCKET_ERROR) {
            fail("could not inspect a manager loopback test port");
        }
        const auto port = ntohs(address.sin_port);
        if (port != 0U && (!excluded.has_value() || port != *excluded)) {
            return port;
        }
    }
    fail("could not reserve distinct manager loopback test ports");
}

[[nodiscard]] int bindProbe(const std::uint16_t port)
{
    UniqueSocket socket{::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP)};
    if (socket.get() == INVALID_SOCKET) {
        fail("could not create a manager loopback bind probe");
    }
    const BOOL exclusive = TRUE;
    if (::setsockopt(
            socket.get(),
            SOL_SOCKET,
            SO_EXCLUSIVEADDRUSE,
            reinterpret_cast<const char*>(&exclusive),
            static_cast<int>(sizeof(exclusive))) == SOCKET_ERROR) {
        fail("could not configure a manager loopback bind probe");
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

[[nodiscard]] bool portIsAvailable(const std::uint16_t port)
{
    return bindProbe(port) == 0;
}

void setSocketTimeouts(const SOCKET socket)
{
    const DWORD timeout = SocketTimeoutMilliseconds;
    if (::setsockopt(
            socket,
            SOL_SOCKET,
            SO_RCVTIMEO,
            reinterpret_cast<const char*>(&timeout),
            static_cast<int>(sizeof(timeout))) == SOCKET_ERROR ||
        ::setsockopt(
            socket,
            SOL_SOCKET,
            SO_SNDTIMEO,
            reinterpret_cast<const char*>(&timeout),
            static_cast<int>(sizeof(timeout))) == SOCKET_ERROR) {
        fail("could not configure manager client socket timeouts");
    }
}

[[nodiscard]] std::string httpGet(const std::uint16_t port)
{
    UniqueSocket socket{::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP)};
    if (socket.get() == INVALID_SOCKET) {
        fail("could not create manager HTTP client socket");
    }
    setSocketTimeouts(socket.get());

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons(port);
    if (::connect(
            socket.get(),
            reinterpret_cast<const sockaddr*>(&address),
            static_cast<int>(sizeof(address))) == SOCKET_ERROR) {
        fail("could not connect to the manager dashboard listener");
    }

    const std::string request =
        "GET /ping HTTP/1.1\r\nHost: 127.0.0.1:" +
        std::to_string(port) + "\r\nConnection: close\r\n\r\n";
    std::size_t sent{};
    while (sent < request.size()) {
        const auto remaining = request.size() - sent;
        const int written = ::send(
            socket.get(),
            request.data() + sent,
            static_cast<int>(remaining),
            0);
        if (written <= 0) {
            fail("could not send manager dashboard request");
        }
        sent += static_cast<std::size_t>(written);
    }

    std::string response;
    char buffer[4'096]{};
    for (;;) {
        const int received = ::recv(
            socket.get(), buffer, static_cast<int>(sizeof(buffer)), 0);
        if (received == 0) {
            break;
        }
        if (received == SOCKET_ERROR) {
            fail("could not receive manager dashboard response");
        }
        response.append(buffer, static_cast<std::size_t>(received));
        if (response.size() > MaximumResponseBytes) {
            fail("manager dashboard response exceeded its test bound");
        }
    }
    require(
        response.starts_with("HTTP/1.1 200 "),
        "manager dashboard did not return HTTP 200");
    return response;
}

[[nodiscard]] Domain::AppConfig configuration(const std::uint16_t port)
{
    auto config = Domain::defaultAppConfig();
    config.dashboard.host = "127.0.0.1";
    config.dashboard.port = port;
    config.dashboard.refreshInterval = 8s;
    return config;
}

void lifecycleAndRealLoopbackCutoversAreExact()
{
    auto clock = std::make_shared<SystemClock>();
    auto uuid = std::make_shared<SequenceUuidGenerator>();
    auto factory = std::make_shared<RecordingApplicationFactory>();
    auto runtime = take(Manager::WindowsManagerRuntime::create(
        clock, uuid, factory));

    const auto firstPort = reserveUnusedIpv4Port();
    const auto secondPort = reserveUnusedIpv4Port(firstPort);
    const auto firstConfig = configuration(firstPort);
    const auto secondConfig = configuration(secondPort);

    const auto started = take(runtime->start(firstConfig, context(*clock)));
    require(started.listenerListening, "initial listener was not published");
    require(started.operationalServiceActive,
            "initial operational service was not active");
    require(started.startedAt.has_value(), "initial uptime epoch is missing");
    require(started.restartCount == 0U, "initial start advanced restart count");
    require(!started.lastError.has_value(), "initial start retained an error");
    require(!started.shutdownRequested,
            "initial start latched shutdown intent");
    require(factory->createCalls() == 1U,
            "initial start did not create exactly one application");
    require(waitUntil([&]() { return portIsExclusivelyOwned(firstPort); }),
            "initial listener did not own its loopback port");
    require(httpGet(firstPort).find("generation=1;port=") != std::string::npos,
            "initial listener did not serve the first application");
    require(factory->application(0U)->lastOperationalServiceActive(),
            "initial request did not observe active operational service");

    const auto paused = take(runtime->pause(context(*clock)));
    require(paused.listenerListening && !paused.operationalServiceActive,
            "pause closed the listener or left operational service active");
    static_cast<void>(httpGet(firstPort));
    require(!factory->application(0U)->lastOperationalServiceActive(),
            "paused listener request did not observe inactive service");

    const auto resumed = take(runtime->start(firstConfig, context(*clock)));
    require(resumed.listenerListening && resumed.operationalServiceActive,
            "idempotent start did not resume the existing listener");
    require(resumed.restartCount == 0U,
            "resume advanced the explicit restart count");
    require(resumed.startedAt == started.startedAt,
            "resume reset the runtime uptime epoch");
    require(factory->createCalls() == 1U,
            "resume replaced the existing application generation");

    factory->failNext();
    const auto failedRestart = runtime->rebind(
        firstConfig, true, context(*clock));
    requireError(
        failedRestart,
        Domain::ErrorCodes::InternalFailure,
        "scripted same-port factory failure returned success");
    const auto afterFailure = take(runtime->snapshot(context(*clock)));
    require(afterFailure.listenerListening &&
                afterFailure.operationalServiceActive,
            "pre-cutover failure disturbed the healthy old listener");
    require(afterFailure.restartCount == 1U,
            "failed explicit restart did not retain one attempted count");
    require(afterFailure.lastError == "scripted application factory failure",
            "failed explicit restart did not retain its stable error");
    require(httpGet(firstPort).find("generation=1;port=") != std::string::npos,
            "pre-cutover factory failure replaced the old application");

    const auto samePortRestart = take(runtime->rebind(
        firstConfig, true, context(*clock)));
    require(samePortRestart.listenerListening &&
                samePortRestart.operationalServiceActive,
            "same-port restart did not publish its successor");
    require(samePortRestart.restartCount == 2U,
            "same-port restart did not advance the count exactly once");
    require(samePortRestart.startedAt.has_value() &&
                samePortRestart.startedAt != started.startedAt,
            "same-port restart did not begin a new uptime epoch");
    require(factory->createCalls() == 3U,
            "same-port retry did not make one fresh factory call");
    require(httpGet(firstPort).find("generation=2;port=") != std::string::npos,
            "same-port restart did not serve the successor application");

    const auto distinct = take(runtime->rebind(
        secondConfig, false, context(*clock)));
    require(distinct.listenerListening && !distinct.operationalServiceActive,
            "distinct-port rebind lost the listener or desired paused state");
    require(distinct.restartCount == 3U,
            "distinct-port rebind did not advance count exactly once");
    require(waitUntil([&]() { return portIsAvailable(firstPort); }),
            "distinct-port rebind did not retire the old endpoint");
    require(waitUntil([&]() { return portIsExclusivelyOwned(secondPort); }),
            "distinct-port rebind did not own the new endpoint");
    require(httpGet(secondPort).find("generation=3;port=") != std::string::npos,
            "distinct-port rebind did not serve the new application");
    require(!factory->application(2U)->lastOperationalServiceActive(),
            "distinct-port request did not preserve desired paused state");

    auto nonbinding = secondConfig;
    nonbinding.dashboard.refreshInterval = 13s;
    const auto applied = take(runtime->applySettings(
        nonbinding, context(*clock)));
    require(applied.listenerListening && !applied.operationalServiceActive &&
                applied.restartCount == 3U,
            "nonbinding settings changed listener, service, or restart count");
    require(factory->createCalls() == 4U,
            "nonbinding settings unexpectedly created an application");

    const auto shutdownRequested = take(runtime->requestShutdown(
        context(*clock)));
    require(shutdownRequested.shutdownRequested,
            "shutdown request did not latch process intent");
    require(shutdownRequested.listenerListening,
            "shutdown request prematurely drained the listener");
    static_cast<void>(httpGet(secondPort));

    runtime->shutdown();
    runtime->shutdown();
    require(waitUntil([&]() { return portIsAvailable(secondPort); }),
            "final runtime shutdown did not release the listener port");
}

void invalidContextsAndConfigurationAreRejected()
{
    auto clock = std::make_shared<SystemClock>();
    auto uuid = std::make_shared<SequenceUuidGenerator>();
    auto factory = std::make_shared<RecordingApplicationFactory>();
    auto runtime = take(Manager::WindowsManagerRuntime::create(
        clock, uuid, factory));
    const auto port = reserveUnusedIpv4Port();
    const auto config = configuration(port);

    std::stop_source cancellation;
    cancellation.request_stop();
    auto cancelled = context(*clock);
    cancelled.cancellation = cancellation.get_token();
    requireError(
        runtime->start(config, cancelled),
        Domain::ErrorCodes::Cancelled,
        "runtime start ignored pre-cancellation");

    auto expired = context(*clock);
    expired.deadline = clock->monotonicNow();
    requireError(
        runtime->start(config, expired),
        Domain::ErrorCodes::DeadlineExceeded,
        "runtime start ignored an expired deadline");

    auto invalid = config;
    invalid.dashboard.host = "0.0.0.0";
    requireError(
        runtime->start(invalid, context(*clock)),
        Domain::ErrorCodes::InvalidRequest,
        "runtime accepted a non-loopback dashboard configuration");
    require(factory->createCalls() == 0U,
            "rejected start reached the application factory");

    const auto started = take(runtime->start(config, context(*clock)));
    require(started.listenerListening, "valid runtime start failed");
    auto mismatch = config;
    mismatch.dashboard.port = reserveUnusedIpv4Port(port);
    requireError(
        runtime->applySettings(mismatch, context(*clock)),
        Domain::ErrorCodes::Conflict,
        "nonbinding settings silently changed the endpoint");
    const auto preserved = take(runtime->snapshot(context(*clock)));
    require(preserved.listenerListening && preserved.restartCount == 0U,
            "rejected nonbinding endpoint change disturbed runtime state");
    runtime->shutdown();
    require(waitUntil([&]() { return portIsAvailable(port); }),
            "context test shutdown did not release the loopback port");
}

static_assert(std::is_final_v<Manager::WindowsManagerRuntime>);
static_assert(!std::is_copy_constructible_v<Manager::WindowsManagerRuntime>);
static_assert(!std::is_move_constructible_v<Manager::WindowsManagerRuntime>);
static_assert(std::is_base_of_v<
              Contracts::IManagerRuntime,
              Manager::WindowsManagerRuntime>);
static_assert(noexcept(std::declval<Manager::WindowsManagerRuntime&>().shutdown()));

} // namespace

int main()
{
    try {
        WinsockScope winsock;
        lifecycleAndRealLoopbackCutoversAreExact();
        std::cout << "PASS manager_windows_runtime.lifecycle\n";
        invalidContextsAndConfigurationAreRejected();
        std::cout << "PASS manager_windows_runtime.validation\n";
        std::cout << "SUMMARY assertions=" << assertions
                  << " failed=0\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL manager_windows_runtime " << error.what() << '\n';
        return 1;
    } catch (...) {
        std::cerr << "FAIL manager_windows_runtime unknown error\n";
        return 1;
    }
}
