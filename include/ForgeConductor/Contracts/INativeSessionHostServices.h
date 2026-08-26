#pragma once

#include "ForgeConductor/Domain/Domain.h"

#include <optional>

namespace ForgeConductor::Contracts {

// Provider transports must make createSession idempotent for the supplied
// request key. The native adapter may repeat that call after process loss.
class INativeSessionTransport {
public:
    virtual ~INativeSessionTransport() = default;

    [[nodiscard]] virtual Domain::Result<Domain::NativeTransportSession>
    createSession(
        const Domain::SessionCreationRequest& request,
        const Domain::OperationContext& context) noexcept = 0;

    [[nodiscard]] virtual Domain::Result<Domain::NativeBootstrapResponse>
    bootstrap(
        const Domain::NativeBootstrapRequest& request,
        const Domain::OperationContext& context) noexcept = 0;

    [[nodiscard]] virtual Domain::Result<Domain::HostSessionStatus> query(
        const Domain::ProviderSessionId& sessionId,
        const Domain::OperationContext& context) noexcept = 0;

    virtual void cancel(
        const Domain::OperationId& operationId,
        const std::optional<Domain::ProviderSessionId>& sessionId) noexcept = 0;

    virtual void shutdown() noexcept = 0;
};

class INativeSessionLedger {
public:
    virtual ~INativeSessionLedger() = default;

    [[nodiscard]] virtual Domain::Result<Domain::NativeSessionLedger> load(
        const Domain::OperationContext& context) noexcept = 0;

    // Commits a complete bounded snapshot only if expectedRevision still
    // matches durable state. Implementations increment the revision and return
    // the exact published snapshot, including its checksum.
    [[nodiscard]] virtual Domain::Result<Domain::NativeSessionLedger> commit(
        const Domain::NativeSessionLedger& ledger,
        std::uint64_t expectedRevision,
        const Domain::OperationContext& context) noexcept = 0;

    virtual void shutdown() noexcept = 0;
};

} // namespace ForgeConductor::Contracts
