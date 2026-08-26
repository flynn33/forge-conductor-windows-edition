#pragma once

#include "ForgeConductor/Domain/Domain.h"

#include <optional>
#include <string_view>

namespace ForgeConductor::Contracts {

class ISessionHostAdapter {
public:
    virtual ~ISessionHostAdapter() = default;

    [[nodiscard]] virtual const Domain::AdapterId& identifier() const noexcept = 0;
    [[nodiscard]] virtual std::string_view version() const noexcept = 0;

    [[nodiscard]] virtual Domain::Result<Domain::HostCapabilities> capabilities(
        const Domain::OperationContext& context) noexcept = 0;

    [[nodiscard]] virtual Domain::Result<Domain::HostSession> createSession(
        const Domain::SessionCreationRequest& request,
        const Domain::OperationContext& context) noexcept = 0;

    [[nodiscard]] virtual Domain::Result<std::optional<Domain::HostSession>>
    queryByIdempotencyKey(
        const Domain::ProjectId& projectId,
        const Domain::IdempotencyKey& key,
        const Domain::OperationContext& context) noexcept = 0;

    [[nodiscard]] virtual Domain::Result<void> bootstrap(
        const Domain::HostSession& session,
        const Domain::ContinuityHandoff& handoff,
        const Domain::OperationContext& context) noexcept = 0;

    [[nodiscard]] virtual Domain::Result<Domain::HandoffAcknowledgement>
    awaitAcknowledgement(
        const Domain::HostSession& session,
        const Domain::ContinuityHandoffId& handoffId,
        const Domain::Sha256Digest& handoffSha256,
        const Domain::OperationContext& context) noexcept = 0;

    [[nodiscard]] virtual Domain::Result<Domain::HostSessionStatus> query(
        const Domain::SessionId& sessionId,
        const Domain::OperationContext& context) noexcept = 0;

    [[nodiscard]] virtual Domain::Result<Domain::HostRecoveryReport> recover(
        const Domain::HostRecoveryRequest& request,
        const Domain::OperationContext& context) noexcept = 0;

    virtual void cancel(const Domain::OperationId& operationId) noexcept = 0;
    virtual void shutdown() noexcept = 0;
};

class ILocalModelClient {
public:
    virtual ~ILocalModelClient() = default;

    [[nodiscard]] virtual Domain::Result<Domain::HostSessionStatus> query(
        const Domain::ProviderSessionId& sessionId,
        const Domain::OperationContext& context) noexcept = 0;

    [[nodiscard]] virtual Domain::Result<void> cancel(
        const Domain::ProviderSessionId& sessionId,
        const Domain::OperationContext& context) noexcept = 0;

    virtual void shutdown() noexcept = 0;
};

} // namespace ForgeConductor::Contracts
