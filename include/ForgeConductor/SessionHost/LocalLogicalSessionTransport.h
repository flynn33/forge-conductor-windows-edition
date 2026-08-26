#pragma once

#include "ForgeConductor/Contracts/IContinuityDocumentCodec.h"
#include "ForgeConductor/Contracts/IFoundationServices.h"
#include "ForgeConductor/Contracts/INativeSessionHostServices.h"

#include <memory>

namespace ForgeConductor::SessionHost {

class LocalLogicalSessionTransport final
    : public Contracts::INativeSessionTransport {
public:
    LocalLogicalSessionTransport(
        std::shared_ptr<Contracts::IHasher> hasher,
        Contracts::IContinuityDocumentCodec& codec);
    LocalLogicalSessionTransport(
        std::shared_ptr<Contracts::IHasher> hasher,
        Contracts::IContinuityDocumentCodec& codec,
        Contracts::INativeLogicalContinuationScheduler& scheduler);
    ~LocalLogicalSessionTransport() noexcept override;

    LocalLogicalSessionTransport(const LocalLogicalSessionTransport&) = delete;
    LocalLogicalSessionTransport& operator=(
        const LocalLogicalSessionTransport&) = delete;
    LocalLogicalSessionTransport(LocalLogicalSessionTransport&&) = delete;
    LocalLogicalSessionTransport& operator=(
        LocalLogicalSessionTransport&&) = delete;

    [[nodiscard]] Domain::Result<Domain::NativeTransportSession> createSession(
        const Domain::SessionCreationRequest& request,
        const Domain::OperationContext& context) noexcept override;

    [[nodiscard]] Domain::Result<Domain::NativeBootstrapResponse> bootstrap(
        const Domain::NativeBootstrapRequest& request,
        const Domain::OperationContext& context) noexcept override;

    [[nodiscard]] Domain::Result<Domain::HostSessionStatus> query(
        const Domain::ProviderSessionId& sessionId,
        const Domain::OperationContext& context) noexcept override;

    [[nodiscard]] Domain::Result<std::optional<Domain::NativeLogicalContinuation>>
    continuation(
        const Domain::ProviderSessionId& sessionId,
        const Domain::OperationContext& context) noexcept;

    void cancel(
        const Domain::OperationId& operationId,
        const std::optional<Domain::ProviderSessionId>& sessionId) noexcept override;

    void shutdown() noexcept override;

private:
    class Impl;
    std::unique_ptr<Impl> implementation_;
};

} // namespace ForgeConductor::SessionHost
