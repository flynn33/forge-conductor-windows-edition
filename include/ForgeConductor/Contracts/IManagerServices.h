#pragma once

#include "ForgeConductor/Domain/OperationContext.h"
#include "ForgeConductor/Domain/ManagerModels.h"

namespace ForgeConductor::Contracts {

class IManagerClient {
public:
    virtual ~IManagerClient() = default;

    [[nodiscard]] virtual Domain::Result<Domain::ManagerStatus> status(
        const Domain::OperationContext& context) noexcept = 0;

    [[nodiscard]] virtual Domain::Result<Domain::ManagerSettings> settings(
        const Domain::OperationContext& context) noexcept = 0;

    [[nodiscard]] virtual Domain::Result<Domain::ManagerStatus> control(
        const Domain::ManagerControlRequest& request,
        const Domain::OperationContext& context) noexcept = 0;

    [[nodiscard]] virtual Domain::Result<Domain::ManagerSettings> updateSettings(
        const Domain::ManagerSettingsPatch& patch,
        bool applyImmediately,
        const Domain::OperationContext& context) noexcept = 0;

    // Requests an orderly shutdown of the remote manager. This is distinct
    // from shutdown(), which only closes this client transport instance.
    [[nodiscard]] virtual Domain::Result<void> requestShutdown(
        const Domain::OperationContext& context) noexcept = 0;

    virtual void shutdown() noexcept = 0;
};

class IManagerServer {
public:
    virtual ~IManagerServer() = default;

    [[nodiscard]] virtual Domain::Result<void> run(
        const Domain::OperationContext& context) noexcept = 0;

    virtual void cancel(const Domain::OperationId& operationId) noexcept = 0;
    virtual void shutdown() noexcept = 0;
};

} // namespace ForgeConductor::Contracts
