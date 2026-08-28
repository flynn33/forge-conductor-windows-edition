#include "DashboardWinsockRuntime.h"

#include <string>
#include <string_view>
#include <utility>

namespace ForgeConductor::Infrastructure::Windows::Detail {
namespace {

[[nodiscard]] Domain::Error makeWinsockError(
    const std::string_view action,
    const int nativeCode,
    const std::string_view stableCode,
    const bool retryable = false) noexcept
{
    try {
        std::string message{action};
        message += " failed with Winsock error ";
        message += std::to_string(nativeCode);
        message += '.';
        return Domain::makeError(stableCode, std::move(message), retryable);
    } catch (...) {
        return Domain::makeError(
            Domain::ErrorCodes::InternalFailure,
            "A dashboard Winsock operation failed and its diagnostic could not be formatted.",
            retryable);
    }
}

[[nodiscard]] Domain::Error startupError(const int nativeCode) noexcept
{
    switch (nativeCode) {
    case WSAVERNOTSUPPORTED:
        return makeWinsockError(
            "Initialize dashboard Winsock", nativeCode,
            Domain::ErrorCodes::UnsupportedVersion);
    case WSASYSNOTREADY:
        return makeWinsockError(
            "Initialize dashboard Winsock", nativeCode,
            Domain::ErrorCodes::HostCapabilityUnavailable, true);
    case WSAEPROCLIM:
        return makeWinsockError(
            "Initialize dashboard Winsock", nativeCode,
            Domain::ErrorCodes::LimitExceeded, true);
    case WSAEINPROGRESS:
        return makeWinsockError(
            "Initialize dashboard Winsock", nativeCode,
            Domain::ErrorCodes::Conflict, true);
    default:
        return makeWinsockError(
            "Initialize dashboard Winsock", nativeCode,
            Domain::ErrorCodes::InternalFailure);
    }
}

[[nodiscard]] Domain::Error socketCreationError(const int nativeCode) noexcept
{
    switch (nativeCode) {
    case WSAEMFILE:
    case WSAENOBUFS:
        return makeWinsockError(
            "Create an overlapped dashboard TCP socket", nativeCode,
            Domain::ErrorCodes::LimitExceeded, true);
    case WSANOTINITIALISED:
    case WSAENETDOWN:
    case WSASYSNOTREADY:
        return makeWinsockError(
            "Create an overlapped dashboard TCP socket", nativeCode,
            Domain::ErrorCodes::HostCapabilityUnavailable, true);
    case WSAEAFNOSUPPORT:
    case WSAEPROTONOSUPPORT:
    case WSAESOCKTNOSUPPORT:
    case WSAEPROTOTYPE:
        return makeWinsockError(
            "Create an overlapped dashboard TCP socket", nativeCode,
            Domain::ErrorCodes::HostCapabilityUnavailable);
    default:
        return makeWinsockError(
            "Create an overlapped dashboard TCP socket", nativeCode,
            Domain::ErrorCodes::InternalFailure);
    }
}

} // namespace

class DashboardWinsockRuntimeState final {
public:
    explicit DashboardWinsockRuntimeState(
        std::shared_ptr<IDashboardWinsockApi> api) noexcept
        : api_{std::move(api)}
    {
    }

    ~DashboardWinsockRuntimeState() noexcept
    {
        static_cast<void>(api_->cleanup());
    }

    DashboardWinsockRuntimeState(const DashboardWinsockRuntimeState&) = delete;
    DashboardWinsockRuntimeState& operator=(
        const DashboardWinsockRuntimeState&) = delete;
    DashboardWinsockRuntimeState(DashboardWinsockRuntimeState&&) = delete;
    DashboardWinsockRuntimeState& operator=(
        DashboardWinsockRuntimeState&&) = delete;

    [[nodiscard]] IDashboardWinsockApi& api() const noexcept { return *api_; }

private:
    std::shared_ptr<IDashboardWinsockApi> api_;
};

int DashboardWinsockSystemApi::startup(
    const WORD requestedVersion,
    WSADATA& data) noexcept
{
    return ::WSAStartup(requestedVersion, &data);
}

int DashboardWinsockSystemApi::cleanup() noexcept
{
    return ::WSACleanup();
}

SOCKET DashboardWinsockSystemApi::createSocket(
    const int addressFamily,
    const int socketType,
    const int protocol,
    const DWORD flags) noexcept
{
    return ::WSASocketW(
        addressFamily, socketType, protocol, nullptr, 0U, flags);
}

int DashboardWinsockSystemApi::closeSocket(const SOCKET socket) noexcept
{
    return ::closesocket(socket);
}

int DashboardWinsockSystemApi::lastError() noexcept
{
    return ::WSAGetLastError();
}

UniqueDashboardSocket::UniqueDashboardSocket(
    std::shared_ptr<DashboardWinsockRuntimeState> state,
    const SOCKET socket) noexcept
    : state_{std::move(state)}, socket_{socket}
{
}

UniqueDashboardSocket::~UniqueDashboardSocket() noexcept
{
    reset();
}

UniqueDashboardSocket::UniqueDashboardSocket(
    UniqueDashboardSocket&& other) noexcept
    : state_{std::move(other.state_)},
      socket_{std::exchange(other.socket_, INVALID_SOCKET)}
{
}

UniqueDashboardSocket& UniqueDashboardSocket::operator=(
    UniqueDashboardSocket&& other) noexcept
{
    if (this != &other) {
        reset();
        state_ = std::move(other.state_);
        socket_ = std::exchange(other.socket_, INVALID_SOCKET);
    }
    return *this;
}

void UniqueDashboardSocket::reset() noexcept
{
    const SOCKET previous = std::exchange(socket_, INVALID_SOCKET);
    if (previous != INVALID_SOCKET && state_ != nullptr) {
        static_cast<void>(state_->api().closeSocket(previous));
    }
    state_.reset();
}

DashboardWinsockRuntime::DashboardWinsockRuntime(
    std::shared_ptr<DashboardWinsockRuntimeState> state) noexcept
    : state_{std::move(state)}
{
}

Domain::Result<std::unique_ptr<DashboardWinsockRuntime>>
DashboardWinsockRuntime::create() noexcept
{
    try {
        return create(std::make_shared<DashboardWinsockSystemApi>());
    } catch (...) {
        return Domain::Result<std::unique_ptr<DashboardWinsockRuntime>>::failure(
            Domain::makeError(
                Domain::ErrorCodes::InternalFailure,
                "The dashboard could not allocate its Winsock API owner."));
    }
}

Domain::Result<std::unique_ptr<DashboardWinsockRuntime>>
DashboardWinsockRuntime::create(
    std::shared_ptr<IDashboardWinsockApi> api) noexcept
{
    if (api == nullptr) {
        return Domain::Result<std::unique_ptr<DashboardWinsockRuntime>>::failure(
            Domain::makeError(
                Domain::ErrorCodes::InvalidRequest,
                "The dashboard Winsock runtime requires a native API dependency."));
    }

    WSADATA data{};
    const int startupStatus = api->startup(RequiredVersion, data);
    if (startupStatus != 0) {
        return Domain::Result<std::unique_ptr<DashboardWinsockRuntime>>::failure(
            startupError(startupStatus));
    }

    if (data.wVersion != RequiredVersion) {
        static_cast<void>(api->cleanup());
        return Domain::Result<std::unique_ptr<DashboardWinsockRuntime>>::failure(
            Domain::makeError(
                Domain::ErrorCodes::UnsupportedVersion,
                "Winsock did not negotiate the required version 2.2."));
    }

    bool startupNeedsCleanup = true;
    try {
        auto state = std::make_shared<DashboardWinsockRuntimeState>(api);
        startupNeedsCleanup = false;
        auto runtime = std::unique_ptr<DashboardWinsockRuntime>{
            new DashboardWinsockRuntime{std::move(state)}};
        return Domain::Result<std::unique_ptr<DashboardWinsockRuntime>>::success(
            std::move(runtime));
    } catch (...) {
        if (startupNeedsCleanup) {
            static_cast<void>(api->cleanup());
        }
        return Domain::Result<std::unique_ptr<DashboardWinsockRuntime>>::failure(
            Domain::makeError(
                Domain::ErrorCodes::InternalFailure,
                "The dashboard could not allocate its initialized Winsock state."));
    }
}

Domain::Result<UniqueDashboardSocket>
DashboardWinsockRuntime::createOverlappedTcpSocket(
    const int addressFamily) noexcept
{
    if (addressFamily != AF_INET && addressFamily != AF_INET6) {
        return Domain::Result<UniqueDashboardSocket>::failure(
            Domain::makeError(
                Domain::ErrorCodes::InvalidRequest,
                "A dashboard TCP socket requires an IPv4 or IPv6 address family."));
    }

    const SOCKET socket = state_->api().createSocket(
        addressFamily, SOCK_STREAM, IPPROTO_TCP, RequiredSocketFlags);
    if (socket == INVALID_SOCKET) {
        return Domain::Result<UniqueDashboardSocket>::failure(
            socketCreationError(state_->api().lastError()));
    }

    try {
        return Domain::Result<UniqueDashboardSocket>::success(
            UniqueDashboardSocket{state_, socket});
    } catch (...) {
        static_cast<void>(state_->api().closeSocket(socket));
        return Domain::Result<UniqueDashboardSocket>::failure(
            Domain::makeError(
                Domain::ErrorCodes::InternalFailure,
                "The dashboard could not own its newly created socket."));
    }
}

} // namespace ForgeConductor::Infrastructure::Windows::Detail
