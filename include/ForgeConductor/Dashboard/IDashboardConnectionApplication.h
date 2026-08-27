#pragma once

#include "ForgeConductor/Dashboard/DashboardHttpModels.h"
#include "ForgeConductor/Dashboard/DashboardPreparedExchange.h"
#include "ForgeConductor/Domain/OperationContext.h"
#include "ForgeConductor/Domain/Result.h"

namespace ForgeConductor::Dashboard {

// Pure application boundary for one already parsed loopback request. The
// request and contexts are owned values; implementations must not retain
// references into a transport buffer.
class IDashboardConnectionApplication {
public:
    virtual ~IDashboardConnectionApplication() noexcept = default;

    [[nodiscard]] virtual Domain::Result<DashboardPreparedExchange> prepare(
        DashboardHttpRequest request,
        bool operationalServiceActive,
        Domain::OperationContext context) noexcept = 0;

    // The transport invokes this only after the complete response has been
    // delivered. It supplies a fresh context whose deadline and cancellation
    // lifetime are independent from prepare and socket delivery.
    [[nodiscard]] virtual Domain::Result<void> executePostDelivery(
        DashboardPostDeliveryAction action,
        Domain::OperationContext context) noexcept = 0;
};

} // namespace ForgeConductor::Dashboard
