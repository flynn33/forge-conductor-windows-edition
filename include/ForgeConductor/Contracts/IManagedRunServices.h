#pragma once

#include "ForgeConductor/Domain/ManagedRunModels.h"
#include "ForgeConductor/Domain/Result.h"

#include <optional>

namespace ForgeConductor::Contracts {

class IManagedResponsesTransport {
public:
    virtual ~IManagedResponsesTransport() = default;

    [[nodiscard]] virtual Domain::Result<Domain::ManagedProviderTurnResult>
    complete(
        const Domain::ManagedProviderTurnRequest& request,
        const Domain::OperationContext& context) noexcept = 0;

    virtual void cancel(
        const Domain::OperationId& operationId,
        const std::optional<Domain::ProviderSessionId>& responseId) noexcept = 0;
};

class IManagedRunStore {
public:
    virtual ~IManagedRunStore() = default;

    [[nodiscard]] virtual Domain::Result<std::optional<Domain::ManagedRunRecord>>
    load(
        const Domain::SessionId& runId,
        const Domain::OperationContext& context) noexcept = 0;

    [[nodiscard]] virtual Domain::Result<void> save(
        const Domain::ManagedRunRecord& record,
        const Domain::OperationContext& context) noexcept = 0;
};

class IManagedRunService {
public:
    virtual ~IManagedRunService() = default;

    [[nodiscard]] virtual Domain::Result<Domain::ManagedRunSnapshot> start(
        const Domain::ManagedRunStartRequest& request,
        const Domain::OperationContext& context) noexcept = 0;

    [[nodiscard]] virtual Domain::Result<Domain::ManagedRunSnapshot> status(
        const Domain::SessionId& runId,
        const Domain::OperationContext& context) noexcept = 0;

    [[nodiscard]] virtual Domain::Result<Domain::ManagedRunSnapshot> cancel(
        const Domain::SessionId& runId,
        const Domain::OperationContext& context) noexcept = 0;

    [[nodiscard]] virtual Domain::Result<Domain::ManagedRunSnapshot> pause(
        const Domain::SessionId& runId,
        const Domain::OperationContext& context) noexcept = 0;

    [[nodiscard]] virtual Domain::Result<Domain::ManagedRunSnapshot> resume(
        const Domain::SessionId& runId,
        const Domain::OperationContext& context) noexcept = 0;

    virtual void shutdown() noexcept = 0;
};

} // namespace ForgeConductor::Contracts
