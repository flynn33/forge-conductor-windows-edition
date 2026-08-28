#include "Infrastructure/Windows/Detail/DashboardListeningSocket.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstring>
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

using ConfigApi = Detail::IDashboardListeningSocketApi;
using Endpoint = Detail::DashboardLoopbackEndpoint;
using Listener = Detail::DashboardListeningSocket;
using Runtime = Detail::DashboardWinsockRuntime;
using SystemConfigApi = Detail::DashboardListeningSocketSystemApi;
using WinsockApi = Detail::IDashboardWinsockApi;

static_assert(std::is_abstract_v<ConfigApi>);
static_assert(std::is_final_v<SystemConfigApi>);
static_assert(std::is_final_v<Listener>);
static_assert(!std::is_copy_constructible_v<Listener>);
static_assert(std::is_nothrow_move_constructible_v<Listener>);
static_assert(!std::is_move_assignable_v<Listener>);
static_assert(std::is_nothrow_destructible_v<Listener>);
static_assert(Listener::ListenBacklog == 40);
static_assert(noexcept(Listener::create(
    std::declval<Runtime&>(), std::declval<const Endpoint&>())));
static_assert(noexcept(Listener::create(
    std::declval<Runtime&>(),
    std::declval<const Endpoint&>(),
    std::declval<std::shared_ptr<ConfigApi>>())));

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
    CreateSocket,
    WinsockLastError,
    SetExclusiveAddressUse,
    SetIpv6Only,
    Bind,
    GetSocketName,
    Listen,
    ConfigurationLastError,
    CloseSocket,
    Cleanup,
};

class Recorder final {
public:
    void record(const Event event) noexcept
    {
        if (count_ < events_.size()) {
            events_[count_++] = event;
        }
    }

    [[nodiscard]] Event event(const std::size_t index) const
    {
        require(index < count_, "event index exceeded recorded calls");
        return events_[index];
    }

    [[nodiscard]] std::size_t count() const noexcept { return count_; }

    [[nodiscard]] std::size_t indexOf(const Event searched) const
    {
        for (std::size_t index = 0U; index < count_; ++index) {
            if (events_[index] == searched) {
                return index;
            }
        }
        fail("expected native event was not recorded");
    }

private:
    std::array<Event, 32U> events_{};
    std::size_t count_{};
};

class FakeWinsockApi final : public WinsockApi {
public:
    explicit FakeWinsockApi(std::shared_ptr<Recorder> recorder) noexcept
        : recorder_{std::move(recorder)}
    {
    }

    [[nodiscard]] int startup(
        const WORD requestedVersion,
        WSADATA& data) noexcept override
    {
        recorder_->record(Event::Startup);
        requestedVersion_ = requestedVersion;
        data.wVersion = MAKEWORD(2, 2);
        return 0;
    }

    [[nodiscard]] int cleanup() noexcept override
    {
        recorder_->record(Event::Cleanup);
        ++cleanupCalls;
        return 0;
    }

    [[nodiscard]] SOCKET createSocket(
        const int addressFamily,
        const int socketType,
        const int protocol,
        const DWORD flags) noexcept override
    {
        recorder_->record(Event::CreateSocket);
        ++createCalls;
        addressFamily_ = addressFamily;
        socketType_ = socketType;
        protocol_ = protocol;
        flags_ = flags;
        return socketToReturn;
    }

    [[nodiscard]] int closeSocket(const SOCKET socket) noexcept override
    {
        recorder_->record(Event::CloseSocket);
        ++closeCalls;
        closedSocket = socket;
        return 0;
    }

    [[nodiscard]] int lastError() noexcept override
    {
        recorder_->record(Event::WinsockLastError);
        ++lastErrorCalls;
        return socketError;
    }

    [[nodiscard]] WORD requestedVersion() const noexcept
    {
        return requestedVersion_;
    }
    [[nodiscard]] int addressFamily() const noexcept { return addressFamily_; }
    [[nodiscard]] int socketType() const noexcept { return socketType_; }
    [[nodiscard]] int protocol() const noexcept { return protocol_; }
    [[nodiscard]] DWORD flags() const noexcept { return flags_; }

    SOCKET socketToReturn{static_cast<SOCKET>(404U)};
    int socketError{WSAENOBUFS};
    std::size_t createCalls{};
    std::size_t lastErrorCalls{};
    std::size_t closeCalls{};
    std::size_t cleanupCalls{};
    SOCKET closedSocket{INVALID_SOCKET};

private:
    std::shared_ptr<Recorder> recorder_;
    WORD requestedVersion_{};
    int addressFamily_{};
    int socketType_{};
    int protocol_{};
    DWORD flags_{};
};

struct OptionCall final {
    SOCKET socket{INVALID_SOCKET};
    int level{};
    int name{};
    BOOL value{};
    int length{};
};

class FakeConfigApi final : public ConfigApi {
public:
    explicit FakeConfigApi(std::shared_ptr<Recorder> recorder) noexcept
        : recorder_{std::move(recorder)}
    {
    }

    void setObserved(const Endpoint& endpoint) noexcept
    {
        observedAddress_ = {};
        observedLength = endpoint.nativeAddressLength();
        std::memcpy(
            &observedAddress_,
            endpoint.nativeAddress(),
            static_cast<std::size_t>(endpoint.nativeAddressLength()));
    }

    template <typename SocketAddress>
    void setObserved(const SocketAddress& address) noexcept
    {
        static_assert(sizeof(SocketAddress) <= sizeof(sockaddr_storage));
        observedAddress_ = {};
        observedLength = static_cast<int>(sizeof(SocketAddress));
        std::memcpy(&observedAddress_, &address, sizeof(address));
    }

    [[nodiscard]] int setSocketOption(
        const SOCKET socket,
        const int level,
        const int optionName,
        const char* const optionValue,
        const int optionLength) noexcept override
    {
        const Event event = optionName == SO_EXCLUSIVEADDRUSE
            ? Event::SetExclusiveAddressUse
            : Event::SetIpv6Only;
        recorder_->record(event);
        if (optionCount < options.size()) {
            auto& captured = options[optionCount];
            captured.socket = socket;
            captured.level = level;
            captured.name = optionName;
            captured.length = optionLength;
            if (optionValue != nullptr &&
                optionLength == static_cast<int>(sizeof(BOOL))) {
                std::memcpy(&captured.value, optionValue, sizeof(BOOL));
            }
        }
        ++optionCount;
        if (optionName == failedOption) {
            return SOCKET_ERROR;
        }
        return 0;
    }

    [[nodiscard]] int bindSocket(
        const SOCKET socket,
        const sockaddr* const address,
        const int addressLength) noexcept override
    {
        recorder_->record(Event::Bind);
        ++bindCalls;
        bindSocketValue = socket;
        boundLength = addressLength;
        boundAddress_ = {};
        if (address != nullptr && addressLength > 0 &&
            static_cast<std::size_t>(addressLength) <=
                sizeof(boundAddress_)) {
            std::memcpy(
                &boundAddress_, address, static_cast<std::size_t>(addressLength));
        }
        return bindStatus;
    }

    [[nodiscard]] int getSocketName(
        const SOCKET socket,
        sockaddr* const address,
        int& addressLength) noexcept override
    {
        recorder_->record(Event::GetSocketName);
        ++getSocketNameCalls;
        getSocketNameSocket = socket;
        getSocketNameInputLength = addressLength;
        if (getSocketNameStatus == 0) {
            const auto copyLength = static_cast<std::size_t>(std::max(
                0, std::min(addressLength, observedLength)));
            if (address != nullptr && copyLength > 0U) {
                std::memcpy(address, &observedAddress_, copyLength);
            }
            addressLength = observedLength;
        }
        return getSocketNameStatus;
    }

    [[nodiscard]] int listenSocket(
        const SOCKET socket,
        const int backlog) noexcept override
    {
        recorder_->record(Event::Listen);
        ++listenCalls;
        listenSocketValue = socket;
        listenBacklog = backlog;
        return listenStatus;
    }

    [[nodiscard]] int lastError() noexcept override
    {
        recorder_->record(Event::ConfigurationLastError);
        ++lastErrorCalls;
        return nativeError;
    }

    [[nodiscard]] const sockaddr_storage& boundAddress() const noexcept
    {
        return boundAddress_;
    }

    std::array<OptionCall, 4U> options{};
    std::size_t optionCount{};
    int failedOption{-1};
    int bindStatus{};
    int getSocketNameStatus{};
    int listenStatus{};
    int nativeError{WSAEINVAL};
    int observedLength{};
    std::size_t bindCalls{};
    std::size_t getSocketNameCalls{};
    std::size_t listenCalls{};
    std::size_t lastErrorCalls{};
    SOCKET bindSocketValue{INVALID_SOCKET};
    int boundLength{};
    SOCKET getSocketNameSocket{INVALID_SOCKET};
    int getSocketNameInputLength{};
    SOCKET listenSocketValue{INVALID_SOCKET};
    int listenBacklog{};

private:
    std::shared_ptr<Recorder> recorder_;
    sockaddr_storage observedAddress_{};
    sockaddr_storage boundAddress_{};
};

class Context final {
public:
    Context(const std::string_view host, const std::uint16_t port)
        : endpoint{take(Endpoint::create(host, port))},
          recorder{std::make_shared<Recorder>()},
          winsock{std::make_shared<FakeWinsockApi>(recorder)},
          runtime{take(Runtime::create(winsock))},
          config{std::make_shared<FakeConfigApi>(recorder)}
    {
        config->setObserved(endpoint);
    }

    Endpoint endpoint;
    std::shared_ptr<Recorder> recorder;
    std::shared_ptr<FakeWinsockApi> winsock;
    std::unique_ptr<Runtime> runtime;
    std::shared_ptr<FakeConfigApi> config;
};

void requireClosedBeforeCleanup(Context& context)
{
    require(context.winsock->closeCalls == 1U,
            "configured socket failure did not close exactly once");
    require(context.winsock->cleanupCalls == 0U,
            "socket failure cleaned Winsock while runtime remained owned");
    context.runtime.reset();
    require(context.winsock->cleanupCalls == 1U,
            "runtime did not clean up exactly once after socket failure");
    require(context.recorder->indexOf(Event::CloseSocket) <
                context.recorder->indexOf(Event::Cleanup),
            "WSACleanup occurred before the failed socket was closed");
}

void requireFailure(
    Context& context,
    const Domain::Result<Listener>& result,
    const std::string_view stableCode,
    const bool retryable)
{
    require(!result, "native failure returned a configured listener");
    require(result.error().code == stableCode,
            "native failure used the wrong stable error code");
    require(result.error().retryable == retryable,
            "native failure used the wrong retryability");
    require(result.error().message.find(
                std::to_string(context.config->nativeError)) !=
                std::string::npos,
            "native failure omitted its Winsock error code");
    require(context.config->lastErrorCalls == 1U,
            "native failure did not read exactly one last error");
    requireClosedBeforeCleanup(context);
}

void ipv4SuccessUsesExactConfigurationAndOwnership()
{
    Context context{"127.0.0.1", 17401U};
    auto result = Listener::create(
        *context.runtime, context.endpoint, context.config);
    require(static_cast<bool>(result), "valid IPv4 listener was rejected");
    auto listener = std::move(result).value();

    require(context.winsock->requestedVersion() == MAKEWORD(2, 2),
            "listener runtime did not request Winsock 2.2");
    require(context.winsock->createCalls == 1U,
            "listener did not create exactly one socket");
    require(context.winsock->addressFamily() == AF_INET,
            "IPv4 listener requested the wrong address family");
    require(context.winsock->socketType() == SOCK_STREAM &&
                context.winsock->protocol() == IPPROTO_TCP,
            "listener did not request a TCP stream socket");
    require(context.winsock->flags() ==
                (WSA_FLAG_OVERLAPPED | WSA_FLAG_NO_HANDLE_INHERIT),
            "listener socket omitted required creation flags");

    require(context.config->optionCount == 1U,
            "IPv4 listener set an unexpected socket option");
    const auto& exclusive = context.config->options[0U];
    require(exclusive.socket == context.winsock->socketToReturn,
            "exclusive-address option used the wrong socket");
    require(exclusive.level == SOL_SOCKET &&
                exclusive.name == SO_EXCLUSIVEADDRUSE,
            "listener did not set SO_EXCLUSIVEADDRUSE at SOL_SOCKET");
    require(exclusive.name != SO_REUSEADDR,
            "listener attempted to set SO_REUSEADDR");
    require(exclusive.value == TRUE &&
                exclusive.length == static_cast<int>(sizeof(BOOL)),
            "SO_EXCLUSIVEADDRUSE did not receive one exact TRUE value");

    require(context.config->bindCalls == 1U &&
                context.config->bindSocketValue ==
                    context.winsock->socketToReturn,
            "listener did not bind its owned socket exactly once");
    require(context.config->boundLength ==
                context.endpoint.nativeAddressLength(),
            "bind used the wrong address length");
    require(std::memcmp(
                &context.config->boundAddress(),
                context.endpoint.nativeAddress(),
                static_cast<std::size_t>(
                    context.endpoint.nativeAddressLength())) == 0,
            "bind changed the configured loopback endpoint");
    require(context.config->getSocketNameCalls == 1U,
            "listener did not verify the bound address exactly once");
    require(context.config->getSocketNameInputLength ==
                static_cast<int>(sizeof(sockaddr_storage)),
            "getsockname did not receive full bounded storage");
    require(context.config->listenCalls == 1U &&
                context.config->listenBacklog == 40,
            "listener did not use the exact backlog of 40");
    require(context.config->lastErrorCalls == 0U,
            "successful configuration read a stale native error");
    require(listener.borrowedNativeSocket() ==
                context.winsock->socketToReturn,
            "borrowed listener socket did not match typed ownership");
    require(listener.endpoint() == context.endpoint,
            "listener did not retain its immutable endpoint value");

    constexpr std::array expected{
        Event::Startup,
        Event::CreateSocket,
        Event::SetExclusiveAddressUse,
        Event::Bind,
        Event::GetSocketName,
        Event::Listen,
    };
    require(context.recorder->count() == expected.size(),
            "IPv4 success made an unexpected native call");
    for (std::size_t index = 0U; index < expected.size(); ++index) {
        require(context.recorder->event(index) == expected[index],
                "IPv4 listener used the wrong native call order");
    }

}

void ipv6SuccessAddsOnlyTheExactV6OnlyOption()
{
    Context context{"::1", 17402U};
    auto listener = take(Listener::create(
        *context.runtime, context.endpoint, context.config));

    require(context.winsock->addressFamily() == AF_INET6,
            "IPv6 listener requested the wrong family");
    require(context.config->optionCount == 2U,
            "IPv6 listener did not set exactly two options");
    require(context.config->options[0U].level == SOL_SOCKET &&
                context.config->options[0U].name == SO_EXCLUSIVEADDRUSE,
            "IPv6 listener did not set exclusive address use first");
    require(context.config->options[1U].level == IPPROTO_IPV6 &&
                context.config->options[1U].name == IPV6_V6ONLY,
            "IPv6 listener did not set IPV6_V6ONLY second");
    require(context.config->options[1U].value == TRUE &&
                context.config->options[1U].length ==
                    static_cast<int>(sizeof(BOOL)),
            "IPV6_V6ONLY did not receive one exact TRUE value");
    for (std::size_t index = 0U; index < context.config->optionCount; ++index) {
        require(context.config->options[index].name != SO_REUSEADDR,
                "IPv6 listener attempted to set SO_REUSEADDR");
    }
    require(context.recorder->indexOf(Event::SetExclusiveAddressUse) <
                context.recorder->indexOf(Event::SetIpv6Only) &&
                context.recorder->indexOf(Event::SetIpv6Only) <
                    context.recorder->indexOf(Event::Bind),
            "IPv6 socket options were set in the wrong order");

    Listener moved{std::move(listener)};
    require(listener.borrowedNativeSocket() == INVALID_SOCKET,
            "listener move source retained native ownership");
    require(moved.borrowedNativeSocket() ==
                context.winsock->socketToReturn,
            "listener move destination lost native ownership");
    require(moved.endpoint() == context.endpoint,
            "listener move changed its endpoint value");
    require(context.winsock->closeCalls == 0U,
            "listener move closed native ownership");

    context.runtime.reset();
    require(context.winsock->cleanupCalls == 0U,
            "runtime cleaned Winsock while the listener still owned a socket");
    {
        auto scoped = std::move(moved);
        require(scoped.borrowedNativeSocket() ==
                    context.winsock->socketToReturn,
                "second listener move lost ownership");
    }
    require(context.winsock->closeCalls == 1U,
            "listener destruction did not close exactly once");
    require(context.winsock->cleanupCalls == 1U,
            "listener success did not balance Winsock startup");
    require(context.recorder->indexOf(Event::CloseSocket) <
                context.recorder->indexOf(Event::Cleanup),
            "successful listener cleaned Winsock before socket close");
}

void nullConfigurationApiIsRejectedBeforeSocketCreation()
{
    Context context{"127.0.0.1", 17403U};
    const auto result = Listener::create(
        *context.runtime,
        context.endpoint,
        std::shared_ptr<ConfigApi>{});
    require(!result, "listener accepted a null configuration API");
    require(result.error().code == Domain::ErrorCodes::InvalidRequest,
            "null configuration API used the wrong error code");
    require(context.winsock->createCalls == 0U,
            "null configuration API created a socket");
    require(context.config->optionCount == 0U,
            "null configuration API reached socket configuration");
}

void socketCreationFailureIsPropagatedWithoutClosingInvalidSocket()
{
    Context context{"127.0.0.1", 17404U};
    context.winsock->socketToReturn = INVALID_SOCKET;
    context.winsock->socketError = WSAENOBUFS;
    const auto result = Listener::create(
        *context.runtime, context.endpoint, context.config);
    require(!result, "failed socket creation returned a listener");
    require(result.error().code == Domain::ErrorCodes::LimitExceeded &&
                result.error().retryable,
            "socket-creation failure was not propagated intact");
    require(context.winsock->lastErrorCalls == 1U,
            "socket-creation failure did not read one native error");
    require(context.winsock->closeCalls == 0U,
            "invalid socket was passed to closesocket");
    require(context.config->optionCount == 0U,
            "failed socket creation reached configuration");
}

void optionFailureMappingsStopAndCloseExactlyOnce()
{
    struct Case final {
        int nativeCode;
        std::string_view stableCode;
        bool retryable;
    };
    constexpr std::array cases{
        Case{WSAENOBUFS, Domain::ErrorCodes::LimitExceeded, true},
        Case{WSANOTINITIALISED,
             Domain::ErrorCodes::HostCapabilityUnavailable, true},
        Case{WSAENETDOWN,
             Domain::ErrorCodes::HostCapabilityUnavailable, true},
        Case{WSASYSNOTREADY,
             Domain::ErrorCodes::HostCapabilityUnavailable, true},
        Case{WSAEAFNOSUPPORT,
             Domain::ErrorCodes::HostCapabilityUnavailable, false},
        Case{WSAENOPROTOOPT,
             Domain::ErrorCodes::HostCapabilityUnavailable, false},
        Case{WSAEPROTONOSUPPORT,
             Domain::ErrorCodes::HostCapabilityUnavailable, false},
        Case{WSAEINVAL, Domain::ErrorCodes::InternalFailure, false},
    };

    for (const auto& testCase : cases) {
        Context context{"127.0.0.1", 17405U};
        context.config->failedOption = SO_EXCLUSIVEADDRUSE;
        context.config->nativeError = testCase.nativeCode;
        const auto result = Listener::create(
            *context.runtime, context.endpoint, context.config);
        requireFailure(
            context, result, testCase.stableCode, testCase.retryable);
        require(context.config->optionCount == 1U,
                "exclusive-option failure set another option");
        require(context.config->bindCalls == 0U &&
                    context.config->getSocketNameCalls == 0U &&
                    context.config->listenCalls == 0U,
                "exclusive-option failure reached a later operation");
    }

    Context ipv6{"::1", 17406U};
    ipv6.config->failedOption = IPV6_V6ONLY;
    ipv6.config->nativeError = WSAENOPROTOOPT;
    const auto result = Listener::create(
        *ipv6.runtime, ipv6.endpoint, ipv6.config);
    requireFailure(
        ipv6,
        result,
        Domain::ErrorCodes::HostCapabilityUnavailable,
        false);
    require(ipv6.config->optionCount == 2U,
            "IPv6-only failure used the wrong option count");
    require(ipv6.config->bindCalls == 0U,
            "IPv6-only failure reached bind");
}

void bindFailureMappingsAreTypedIncludingAddressConflict()
{
    struct Case final {
        int nativeCode;
        std::string_view stableCode;
        bool retryable;
    };
    constexpr std::array cases{
        Case{WSAEADDRINUSE, Domain::ErrorCodes::Conflict, true},
        Case{WSAEACCES, Domain::ErrorCodes::Unauthorized, false},
        Case{WSAENOBUFS, Domain::ErrorCodes::LimitExceeded, true},
        Case{WSANOTINITIALISED,
             Domain::ErrorCodes::HostCapabilityUnavailable, true},
        Case{WSAENETDOWN,
             Domain::ErrorCodes::HostCapabilityUnavailable, true},
        Case{WSASYSNOTREADY,
             Domain::ErrorCodes::HostCapabilityUnavailable, true},
        Case{WSAEADDRNOTAVAIL,
             Domain::ErrorCodes::HostCapabilityUnavailable, false},
        Case{WSAEAFNOSUPPORT,
             Domain::ErrorCodes::HostCapabilityUnavailable, false},
        Case{WSAEINVAL, Domain::ErrorCodes::InternalFailure, false},
    };

    for (const auto& testCase : cases) {
        Context context{"127.0.0.1", 17407U};
        context.config->bindStatus = SOCKET_ERROR;
        context.config->nativeError = testCase.nativeCode;
        const auto result = Listener::create(
            *context.runtime, context.endpoint, context.config);
        requireFailure(
            context, result, testCase.stableCode, testCase.retryable);
        require(context.config->bindCalls == 1U,
                "bind failure did not make exactly one bind call");
        require(context.config->getSocketNameCalls == 0U &&
                    context.config->listenCalls == 0U,
                "bind failure reached a later operation");
    }
}

void socketNameFailureMappingsStopBeforeValidationAndListen()
{
    struct Case final {
        int nativeCode;
        std::string_view stableCode;
        bool retryable;
    };
    constexpr std::array cases{
        Case{WSAENOBUFS, Domain::ErrorCodes::LimitExceeded, true},
        Case{WSANOTINITIALISED,
             Domain::ErrorCodes::HostCapabilityUnavailable, true},
        Case{WSAENETDOWN,
             Domain::ErrorCodes::HostCapabilityUnavailable, true},
        Case{WSASYSNOTREADY,
             Domain::ErrorCodes::HostCapabilityUnavailable, true},
        Case{WSAEFAULT, Domain::ErrorCodes::InternalFailure, false},
    };

    for (const auto& testCase : cases) {
        Context context{"127.0.0.1", 17408U};
        context.config->getSocketNameStatus = SOCKET_ERROR;
        context.config->nativeError = testCase.nativeCode;
        const auto result = Listener::create(
            *context.runtime, context.endpoint, context.config);
        requireFailure(
            context, result, testCase.stableCode, testCase.retryable);
        require(context.config->getSocketNameCalls == 1U,
                "getsockname failure did not make exactly one call");
        require(context.config->listenCalls == 0U,
                "getsockname failure reached listen");
    }
}

void boundAddressValidationRejectsNativeIntegrityDrift()
{
    {
        Context context{"127.0.0.1", 17409U};
        auto mismatched = *reinterpret_cast<const sockaddr_in*>(
            context.endpoint.nativeAddress());
        auto* const portBytes =
            reinterpret_cast<unsigned char*>(&mismatched.sin_port);
        portBytes[1U] ^= 1U;
        context.config->setObserved(mismatched);
        const auto result = Listener::create(
            *context.runtime, context.endpoint, context.config);
        require(!result, "mismatched bound port was accepted");
        require(result.error().code == Domain::ErrorCodes::IntegrityFailure,
                "bound-port mismatch used the wrong stable code");
        require(context.config->lastErrorCalls == 0U,
                "integrity rejection read a stale native error");
        require(context.config->listenCalls == 0U,
                "integrity rejection reached listen");
        requireClosedBeforeCleanup(context);
    }
    {
        Context context{"127.0.0.1", 17410U};
        context.config->observedLength =
            static_cast<int>(sizeof(sockaddr_in)) - 1;
        const auto result = Listener::create(
            *context.runtime, context.endpoint, context.config);
        require(!result, "short getsockname result was accepted");
        require(result.error().code == Domain::ErrorCodes::IntegrityFailure,
                "short bound address used the wrong stable code");
        require(context.config->listenCalls == 0U,
                "short bound address reached listen");
        requireClosedBeforeCleanup(context);
    }
    {
        Context context{"::1", 17411U};
        auto mismatched = *reinterpret_cast<const sockaddr_in6*>(
            context.endpoint.nativeAddress());
        mismatched.sin6_scope_id = 1U;
        context.config->setObserved(mismatched);
        const auto result = Listener::create(
            *context.runtime, context.endpoint, context.config);
        require(!result, "noncanonical IPv6 bound metadata was accepted");
        require(result.error().code == Domain::ErrorCodes::IntegrityFailure,
                "IPv6 integrity drift used the wrong stable code");
        require(context.config->listenCalls == 0U,
                "IPv6 integrity rejection reached listen");
        requireClosedBeforeCleanup(context);
    }
}

void listenFailureMappingsCloseAfterExactBacklogAttempt()
{
    struct Case final {
        int nativeCode;
        std::string_view stableCode;
        bool retryable;
    };
    constexpr std::array cases{
        Case{WSAEADDRINUSE, Domain::ErrorCodes::Conflict, true},
        Case{WSAEMFILE, Domain::ErrorCodes::LimitExceeded, true},
        Case{WSAENOBUFS, Domain::ErrorCodes::LimitExceeded, true},
        Case{WSANOTINITIALISED,
             Domain::ErrorCodes::HostCapabilityUnavailable, true},
        Case{WSAENETDOWN,
             Domain::ErrorCodes::HostCapabilityUnavailable, true},
        Case{WSASYSNOTREADY,
             Domain::ErrorCodes::HostCapabilityUnavailable, true},
        Case{WSAEOPNOTSUPP,
             Domain::ErrorCodes::HostCapabilityUnavailable, false},
        Case{WSAEINVAL, Domain::ErrorCodes::InternalFailure, false},
    };

    for (const auto& testCase : cases) {
        Context context{"127.0.0.1", 17412U};
        context.config->listenStatus = SOCKET_ERROR;
        context.config->nativeError = testCase.nativeCode;
        const auto result = Listener::create(
            *context.runtime, context.endpoint, context.config);
        requireFailure(
            context, result, testCase.stableCode, testCase.retryable);
        require(context.config->listenCalls == 1U &&
                    context.config->listenBacklog == Listener::ListenBacklog,
                "listen failure did not attempt the exact backlog once");
    }
}

} // namespace

int main()
{
    try {
        ipv4SuccessUsesExactConfigurationAndOwnership();
        ipv6SuccessAddsOnlyTheExactV6OnlyOption();
        nullConfigurationApiIsRejectedBeforeSocketCreation();
        socketCreationFailureIsPropagatedWithoutClosingInvalidSocket();
        optionFailureMappingsStopAndCloseExactlyOnce();
        bindFailureMappingsAreTypedIncludingAddressConflict();
        socketNameFailureMappingsStopBeforeValidationAndListen();
        boundAddressValidationRejectsNativeIntegrityDrift();
        listenFailureMappingsCloseAfterExactBacklogAttempt();
        std::cout << "Dashboard listening socket tests passed: "
                  << assertionCount << " assertions\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Dashboard listening socket tests failed after "
                  << assertionCount << " assertions: " << error.what()
                  << '\n';
        return 1;
    } catch (...) {
        std::cerr << "Dashboard listening socket tests failed after "
                  << assertionCount << " assertions: unknown exception\n";
        return 1;
    }
}
