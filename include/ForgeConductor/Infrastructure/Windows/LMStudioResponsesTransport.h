#pragma once

#include "ForgeConductor/Contracts/INativeSessionHostServices.h"

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>

namespace ForgeConductor::Infrastructure::Windows {

struct LMStudioResponsesTransportConfiguration final {
    std::string loopbackHost{"127.0.0.1"};
    std::uint16_t port{1234U};
    std::string basePath{"/v1"};
    bool secure{};
    std::optional<std::string> model;
    std::optional<std::string> bearerToken;
    std::chrono::milliseconds connectTimeout{std::chrono::seconds{5}};
    std::chrono::milliseconds sendTimeout{std::chrono::seconds{30}};
    std::chrono::milliseconds receiveTimeout{std::chrono::seconds{120}};
};

// Native non-streaming LM Studio Responses transport. A session begins as a
// local pending binding; bootstrap creates a real fresh /v1/responses root,
// services its context_get function call, and returns the actual terminal
// response id for durable previous_response_id chaining.
class LMStudioResponsesTransport final
    : public Contracts::INativeSessionTransport {
public:
    explicit LMStudioResponsesTransport(
        LMStudioResponsesTransportConfiguration configuration = {});
    ~LMStudioResponsesTransport() noexcept override;

    LMStudioResponsesTransport(const LMStudioResponsesTransport&) = delete;
    LMStudioResponsesTransport& operator=(
        const LMStudioResponsesTransport&) = delete;
    LMStudioResponsesTransport(LMStudioResponsesTransport&&) = delete;
    LMStudioResponsesTransport& operator=(
        LMStudioResponsesTransport&&) = delete;

    [[nodiscard]] Domain::Result<Domain::NativeTransportSession> createSession(
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
