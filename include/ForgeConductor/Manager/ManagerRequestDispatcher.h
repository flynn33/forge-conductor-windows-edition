#pragma once

#include "ForgeConductor/Contracts/IFoundationServices.h"
#include "ForgeConductor/Contracts/IManagerRuntime.h"
#include "ForgeConductor/Manager/ManagerProtocolCodec.h"
#include "ForgeConductor/Manager/ManagerTransportLimits.h"

#include <chrono>
#include <cstddef>
#include <memory>

namespace ForgeConductor::Manager {

class ManagerRequestDispatcher final {
public:
    ManagerRequestDispatcher(
        std::shared_ptr<Contracts::IManagerController> controller,
        std::shared_ptr<Contracts::IClock> clock,
        ManagerTransportLimits limits = {});
    ~ManagerRequestDispatcher() noexcept;

    ManagerRequestDispatcher(const ManagerRequestDispatcher&) = delete;
    ManagerRequestDispatcher& operator=(const ManagerRequestDispatcher&) = delete;
    ManagerRequestDispatcher(ManagerRequestDispatcher&&) = delete;
    ManagerRequestDispatcher& operator=(ManagerRequestDispatcher&&) = delete;

    [[nodiscard]] ManagerResponse dispatch(const ManagerRequest& request) noexcept;

    // Stops regular admission and requests cancellation for every admitted
    // operation. Cancellation and shutdown control requests bypass admission.
    void beginShutdown() noexcept;
    void cancel(const Domain::OperationId& operationId) noexcept;

    [[nodiscard]] bool waitUntilIdle(
        std::chrono::milliseconds timeout) noexcept;
    [[nodiscard]] std::size_t activeOperationCount() const noexcept;
    [[nodiscard]] bool isAccepting() const noexcept;

    // Idempotently closes admission, drains for the configured bounded interval,
    // and closes the injected controller after no dispatcher callback is active.
    void shutdown() noexcept;

private:
    class Implementation;
    std::shared_ptr<Implementation> implementation_;
};

} // namespace ForgeConductor::Manager
