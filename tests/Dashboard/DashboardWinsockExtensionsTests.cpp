#include "Infrastructure/Windows/Detail/DashboardWinsockExtensions.h"

#include <MSWSock.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <iostream>
#include <limits>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

namespace {

namespace Domain = ForgeConductor::Domain;
namespace Detail = ForgeConductor::Infrastructure::Windows::Detail;

using AcceptedAddress = Detail::DashboardAcceptedSocketAddress;
using AcceptedAddresses = Detail::DashboardAcceptedAddresses;
using Api = Detail::IDashboardWinsockExtensionApi;
using Disposition = Detail::DashboardAcceptIssueDisposition;
using Extensions = Detail::DashboardWinsockExtensions;
using SystemApi = Detail::DashboardWinsockExtensionSystemApi;

static_assert(std::is_abstract_v<Api>);
static_assert(std::is_final_v<SystemApi>);
static_assert(std::is_final_v<AcceptedAddress>);
static_assert(std::is_final_v<AcceptedAddresses>);
static_assert(std::is_final_v<Extensions>);
static_assert(!std::is_copy_constructible_v<Extensions>);
static_assert(!std::is_move_constructible_v<Extensions>);
static_assert(std::is_nothrow_destructible_v<Extensions>);
static_assert(Extensions::RequiredReceiveDataLength == 0U);
static_assert(Extensions::RequiredAddressPadding == 16U);
static_assert(
    Extensions::Ipv4AddressRegionLength == sizeof(sockaddr_in) + 16U);
static_assert(
    Extensions::Ipv6AddressRegionLength == sizeof(sockaddr_in6) + 16U);
static_assert(noexcept(Extensions::discover(INVALID_SOCKET)));
static_assert(noexcept(Extensions::discover(INVALID_SOCKET, {})));

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

template <typename Value>
void requireError(
    const Domain::Result<Value>& result,
    const std::string_view code,
    const bool retryable,
    const std::string_view context)
{
    require(!result, context);
    require(result.error().code == code, "failure used the wrong stable code");
    require(result.error().retryable == retryable,
            "failure used the wrong retryability");
}

[[nodiscard]] bool sameGuid(const GUID& left, const GUID& right) noexcept
{
    return std::memcmp(&left, &right, sizeof(GUID)) == 0;
}

enum class ExtractMode : unsigned char {
    Valid,
    NullLocal,
    NullRemote,
    ShortLocal,
    LongRemote,
    LocalInRemoteRegion,
    RemoteInLocalRegion,
    LocalBeforeBuffer,
    RemoteAtBufferEnd,
    WrongLocalFamily,
    WrongRemoteFamily,
};

struct NativeCallState final {
    BOOL acceptResult{TRUE};
    DWORD bytesReceivedToWrite{};
    std::size_t acceptCalls{};
    SOCKET listenerSocket{INVALID_SOCKET};
    SOCKET acceptedSocket{INVALID_SOCKET};
    void* acceptBuffer{};
    DWORD receiveLength{};
    DWORD localLength{};
    DWORD remoteLength{};
    DWORD* bytesReceived{};
    OVERLAPPED* operation{};

    std::size_t extractCalls{};
    void* extractBuffer{};
    DWORD extractReceiveLength{};
    DWORD extractLocalLength{};
    DWORD extractRemoteLength{};
    int addressFamily{AF_INET};
    ExtractMode extractMode{ExtractMode::Valid};
};

NativeCallState* activeNativeState{};

void populateAddress(
    void* const destination,
    const int addressFamily,
    const bool local) noexcept
{
    if (addressFamily == AF_INET) {
        sockaddr_in value{};
        value.sin_family = AF_INET;
        value.sin_port = local ? static_cast<unsigned short>(0x1111U)
                               : static_cast<unsigned short>(0x2222U);
        const std::array<unsigned char, 4U> bytes{
            127U, 0U, 0U, static_cast<unsigned char>(local ? 1U : 2U)};
        std::memcpy(&value.sin_addr, bytes.data(), bytes.size());
        std::memcpy(destination, &value, sizeof(value));
        return;
    }

    sockaddr_in6 value{};
    value.sin6_family = AF_INET6;
    value.sin6_port = local ? static_cast<unsigned short>(0x3333U)
                            : static_cast<unsigned short>(0x4444U);
    auto* const bytes = reinterpret_cast<unsigned char*>(&value.sin6_addr);
    bytes[15U] = static_cast<unsigned char>(local ? 1U : 2U);
    std::memcpy(destination, &value, sizeof(value));
}

BOOL PASCAL fakeAcceptEx(
    const SOCKET listenerSocket,
    const SOCKET acceptedSocket,
    PVOID const outputBuffer,
    const DWORD receiveDataLength,
    const DWORD localAddressLength,
    const DWORD remoteAddressLength,
    LPDWORD const bytesReceived,
    LPOVERLAPPED const operation)
{
    if (activeNativeState == nullptr) {
        return FALSE;
    }
    auto& state = *activeNativeState;
    ++state.acceptCalls;
    state.listenerSocket = listenerSocket;
    state.acceptedSocket = acceptedSocket;
    state.acceptBuffer = outputBuffer;
    state.receiveLength = receiveDataLength;
    state.localLength = localAddressLength;
    state.remoteLength = remoteAddressLength;
    state.bytesReceived = bytesReceived;
    state.operation = operation;
    if (bytesReceived != nullptr) {
        *bytesReceived = state.bytesReceivedToWrite;
    }
    return state.acceptResult;
}

VOID PASCAL fakeGetAcceptExSockaddrs(
    PVOID const outputBuffer,
    const DWORD receiveDataLength,
    const DWORD localAddressLength,
    const DWORD remoteAddressLength,
    LPSOCKADDR* const localAddress,
    LPINT const localAddressLengthActual,
    LPSOCKADDR* const remoteAddress,
    LPINT const remoteAddressLengthActual)
{
    if (activeNativeState == nullptr) {
        return;
    }
    auto& state = *activeNativeState;
    ++state.extractCalls;
    state.extractBuffer = outputBuffer;
    state.extractReceiveLength = receiveDataLength;
    state.extractLocalLength = localAddressLength;
    state.extractRemoteLength = remoteAddressLength;

    auto* const bytes = static_cast<std::byte*>(outputBuffer);
    auto* local = reinterpret_cast<sockaddr*>(bytes + 16U);
    auto* remote = reinterpret_cast<sockaddr*>(
        bytes + receiveDataLength + localAddressLength + 16U);
    const int expectedLength = state.addressFamily == AF_INET
        ? static_cast<int>(sizeof(sockaddr_in))
        : static_cast<int>(sizeof(sockaddr_in6));
    populateAddress(local, state.addressFamily, true);
    populateAddress(remote, state.addressFamily, false);
    int localActual = expectedLength;
    int remoteActual = expectedLength;

    switch (state.extractMode) {
    case ExtractMode::Valid:
        break;
    case ExtractMode::NullLocal:
        local = nullptr;
        break;
    case ExtractMode::NullRemote:
        remote = nullptr;
        break;
    case ExtractMode::ShortLocal:
        --localActual;
        break;
    case ExtractMode::LongRemote:
        ++remoteActual;
        break;
    case ExtractMode::LocalInRemoteRegion:
        local = reinterpret_cast<sockaddr*>(
            bytes + receiveDataLength + localAddressLength + 16U);
        break;
    case ExtractMode::RemoteInLocalRegion:
        remote = reinterpret_cast<sockaddr*>(bytes + 16U);
        break;
    case ExtractMode::LocalBeforeBuffer:
        local = reinterpret_cast<sockaddr*>(
            reinterpret_cast<std::uintptr_t>(outputBuffer) - 1U);
        break;
    case ExtractMode::RemoteAtBufferEnd:
        remote = reinterpret_cast<sockaddr*>(
            bytes + receiveDataLength + localAddressLength +
            remoteAddressLength);
        break;
    case ExtractMode::WrongLocalFamily: {
        const ADDRESS_FAMILY wrongFamily =
            state.addressFamily == AF_INET ? AF_INET6 : AF_INET;
        std::memcpy(local, &wrongFamily, sizeof(wrongFamily));
        break;
    }
    case ExtractMode::WrongRemoteFamily: {
        const ADDRESS_FAMILY wrongFamily =
            state.addressFamily == AF_INET ? AF_INET6 : AF_INET;
        std::memcpy(remote, &wrongFamily, sizeof(wrongFamily));
        break;
    }
    }

    *localAddress = local;
    *localAddressLengthActual = localActual;
    *remoteAddress = remote;
    *remoteAddressLengthActual = remoteActual;
}

struct QueryConfiguration final {
    int status{};
    std::optional<DWORD> bytesReturned;
    bool returnNullPointer{};
};

struct QueryObservation final {
    SOCKET socket{INVALID_SOCKET};
    DWORD controlCode{};
    GUID identifier{};
    DWORD inputLength{};
    DWORD outputLength{};
};

class FakeApi final : public Api {
public:
    [[nodiscard]] int ioctl(
        const SOCKET socket,
        const DWORD controlCode,
        const void* const inputBuffer,
        const DWORD inputBufferLength,
        void* const outputBuffer,
        const DWORD outputBufferLength,
        DWORD& bytesReturned) noexcept override
    {
        const std::size_t index = queryCalls++;
        if (index >= observations.size() || inputBuffer == nullptr) {
            return SOCKET_ERROR;
        }
        auto& observed = observations[index];
        observed.socket = socket;
        observed.controlCode = controlCode;
        observed.inputLength = inputBufferLength;
        observed.outputLength = outputBufferLength;
        if (inputBufferLength == sizeof(GUID)) {
            std::memcpy(&observed.identifier, inputBuffer, sizeof(GUID));
        }

        const auto& configured = configurations[index];
        const bool isAccept = sameGuid(observed.identifier, WSAID_ACCEPTEX);
        const DWORD naturalSize = isAccept
            ? static_cast<DWORD>(sizeof(LPFN_ACCEPTEX))
            : static_cast<DWORD>(sizeof(LPFN_GETACCEPTEXSOCKADDRS));
        bytesReturned = configured.bytesReturned.value_or(naturalSize);
        if (configured.status != 0) {
            return configured.status;
        }
        if (isAccept) {
            const LPFN_ACCEPTEX pointer = configured.returnNullPointer
                ? nullptr
                : &fakeAcceptEx;
            std::memcpy(outputBuffer, &pointer, sizeof(pointer));
        } else {
            const LPFN_GETACCEPTEXSOCKADDRS pointer =
                configured.returnNullPointer
                ? nullptr
                : &fakeGetAcceptExSockaddrs;
            std::memcpy(outputBuffer, &pointer, sizeof(pointer));
        }
        return 0;
    }

    [[nodiscard]] int lastError() noexcept override
    {
        ++lastErrorCalls;
        return nativeError;
    }

    std::array<QueryConfiguration, 2U> configurations{};
    std::array<QueryObservation, 2U> observations{};
    std::size_t queryCalls{};
    std::size_t lastErrorCalls{};
    int nativeError{WSAEFAULT};
};

[[nodiscard]] std::unique_ptr<Extensions> discovered(
    const std::shared_ptr<FakeApi>& api)
{
    return take(Extensions::discover(static_cast<SOCKET>(71U), api));
}

class ActiveNativeScope final {
public:
    explicit ActiveNativeScope(NativeCallState& state) noexcept
    {
        activeNativeState = &state;
    }
    ~ActiveNativeScope() noexcept { activeNativeState = nullptr; }

    ActiveNativeScope(const ActiveNativeScope&) = delete;
    ActiveNativeScope& operator=(const ActiveNativeScope&) = delete;
};

void discoveryUsesBothExactWinsockIdentifiers()
{
    auto api = std::make_shared<FakeApi>();
    auto owner = discovered(api);
    require(owner != nullptr, "extension discovery returned a null owner");
    require(api->queryCalls == 2U,
            "extension discovery did not make exactly two queries");

    const GUID acceptIdentifier = WSAID_ACCEPTEX;
    const GUID addressesIdentifier = WSAID_GETACCEPTEXSOCKADDRS;
    require(sameGuid(api->observations[0U].identifier, acceptIdentifier),
            "first discovery query did not use WSAID_ACCEPTEX");
    require(sameGuid(api->observations[1U].identifier, addressesIdentifier),
            "second discovery query did not use WSAID_GETACCEPTEXSOCKADDRS");
    for (std::size_t index{}; index < api->queryCalls; ++index) {
        const auto& observed = api->observations[index];
        require(observed.socket == static_cast<SOCKET>(71U),
                "discovery changed the socket");
        require(observed.controlCode ==
                    SIO_GET_EXTENSION_FUNCTION_POINTER,
                "discovery used the wrong WSAIoctl control code");
        require(observed.inputLength == sizeof(GUID),
                "discovery supplied the wrong GUID byte count");
        require(observed.outputLength == sizeof(void*),
                "discovery supplied the wrong function-pointer byte count");
    }
    require(api->lastErrorCalls == 0U,
            "successful discovery queried a stale last error");
}

void discoveryRejectsInvalidDependenciesWithoutNativeCalls()
{
    auto api = std::make_shared<FakeApi>();
    requireError(
        Extensions::discover(INVALID_SOCKET, api),
        Domain::ErrorCodes::InvalidRequest,
        false,
        "discovery accepted INVALID_SOCKET");
    require(api->queryCalls == 0U,
            "invalid-socket discovery made a native query");

    requireError(
        Extensions::discover(
            static_cast<SOCKET>(72U), std::shared_ptr<Api>{}),
        Domain::ErrorCodes::InvalidRequest,
        false,
        "discovery accepted a null API");
}

void discoveryFailuresAreTypedForEitherExtension()
{
    struct Case final {
        int nativeCode;
        std::string_view stableCode;
        bool retryable;
    };
    constexpr std::array cases{
        Case{WSANOTINITIALISED,
             Domain::ErrorCodes::HostCapabilityUnavailable, true},
        Case{WSAENETDOWN,
             Domain::ErrorCodes::HostCapabilityUnavailable, true},
        Case{WSAENOBUFS, Domain::ErrorCodes::LimitExceeded, true},
        Case{WSA_OPERATION_ABORTED, Domain::ErrorCodes::Cancelled, false},
        Case{WSAENOPROTOOPT,
             Domain::ErrorCodes::HostCapabilityUnavailable, false},
        Case{WSAEOPNOTSUPP,
             Domain::ErrorCodes::HostCapabilityUnavailable, false},
        Case{WSAEFAULT, Domain::ErrorCodes::InternalFailure, false},
    };

    for (std::size_t failingQuery{}; failingQuery < 2U; ++failingQuery) {
        for (const auto& testCase : cases) {
            auto api = std::make_shared<FakeApi>();
            api->configurations[failingQuery].status = SOCKET_ERROR;
            api->nativeError = testCase.nativeCode;
            const auto result = Extensions::discover(
                static_cast<SOCKET>(73U), api);
            requireError(result, testCase.stableCode, testCase.retryable,
                         "a failed extension query returned an owner");
            require(api->queryCalls == failingQuery + 1U,
                    "discovery continued after a failed query");
            require(api->lastErrorCalls == 1U,
                    "failed discovery did not read last error exactly once");
            require(result.error().message.find(
                        std::to_string(testCase.nativeCode)) !=
                        std::string::npos,
                    "discovery error omitted the native error code");
        }
    }

    auto api = std::make_shared<FakeApi>();
    api->configurations[0U].status = 1;
    requireError(
        Extensions::discover(static_cast<SOCKET>(74U), api),
        Domain::ErrorCodes::InternalFailure,
        false,
        "discovery accepted a nonstandard ioctl status");
    require(api->lastErrorCalls == 0U,
            "nonstandard status queried an unrelated last error");
}

void discoveryRejectsNullTruncatedAndOversizedPointers()
{
    const DWORD pointerSize = static_cast<DWORD>(sizeof(void*));
    constexpr std::array malformedSizes{
        0U,
        static_cast<unsigned int>(sizeof(void*) - 1U),
        static_cast<unsigned int>(sizeof(void*) + 1U),
        (std::numeric_limits<unsigned int>::max)(),
    };
    for (std::size_t failingQuery{}; failingQuery < 2U; ++failingQuery) {
        for (const DWORD returned : malformedSizes) {
            auto api = std::make_shared<FakeApi>();
            api->configurations[failingQuery].bytesReturned = returned;
            const auto result = Extensions::discover(
                static_cast<SOCKET>(75U), api);
            requireError(
                result, Domain::ErrorCodes::IntegrityFailure, false,
                "discovery accepted a non-exact pointer byte count");
            require(api->queryCalls == failingQuery + 1U,
                    "malformed discovery continued to another query");
            require(api->lastErrorCalls == 0U,
                    "malformed successful query read last error");
        }

        auto api = std::make_shared<FakeApi>();
        api->configurations[failingQuery].bytesReturned = pointerSize;
        api->configurations[failingQuery].returnNullPointer = true;
        requireError(
            Extensions::discover(static_cast<SOCKET>(76U), api),
            Domain::ErrorCodes::IntegrityFailure,
            false,
            "discovery accepted a null extension pointer");
        require(api->queryCalls == failingQuery + 1U,
                "null-pointer discovery continued to another query");
    }
}

void issueAcceptRejectsEveryInvalidFramingInputBeforeNativeCall()
{
    auto api = std::make_shared<FakeApi>();
    auto owner = discovered(api);
    NativeCallState native{};
    ActiveNativeScope active{native};
    std::array<std::byte, Extensions::Ipv6AddressRegionLength * 2U> buffer{};
    OVERLAPPED operation{};

    const auto reject = [&](
                            const SOCKET listener,
                            const SOCKET accepted,
                            const int family,
                            void* const storage,
                            const DWORD storageLength,
                            const DWORD receiveLength,
                            const DWORD localLength,
                            const DWORD remoteLength) {
        const std::size_t before = native.acceptCalls;
        requireError(
            owner->issueAccept(
                listener,
                accepted,
                family,
                storage,
                storageLength,
                receiveLength,
                localLength,
                remoteLength,
                operation),
            Domain::ErrorCodes::InvalidRequest,
            false,
            "issueAccept accepted invalid framing");
        require(native.acceptCalls == before,
                "invalid issueAccept input reached AcceptEx");
    };

    constexpr SOCKET Listener = static_cast<SOCKET>(81U);
    constexpr SOCKET Accepted = static_cast<SOCKET>(82U);
    constexpr DWORD Ipv4 = Extensions::Ipv4AddressRegionLength;
    reject(INVALID_SOCKET, Accepted, AF_INET, buffer.data(), Ipv4 * 2U,
           0U, Ipv4, Ipv4);
    reject(Listener, INVALID_SOCKET, AF_INET, buffer.data(), Ipv4 * 2U,
           0U, Ipv4, Ipv4);
    reject(Listener, Accepted, AF_UNSPEC, buffer.data(), Ipv4 * 2U,
           0U, Ipv4, Ipv4);
    reject(Listener, Accepted, AF_INET, nullptr, Ipv4 * 2U,
           0U, Ipv4, Ipv4);
    reject(Listener, Accepted, AF_INET, buffer.data(), Ipv4 * 2U,
           1U, Ipv4, Ipv4);
    reject(Listener, Accepted, AF_INET, buffer.data(), Ipv4 * 2U,
           0U, Ipv4 - 1U, Ipv4);
    reject(Listener, Accepted, AF_INET, buffer.data(), Ipv4 * 2U,
           0U, Ipv4, Ipv4 + 1U);
    reject(Listener, Accepted, AF_INET, buffer.data(), Ipv4 * 2U - 1U,
           0U, Ipv4, Ipv4);

    constexpr DWORD Ipv6 = Extensions::Ipv6AddressRegionLength;
    reject(Listener, Accepted, AF_INET6, buffer.data(), Ipv6 * 2U,
           0U, Ipv4, Ipv6);
    reject(Listener, Accepted, AF_INET6, buffer.data(), Ipv6 * 2U,
           0U, Ipv6, Ipv4);
    require(api->lastErrorCalls == 0U,
            "input rejection queried a stale last error");
}

void issueAcceptDistinguishesSynchronousPendingAndTypedFailures()
{
    auto api = std::make_shared<FakeApi>();
    auto owner = discovered(api);
    NativeCallState native{};
    ActiveNativeScope active{native};
    constexpr DWORD Region = Extensions::Ipv4AddressRegionLength;
    std::array<std::byte, Region * 2U> buffer{};
    OVERLAPPED operation{};

    const auto synchronous = owner->issueAccept(
        static_cast<SOCKET>(83U),
        static_cast<SOCKET>(84U),
        AF_INET,
        buffer.data(),
        static_cast<DWORD>(buffer.size()),
        0U,
        Region,
        Region,
        operation);
    require(static_cast<bool>(synchronous),
            "synchronous AcceptEx issue failed");
    require(synchronous.value() == Disposition::CompletedSynchronously,
            "synchronous AcceptEx issue returned pending");
    require(native.acceptCalls == 1U,
            "synchronous issue did not call AcceptEx exactly once");
    require(native.listenerSocket == static_cast<SOCKET>(83U) &&
                native.acceptedSocket == static_cast<SOCKET>(84U),
            "issue changed a socket argument");
    require(native.acceptBuffer == buffer.data(),
            "issue changed the output buffer");
    require(native.receiveLength == 0U,
            "issue allowed AcceptEx to wait for payload");
    require(native.localLength == Region && native.remoteLength == Region,
            "issue changed exact address-region lengths");
    require(native.bytesReceived != nullptr,
            "issue omitted AcceptEx byte storage");
    require(native.operation == &operation,
            "issue changed OVERLAPPED ownership");
    require(api->lastErrorCalls == 0U,
            "synchronous success queried a stale last error");

    constexpr DWORD Ipv6Region = Extensions::Ipv6AddressRegionLength;
    std::array<std::byte, Ipv6Region * 2U> ipv6Buffer{};
    const auto ipv6Synchronous = owner->issueAccept(
        static_cast<SOCKET>(85U),
        static_cast<SOCKET>(86U),
        AF_INET6,
        ipv6Buffer.data(),
        static_cast<DWORD>(ipv6Buffer.size()),
        0U,
        Ipv6Region,
        Ipv6Region,
        operation);
    require(static_cast<bool>(ipv6Synchronous),
            "exact IPv6 AcceptEx framing was rejected");
    require(ipv6Synchronous.value() ==
                Disposition::CompletedSynchronously,
            "synchronous IPv6 issue returned pending");
    require(native.receiveLength == 0U &&
                native.localLength == Ipv6Region &&
                native.remoteLength == Ipv6Region,
            "IPv6 issue changed zero-receive or sockaddr-plus-16 framing");

    native.acceptResult = FALSE;
    api->nativeError = ERROR_IO_PENDING;
    const auto pending = owner->issueAccept(
        static_cast<SOCKET>(83U),
        static_cast<SOCKET>(84U),
        AF_INET,
        buffer.data(),
        static_cast<DWORD>(buffer.size()),
        0U,
        Region,
        Region,
        operation);
    require(static_cast<bool>(pending), "pending AcceptEx issue failed");
    require(pending.value() == Disposition::Pending,
            "ERROR_IO_PENDING was not treated as issued");
    require(api->lastErrorCalls == 1U,
            "pending issue did not read last error exactly once");

    struct Case final {
        int nativeCode;
        std::string_view stableCode;
        bool retryable;
    };
    constexpr std::array cases{
        Case{WSA_OPERATION_ABORTED, Domain::ErrorCodes::Cancelled, false},
        Case{WSAECONNRESET, Domain::ErrorCodes::TransportClosed, true},
        Case{WSAENOTSOCK, Domain::ErrorCodes::TransportClosed, false},
        Case{WSAENOBUFS, Domain::ErrorCodes::LimitExceeded, true},
        Case{WSAEMFILE, Domain::ErrorCodes::LimitExceeded, true},
        Case{WSAENETDOWN,
             Domain::ErrorCodes::HostCapabilityUnavailable, true},
        Case{WSAEWOULDBLOCK, Domain::ErrorCodes::Conflict, true},
        Case{WSAEFAULT, Domain::ErrorCodes::InternalFailure, false},
    };
    for (const auto& testCase : cases) {
        const std::size_t callsBefore = native.acceptCalls;
        const std::size_t errorsBefore = api->lastErrorCalls;
        api->nativeError = testCase.nativeCode;
        const auto result = owner->issueAccept(
            static_cast<SOCKET>(83U),
            static_cast<SOCKET>(84U),
            AF_INET,
            buffer.data(),
            static_cast<DWORD>(buffer.size()),
            0U,
            Region,
            Region,
            operation);
        requireError(result, testCase.stableCode, testCase.retryable,
                     "AcceptEx issue failure returned success");
        require(native.acceptCalls == callsBefore + 1U,
                "failed issue did not call AcceptEx exactly once");
        require(api->lastErrorCalls == errorsBefore + 1U,
                "failed issue did not read last error exactly once");
        require(result.error().message.find(
                    std::to_string(testCase.nativeCode)) !=
                    std::string::npos,
                "issue error omitted the native error code");
    }
}

void extractionRejectsInvalidFramingBeforeCallingTheNativeExtractor()
{
    auto api = std::make_shared<FakeApi>();
    auto owner = discovered(api);
    NativeCallState native{};
    ActiveNativeScope active{native};
    std::array<std::byte, Extensions::Ipv6AddressRegionLength * 2U> buffer{};

    const auto reject = [&](
                            const int family,
                            const void* const storage,
                            const DWORD storageLength,
                            const DWORD receiveLength,
                            const DWORD localLength,
                            const DWORD remoteLength) {
        const std::size_t before = native.extractCalls;
        requireError(
            owner->extractAddresses(
                family,
                storage,
                storageLength,
                receiveLength,
                localLength,
                remoteLength),
            Domain::ErrorCodes::InvalidRequest,
            false,
            "extractAddresses accepted invalid framing");
        require(native.extractCalls == before,
                "invalid extraction input reached GetAcceptExSockaddrs");
    };

    constexpr DWORD Ipv4 = Extensions::Ipv4AddressRegionLength;
    reject(AF_UNSPEC, buffer.data(), Ipv4 * 2U, 0U, Ipv4, Ipv4);
    reject(AF_INET, nullptr, Ipv4 * 2U, 0U, Ipv4, Ipv4);
    reject(AF_INET, buffer.data(), Ipv4 * 2U, 1U, Ipv4, Ipv4);
    reject(AF_INET, buffer.data(), Ipv4 * 2U, 0U, Ipv4 - 1U, Ipv4);
    reject(AF_INET, buffer.data(), Ipv4 * 2U, 0U, Ipv4, Ipv4 + 1U);
    reject(AF_INET, buffer.data(), Ipv4 * 2U - 1U, 0U, Ipv4, Ipv4);

    constexpr DWORD Ipv6 = Extensions::Ipv6AddressRegionLength;
    reject(AF_INET6, buffer.data(), Ipv6 * 2U, 0U, Ipv4, Ipv6);
    reject(AF_INET6, buffer.data(), Ipv6 * 2U, 0U, Ipv6, Ipv4);
}

void extractionReturnsOwnedExactIpv4AndIpv6Addresses()
{
    for (const int family : {AF_INET, AF_INET6}) {
        auto api = std::make_shared<FakeApi>();
        auto owner = discovered(api);
        NativeCallState native{};
        native.addressFamily = family;
        ActiveNativeScope active{native};
        const DWORD region = family == AF_INET
            ? Extensions::Ipv4AddressRegionLength
            : Extensions::Ipv6AddressRegionLength;
        std::array<std::byte, Extensions::Ipv6AddressRegionLength * 2U> buffer{};
        auto addresses = take(owner->extractAddresses(
            family,
            buffer.data(),
            region * 2U,
            0U,
            region,
            region));
        const int expectedLength = family == AF_INET
            ? static_cast<int>(sizeof(sockaddr_in))
            : static_cast<int>(sizeof(sockaddr_in6));
        require(native.extractCalls == 1U,
                "valid extraction did not call native exactly once");
        require(native.extractBuffer == buffer.data(),
                "extraction changed the buffer");
        require(native.extractReceiveLength == 0U,
                "extraction changed zero receive framing");
        require(native.extractLocalLength == region &&
                    native.extractRemoteLength == region,
                "extraction changed address-region lengths");
        require(addresses.local().nativeAddressLength() == expectedLength &&
                    addresses.remote().nativeAddressLength() == expectedLength,
                "extraction changed exact sockaddr lengths");
        require(addresses.local().addressFamily() == family &&
                    addresses.remote().addressFamily() == family,
                "extraction changed address families");
        require(addresses.local().nativeAddress() !=
                    reinterpret_cast<const sockaddr*>(buffer.data() + 16U),
                "local result aliases reusable accept storage");
        require(addresses.remote().nativeAddress() !=
                    reinterpret_cast<const sockaddr*>(
                        buffer.data() + region + 16U),
                "remote result aliases reusable accept storage");

        std::memset(buffer.data(), 0, buffer.size());
        require(addresses.local().addressFamily() == family &&
                    addresses.remote().addressFamily() == family,
                "owned addresses changed after accept storage reuse");
    }
}

void extractionRejectsEveryMalformedNativeAddressResult()
{
    constexpr std::array modes{
        ExtractMode::NullLocal,
        ExtractMode::NullRemote,
        ExtractMode::ShortLocal,
        ExtractMode::LongRemote,
        ExtractMode::LocalInRemoteRegion,
        ExtractMode::RemoteInLocalRegion,
        ExtractMode::LocalBeforeBuffer,
        ExtractMode::RemoteAtBufferEnd,
        ExtractMode::WrongLocalFamily,
        ExtractMode::WrongRemoteFamily,
    };
    for (const int family : {AF_INET, AF_INET6}) {
        for (const auto mode : modes) {
            auto api = std::make_shared<FakeApi>();
            auto owner = discovered(api);
            NativeCallState native{};
            native.addressFamily = family;
            native.extractMode = mode;
            ActiveNativeScope active{native};
            const DWORD region = family == AF_INET
                ? Extensions::Ipv4AddressRegionLength
                : Extensions::Ipv6AddressRegionLength;
            std::array<std::byte,
                       Extensions::Ipv6AddressRegionLength * 2U> buffer{};
            requireError(
                owner->extractAddresses(
                    family,
                    buffer.data(),
                    region * 2U,
                    0U,
                    region,
                    region),
                Domain::ErrorCodes::IntegrityFailure,
                false,
                "malformed native address output was accepted");
            require(native.extractCalls == 1U,
                    "malformed-output case did not call native exactly once");
        }
    }
}

class NativeSocketOwner final {
public:
    explicit NativeSocketOwner(const SOCKET socket) noexcept : socket_{socket} {}
    ~NativeSocketOwner() noexcept
    {
        if (socket_ != INVALID_SOCKET) {
            static_cast<void>(::closesocket(socket_));
        }
    }
    NativeSocketOwner(const NativeSocketOwner&) = delete;
    NativeSocketOwner& operator=(const NativeSocketOwner&) = delete;

    [[nodiscard]] SOCKET get() const noexcept { return socket_; }

private:
    SOCKET socket_{INVALID_SOCKET};
};

class WinsockStartupOwner final {
public:
    explicit WinsockStartupOwner(const bool initialized) noexcept
        : initialized_{initialized}
    {
    }
    ~WinsockStartupOwner() noexcept
    {
        if (initialized_) {
            static_cast<void>(::WSACleanup());
        }
    }
    WinsockStartupOwner(const WinsockStartupOwner&) = delete;
    WinsockStartupOwner& operator=(const WinsockStartupOwner&) = delete;

private:
    bool initialized_{};
};

void systemDiscoveryFindsBothExtensionsWithoutBinding()
{
    WSADATA data{};
    const int startupStatus = ::WSAStartup(MAKEWORD(2, 2), &data);
    require(startupStatus == 0, "real WSAStartup 2.2 failed");
    WinsockStartupOwner startup{true};
    require(data.wVersion == MAKEWORD(2, 2),
            "real Winsock did not negotiate version 2.2");

    NativeSocketOwner socket{::WSASocketW(
        AF_INET,
        SOCK_STREAM,
        IPPROTO_TCP,
        nullptr,
        0U,
        WSA_FLAG_OVERLAPPED | WSA_FLAG_NO_HANDLE_INHERIT)};
    require(socket.get() != INVALID_SOCKET,
            "real unbound discovery socket creation failed");
    const auto extensions = Extensions::discover(socket.get());
    require(static_cast<bool>(extensions),
            "real unbound socket could not discover both extensions");
    require(extensions.value() != nullptr,
            "real extension discovery returned a null owner");
}

} // namespace

int main()
{
    try {
        discoveryUsesBothExactWinsockIdentifiers();
        discoveryRejectsInvalidDependenciesWithoutNativeCalls();
        discoveryFailuresAreTypedForEitherExtension();
        discoveryRejectsNullTruncatedAndOversizedPointers();
        issueAcceptRejectsEveryInvalidFramingInputBeforeNativeCall();
        issueAcceptDistinguishesSynchronousPendingAndTypedFailures();
        extractionRejectsInvalidFramingBeforeCallingTheNativeExtractor();
        extractionReturnsOwnedExactIpv4AndIpv6Addresses();
        extractionRejectsEveryMalformedNativeAddressResult();
        systemDiscoveryFindsBothExtensionsWithoutBinding();
        std::cout << "Dashboard Winsock extension tests passed: "
                  << assertionCount << " assertions.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Dashboard Winsock extension tests failed after "
                  << assertionCount << " assertions: " << error.what()
                  << '\n';
        return 1;
    }
}
