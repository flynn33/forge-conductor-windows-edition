#pragma once

#include "ForgeConductor/Contracts/INativeSessionHostServices.h"

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>

namespace ForgeConductor::Infrastructure::Windows {

struct WinHttpLocalModelSessionTransportConfiguration final {
    std::string loopbackHost{"127.0.0.1"};
    std::uint16_t port{};
    std::string basePath{"/v1/forge"};
    bool secure{};
    std::chrono::milliseconds connectTimeout{std::chrono::seconds{5}};
    std::chrono::milliseconds sendTimeout{std::chrono::seconds{10}};
    std::chrono::milliseconds receiveTimeout{std::chrono::seconds{10}};
};

// Synchronous, caller-owned transport for a loopback local-model provider.
// WinHTTP calls are bounded by both the immutable transport configuration and
// each supplied operation deadline. Cancellation closes the active request.
class WinHttpLocalModelSessionTransport final
    : public Contracts::INativeSessionTransport {
public:
    explicit WinHttpLocalModelSessionTransport(
        WinHttpLocalModelSessionTransportConfiguration configuration);
    ~WinHttpLocalModelSessionTransport() noexcept override;

    WinHttpLocalModelSessionTransport(
        const WinHttpLocalModelSessionTransport&) = delete;
    WinHttpLocalModelSessionTransport& operator=(
        const WinHttpLocalModelSessionTransport&) = delete;
    WinHttpLocalModelSessionTransport(
        WinHttpLocalModelSessionTransport&&) = delete;
    WinHttpLocalModelSessionTransport& operator=(
        WinHttpLocalModelSessionTransport&&) = delete;

    [[nodiscard]] Domain::Result<Domain::NativeTransportSession>
    createSession(
        const Domain::SessionCreationRequest& request,
        const Domain::OperationContext& context) noexcept override;

    [[nodiscard]] Domain::Result<Domain::NativeBootstrapResponse> bootstrap(
        const Domain::NativeBootstrapRequest& request,
        const Domain::OperationContext& context) noexcept override;

    [[nodiscard]] Domain::Result<Domain::HostSessionStatus> query(
        const Domain::ProviderSessionId& sessionId,
        const Domain::OperationContext& context) noexcept override;

    void cancel(
        const Domain::OperationId& operationId,
        const std::optional<Domain::ProviderSessionId>& sessionId)
        noexcept override;

    void shutdown() noexcept override;

private:
    class Impl;
    std::unique_ptr<Impl> implementation_;
};

} // namespace ForgeConductor::Infrastructure::Windows
