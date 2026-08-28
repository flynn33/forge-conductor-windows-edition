#include "Infrastructure/Windows/Detail/DashboardWinsockRuntime.h"

#include <array>
#include <cstddef>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

namespace {

namespace Domain = ForgeConductor::Domain;
namespace Detail = ForgeConductor::Infrastructure::Windows::Detail;

using Api = Detail::IDashboardWinsockApi;
using Runtime = Detail::DashboardWinsockRuntime;
using Socket = Detail::UniqueDashboardSocket;
using SystemApi = Detail::DashboardWinsockSystemApi;

static_assert(std::is_abstract_v<Api>);
static_assert(std::is_final_v<SystemApi>);
static_assert(std::is_final_v<Runtime>);
static_assert(!std::is_copy_constructible_v<Runtime>);
static_assert(!std::is_move_constructible_v<Runtime>);
static_assert(std::is_final_v<Socket>);
static_assert(!std::is_copy_constructible_v<Socket>);
static_assert(std::is_nothrow_move_constructible_v<Socket>);
static_assert(std::is_nothrow_move_assignable_v<Socket>);
static_assert(std::is_nothrow_destructible_v<Socket>);
static_assert(Runtime::RequiredVersion == MAKEWORD(2, 2));
static_assert(
    Runtime::RequiredSocketFlags ==
    (WSA_FLAG_OVERLAPPED | WSA_FLAG_NO_HANDLE_INHERIT));
static_assert(noexcept(Runtime::create()));
static_assert(noexcept(Runtime::create({})));
static_assert(noexcept(
    std::declval<Runtime&>().createOverlappedTcpSocket(AF_INET)));
static_assert(noexcept(std::declval<Socket&>().reset()));

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

enum class Event : unsigned char {
    Startup,
    Cleanup,
    CreateSocket,
    LastError,
    CloseSocket,
};

class FakeApi final : public Api {
public:
    [[nodiscard]] int startup(
        const WORD requestedVersion,
        WSADATA& data) noexcept override
    {
        record(Event::Startup);
        requestedVersion_ = requestedVersion;
        data.wVersion = negotiatedVersion;
        data.wHighVersion = highVersion;
        return startupStatus;
    }

    [[nodiscard]] int cleanup() noexcept override
    {
        record(Event::Cleanup);
        ++cleanupCalls;
        return cleanupStatus;
    }

    [[nodiscard]] SOCKET createSocket(
        const int addressFamily,
        const int socketType,
        const int protocol,
        const DWORD flags) noexcept override
    {
        record(Event::CreateSocket);
        ++createCalls;
        addressFamily_ = addressFamily;
        socketType_ = socketType;
        protocol_ = protocol;
        flags_ = flags;
        return socketToReturn;
    }

    [[nodiscard]] int closeSocket(const SOCKET socket) noexcept override
    {
        record(Event::CloseSocket);
        ++closeCalls;
        lastClosedSocket = socket;
        return closeStatus;
    }

    [[nodiscard]] int lastError() noexcept override
    {
        record(Event::LastError);
        ++lastErrorCalls;
        return socketError;
    }

    [[nodiscard]] Event event(const std::size_t index) const
    {
        require(index < eventCount_, "event index exceeded recorded calls");
        return events_[index];
    }

    [[nodiscard]] std::size_t eventCount() const noexcept { return eventCount_; }
    [[nodiscard]] WORD requestedVersion() const noexcept
    {
        return requestedVersion_;
    }
    [[nodiscard]] int addressFamily() const noexcept { return addressFamily_; }
    [[nodiscard]] int socketType() const noexcept { return socketType_; }
    [[nodiscard]] int protocol() const noexcept { return protocol_; }
    [[nodiscard]] DWORD flags() const noexcept { return flags_; }

    WORD negotiatedVersion{MAKEWORD(2, 2)};
    WORD highVersion{MAKEWORD(2, 2)};
    int startupStatus{};
    int cleanupStatus{};
    SOCKET socketToReturn{static_cast<SOCKET>(101U)};
    int socketError{WSAEINVAL};
    int closeStatus{};
    std::size_t cleanupCalls{};
    std::size_t createCalls{};
    std::size_t lastErrorCalls{};
    std::size_t closeCalls{};
    SOCKET lastClosedSocket{INVALID_SOCKET};

private:
    void record(const Event event) noexcept
    {
        if (eventCount_ < events_.size()) {
            events_[eventCount_++] = event;
        }
    }

    std::array<Event, 32U> events_{};
    std::size_t eventCount_{};
    WORD requestedVersion_{};
    int addressFamily_{};
    int socketType_{};
    int protocol_{};
    DWORD flags_{};
};

[[nodiscard]] std::unique_ptr<Runtime> runtime(
    const std::shared_ptr<FakeApi>& api)
{
    return take(Runtime::create(api));
}

void startupAndCleanupAreBalancedExactlyOnce()
{
    auto api = std::make_shared<FakeApi>();
    api->cleanupStatus = SOCKET_ERROR;
    {
        auto owner = runtime(api);
        require(owner != nullptr, "runtime factory returned a null owner");
        require(api->requestedVersion() == MAKEWORD(2, 2),
                "runtime requested a Winsock version other than 2.2");
        require(api->cleanupCalls == 0U,
                "successful startup cleaned up before owner destruction");
        require(api->eventCount() == 1U && api->event(0U) == Event::Startup,
                "successful startup made unexpected native calls");
    }
    require(api->cleanupCalls == 1U,
            "runtime destruction did not clean up exactly once");
    require(api->eventCount() == 2U && api->event(1U) == Event::Cleanup,
            "cleanup did not follow startup");
}

void startupFailuresAreTypedAndNeverCleanedUp()
{
    struct Case final {
        int nativeCode;
        std::string_view stableCode;
        bool retryable;
    };
    constexpr std::array cases{
        Case{WSAVERNOTSUPPORTED, Domain::ErrorCodes::UnsupportedVersion, false},
        Case{WSASYSNOTREADY, Domain::ErrorCodes::HostCapabilityUnavailable, true},
        Case{WSAEPROCLIM, Domain::ErrorCodes::LimitExceeded, true},
        Case{WSAEINPROGRESS, Domain::ErrorCodes::Conflict, true},
        Case{WSAEFAULT, Domain::ErrorCodes::InternalFailure, false},
    };

    for (const auto& testCase : cases) {
        auto api = std::make_shared<FakeApi>();
        api->startupStatus = testCase.nativeCode;
        const auto result = Runtime::create(api);
        require(!result, "failed WSAStartup returned a runtime");
        require(result.error().code == testCase.stableCode,
                "WSAStartup failure returned the wrong stable code");
        require(result.error().retryable == testCase.retryable,
                "WSAStartup failure returned the wrong retryability");
        require(result.error().message.find(std::to_string(testCase.nativeCode)) !=
                    std::string::npos,
                "WSAStartup failure omitted its native error");
        require(api->cleanupCalls == 0U,
                "failed WSAStartup incorrectly called WSACleanup");
        require(api->eventCount() == 1U && api->event(0U) == Event::Startup,
                "failed WSAStartup made unexpected native calls");
    }
}

void negotiatedVersionMismatchCleansUpBeforeRejection()
{
    constexpr std::array invalidVersions{
        MAKEWORD(1, 1), MAKEWORD(2, 0), MAKEWORD(2, 1), MAKEWORD(3, 0)};
    for (const WORD version : invalidVersions) {
        auto api = std::make_shared<FakeApi>();
        api->negotiatedVersion = version;
        const auto result = Runtime::create(api);
        require(!result, "an unsupported negotiated version was accepted");
        require(result.error().code == Domain::ErrorCodes::UnsupportedVersion,
                "negotiated-version rejection returned the wrong stable code");
        require(!result.error().retryable,
                "negotiated-version rejection was retryable");
        require(api->cleanupCalls == 1U,
                "negotiated-version rejection did not balance startup");
        require(api->eventCount() == 2U &&
                    api->event(0U) == Event::Startup &&
                    api->event(1U) == Event::Cleanup,
                "version rejection used the wrong native ordering");
    }
}

void nullApiIsRejectedWithoutNativeCalls()
{
    const auto result = Runtime::create(std::shared_ptr<Api>{});
    require(!result, "runtime accepted a null API dependency");
    require(result.error().code == Domain::ErrorCodes::InvalidRequest,
            "null API returned the wrong stable code");
    require(!result.error().retryable, "null API rejection was retryable");
}

void factoryUsesExactTcpAndHandleFlags()
{
    auto api = std::make_shared<FakeApi>();
    auto owner = runtime(api);
    auto ipv4 = take(owner->createOverlappedTcpSocket(AF_INET));

    require(ipv4.get() == api->socketToReturn,
            "factory did not return the native socket");
    require(static_cast<bool>(ipv4), "created socket was not valid");
    require(api->createCalls == 1U, "factory did not create exactly one socket");
    require(api->addressFamily() == AF_INET,
            "factory changed the IPv4 address family");
    require(api->socketType() == SOCK_STREAM,
            "factory did not request a stream socket");
    require(api->protocol() == IPPROTO_TCP,
            "factory did not request TCP");
    require(api->flags() ==
                (WSA_FLAG_OVERLAPPED | WSA_FLAG_NO_HANDLE_INHERIT),
            "factory omitted the overlapped or non-inheritable flag");
    require(api->eventCount() == 2U &&
                api->event(0U) == Event::Startup &&
                api->event(1U) == Event::CreateSocket,
            "socket creation did not follow startup directly");

    ipv4.reset();
    auto ipv6 = take(owner->createOverlappedTcpSocket(AF_INET6));
    require(api->addressFamily() == AF_INET6,
            "factory changed the IPv6 address family");
    ipv6.reset();

    const auto unsupported = owner->createOverlappedTcpSocket(AF_UNSPEC);
    require(!unsupported, "factory accepted an unspecified address family");
    require(unsupported.error().code == Domain::ErrorCodes::InvalidRequest,
            "invalid family returned the wrong stable code");
    require(api->createCalls == 2U,
            "invalid family reached the native socket API");
}

void socketFailureMappingIsTypedAndDoesNotClose()
{
    struct Case final {
        int nativeCode;
        std::string_view stableCode;
        bool retryable;
    };
    constexpr std::array cases{
        Case{WSAEMFILE, Domain::ErrorCodes::LimitExceeded, true},
        Case{WSAENOBUFS, Domain::ErrorCodes::LimitExceeded, true},
        Case{WSANOTINITIALISED,
             Domain::ErrorCodes::HostCapabilityUnavailable, true},
        Case{WSAENETDOWN,
             Domain::ErrorCodes::HostCapabilityUnavailable, true},
        Case{WSAEAFNOSUPPORT,
             Domain::ErrorCodes::HostCapabilityUnavailable, false},
        Case{WSAEPROTONOSUPPORT,
             Domain::ErrorCodes::HostCapabilityUnavailable, false},
        Case{WSAEACCES, Domain::ErrorCodes::InternalFailure, false},
    };

    for (const auto& testCase : cases) {
        auto api = std::make_shared<FakeApi>();
        api->socketToReturn = INVALID_SOCKET;
        api->socketError = testCase.nativeCode;
        auto owner = runtime(api);
        const auto result = owner->createOverlappedTcpSocket(AF_INET);
        require(!result, "failed WSASocketW returned an owned socket");
        require(result.error().code == testCase.stableCode,
                "socket failure returned the wrong stable code");
        require(result.error().retryable == testCase.retryable,
                "socket failure returned the wrong retryability");
        require(result.error().message.find(std::to_string(testCase.nativeCode)) !=
                    std::string::npos,
                "socket failure omitted its native error");
        require(api->createCalls == 1U && api->lastErrorCalls == 1U,
                "socket failure did not read exactly one native error");
        require(api->closeCalls == 0U,
                "invalid socket result was passed to closesocket");
        require(api->eventCount() == 3U &&
                    api->event(1U) == Event::CreateSocket &&
                    api->event(2U) == Event::LastError,
                "socket failure used the wrong native ordering");
    }
}

void resetAndDestructionCloseExactlyOnce()
{
    auto api = std::make_shared<FakeApi>();
    api->closeStatus = SOCKET_ERROR;
    auto owner = runtime(api);
    {
        auto socket = take(owner->createOverlappedTcpSocket(AF_INET));
        socket.reset();
        socket.reset();
        require(api->closeCalls == 1U,
                "repeated reset closed the same socket more than once");
        require(api->lastClosedSocket == static_cast<SOCKET>(101U),
                "reset closed the wrong socket");
    }
    require(api->closeCalls == 1U,
            "destruction reclosed an explicitly reset socket");

    api->socketToReturn = static_cast<SOCKET>(202U);
    {
        auto socket = take(owner->createOverlappedTcpSocket(AF_INET6));
        require(socket.get() == static_cast<SOCKET>(202U),
                "second factory result had the wrong socket");
    }
    require(api->closeCalls == 2U,
            "socket destruction did not close exactly once");
    require(api->lastClosedSocket == static_cast<SOCKET>(202U),
            "destruction closed the wrong socket");
}

void movesTransferOwnershipWithoutCreatingARawOwner()
{
    auto firstApi = std::make_shared<FakeApi>();
    auto firstRuntime = runtime(firstApi);
    auto first = take(firstRuntime->createOverlappedTcpSocket(AF_INET));
    Socket moved{std::move(first)};
    require(!first, "move source retained its socket");
    require(moved.get() == static_cast<SOCKET>(101U),
            "move destination lost its socket");
    require(firstApi->closeCalls == 0U, "move construction closed the socket");

    auto secondApi = std::make_shared<FakeApi>();
    secondApi->socketToReturn = static_cast<SOCKET>(303U);
    auto secondRuntime = runtime(secondApi);
    auto second = take(secondRuntime->createOverlappedTcpSocket(AF_INET6));
    moved = std::move(second);
    require(firstApi->closeCalls == 1U,
            "move assignment did not close prior ownership exactly once");
    require(firstApi->lastClosedSocket == static_cast<SOCKET>(101U),
            "move assignment closed the wrong prior socket");
    require(!second, "move-assignment source retained its socket");
    require(moved.get() == static_cast<SOCKET>(303U),
            "move assignment lost replacement ownership");

    moved = std::move(moved);
    require(moved.get() == static_cast<SOCKET>(303U),
            "self move assignment changed socket ownership");
    require(secondApi->closeCalls == 0U,
            "self move assignment closed the socket");

    moved.reset();
    require(secondApi->closeCalls == 1U,
            "reset did not close moved ownership exactly once");
    require(secondApi->lastClosedSocket == static_cast<SOCKET>(303U),
            "reset closed the wrong moved socket");
}

void socketsKeepStartupAliveUntilTheyClose()
{
    auto api = std::make_shared<FakeApi>();
    auto owner = runtime(api);
    auto socket = take(owner->createOverlappedTcpSocket(AF_INET));

    owner.reset();
    require(api->cleanupCalls == 0U,
            "runtime cleaned up while an owned socket remained live");
    require(api->closeCalls == 0U,
            "runtime destruction closed a socket owned elsewhere");

    socket.reset();
    require(api->closeCalls == 1U,
            "last socket owner did not close its socket");
    require(api->cleanupCalls == 1U,
            "last socket owner did not release the startup reference");
    require(api->eventCount() == 4U &&
                api->event(2U) == Event::CloseSocket &&
                api->event(3U) == Event::Cleanup,
            "WSACleanup occurred before closesocket");
}

void systemApiSmokeIsProcessIsolated()
{
    auto owner = take(Runtime::create());
    require(owner != nullptr, "real Winsock runtime factory returned null");
    auto socket = take(owner->createOverlappedTcpSocket(AF_INET));
    require(static_cast<bool>(socket),
            "real overlapped non-inheritable socket was invalid");
    socket.reset();
    owner.reset();
}

} // namespace

int main()
{
    try {
        startupAndCleanupAreBalancedExactlyOnce();
        startupFailuresAreTypedAndNeverCleanedUp();
        negotiatedVersionMismatchCleansUpBeforeRejection();
        nullApiIsRejectedWithoutNativeCalls();
        factoryUsesExactTcpAndHandleFlags();
        socketFailureMappingIsTypedAndDoesNotClose();
        resetAndDestructionCloseExactlyOnce();
        movesTransferOwnershipWithoutCreatingARawOwner();
        socketsKeepStartupAliveUntilTheyClose();
        systemApiSmokeIsProcessIsolated();
        std::cout << "Dashboard Winsock runtime tests passed: "
                  << assertionCount << " assertions\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Dashboard Winsock runtime tests failed after "
                  << assertionCount << " assertions: " << error.what() << '\n';
        return 1;
    } catch (...) {
        std::cerr << "Dashboard Winsock runtime tests failed after "
                  << assertionCount << " assertions: unknown exception\n";
        return 1;
    }
}
