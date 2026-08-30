#pragma once

#include "ForgeConductor/Domain/OperationContext.h"

#include <cstdint>
#include <string_view>

namespace ForgeConductor::Contracts {

// Process-owned one-shot boundary for opening the authenticated dashboard in
// the current user's registered browser. launch() admits bounded asynchronous
// work only; beginShutdown() signals cancellation without joining, while
// shutdown() retains the exact join before process ownership can be released.
class IDashboardBrowserLauncher {
public:
    virtual ~IDashboardBrowserLauncher() noexcept = default;

    [[nodiscard]] virtual Domain::Result<void> launch(
        std::string_view dashboardHost,
        std::uint16_t dashboardPort,
        const Domain::OperationContext& context) noexcept = 0;

    // Nonblocking. Closes admission and requests cancellation of admitted work.
    virtual void beginShutdown() noexcept = 0;

    // Closes admission, requests cancellation, and exact-joins admitted work.
    virtual void shutdown() noexcept = 0;
};

} // namespace ForgeConductor::Contracts
