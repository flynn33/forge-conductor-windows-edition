#pragma once

#include "ForgeConductor/Domain/GraphicsModels.h"
#include "ForgeConductor/Domain/OperationContext.h"

namespace ForgeConductor::Contracts {

class IGraphicsDeviceService {
public:
    virtual ~IGraphicsDeviceService() = default;

    [[nodiscard]] virtual Domain::Result<Domain::GraphicsDeviceStatus> initialize(
        const Domain::OperationContext& context) noexcept = 0;

    [[nodiscard]] virtual Domain::Result<Domain::GraphicsDeviceStatus> status(
        const Domain::OperationContext& context) noexcept = 0;

    [[nodiscard]] virtual Domain::Result<Domain::GraphicsDeviceStatus> recover(
        const Domain::OperationContext& context) noexcept = 0;

    virtual void cancel(const Domain::OperationId& operationId) noexcept = 0;
    virtual void shutdown() noexcept = 0;
};

class IRenderService {
public:
    virtual ~IRenderService() = default;

    [[nodiscard]] virtual Domain::Result<Domain::RenderOutcome> render(
        const Domain::RenderRequest& request,
        const Domain::OperationContext& context) noexcept = 0;

    virtual void cancel(const Domain::OperationId& operationId) noexcept = 0;
    virtual void shutdown() noexcept = 0;
};

} // namespace ForgeConductor::Contracts
