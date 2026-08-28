#pragma once

#include "DashboardConnectionEventBridge.h"
#include "DashboardConnectionResponseCatalog.h"
#include "DashboardConnectionRuntimeServices.h"
#include "DashboardConnectionSocket.h"
#include "DashboardIocpWorkerKernel.h"

#include "ForgeConductor/Dashboard/IDashboardConnectionApplication.h"
#include "ForgeConductor/Domain/Result.h"
#include "ForgeConductor/Infrastructure/Windows/WindowsDashboardDeadlineScheduler.h"
#include "ForgeConductor/Infrastructure/Windows/WindowsDashboardHandlerExecutor.h"

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>

namespace ForgeConductor::Infrastructure::Windows::Detail {

enum class DashboardConnectionLifecycleState : std::uint8_t {
    Created,
    Receiving,
    Preparing,
    SendingComplete,
    SendingSseBootstrap,
    SseIdle,
    SendingSseFrame,
    AwaitingPostDelivery,
    Closing,
    Drained,
};

// Fixed-size status used by the process registry without exposing transport
// ownership or permitting work to escape the connection's bounded owner.
class DashboardConnectionStateSnapshot final {
public:
    DashboardConnectionStateSnapshot(
        DashboardConnectionLifecycleState state,
        std::uint64_t registrationId,
        std::uint64_t generationId,
        DashboardIoCompletionKey completionKey,
        bool socketOperationOutstanding,
        bool eventOperationOutstanding,
        bool deadlineArmed,
        bool shutdownRequested,
        bool hasFailure) noexcept;

    [[nodiscard]] DashboardConnectionLifecycleState state() const noexcept
    {
        return state_;
    }

    [[nodiscard]] std::uint64_t registrationId() const noexcept
    {
        return registrationId_;
    }

    [[nodiscard]] std::uint64_t generationId() const noexcept
    {
        return generationId_;
    }

    [[nodiscard]] DashboardIoCompletionKey completionKey() const noexcept
    {
        return completionKey_;
    }

    [[nodiscard]] bool socketOperationOutstanding() const noexcept
    {
        return socketOperationOutstanding_;
    }

    [[nodiscard]] bool eventOperationOutstanding() const noexcept
    {
        return eventOperationOutstanding_;
    }

    [[nodiscard]] bool deadlineArmed() const noexcept
    {
        return deadlineArmed_;
    }

    [[nodiscard]] bool shutdownRequested() const noexcept
    {
        return shutdownRequested_;
    }

    [[nodiscard]] bool hasFailure() const noexcept { return hasFailure_; }

private:
    DashboardConnectionLifecycleState state_{};
    std::uint64_t registrationId_{};
    std::uint64_t generationId_{};
    DashboardIoCompletionKey completionKey_{0U};
    bool socketOperationOutstanding_{};
    bool eventOperationOutstanding_{};
    bool deadlineArmed_{};
    bool shutdownRequested_{};
    bool hasFailure_{};
};

// Stable, registry-facing dispatch boundary. The registry selects an owner by
// completion key and never holds its own lock while invoking these methods.
// Implementations consume completions synchronously and retain every native,
// synthetic-event, and deadline obligation until isDrained becomes true.
class IDashboardConnectionDispatchTarget {
public:
    virtual ~IDashboardConnectionDispatchTarget() noexcept = default;

    [[nodiscard]] virtual DashboardIoCompletionKey completionKey()
        const noexcept = 0;
    [[nodiscard]] virtual std::uint64_t registrationId() const noexcept = 0;
    [[nodiscard]] virtual std::uint64_t generationId() const noexcept = 0;

    [[nodiscard]] virtual Domain::Result<void> start() noexcept = 0;

    virtual void dispatchIocp(
        DWORD transferredBytes,
        OVERLAPPED* operation,
        DWORD nativeError) noexcept = 0;

    virtual void dispatchDeadline(
        WindowsDashboardDeadline deadline) noexcept = 0;

    virtual void beginShutdown() noexcept = 0;

    [[nodiscard]] virtual bool isDrained() const noexcept = 0;
    [[nodiscard]] virtual DashboardConnectionStateSnapshot snapshot()
        const noexcept = 0;
};

// One heap-stable owner for the complete lifecycle of an accepted dashboard
// connection. The process registry retains this object until Drained. The
// borrowed process services and kernel outlive every registered connection.
class DashboardConnectionState final
    : public IDashboardConnectionDispatchTarget,
      public IDashboardConnectionEventFatalSink,
      private std::enable_shared_from_this<DashboardConnectionState> {
public:
    static constexpr auto HeaderIngressLifetime = std::chrono::seconds{2};
    static constexpr auto SocketLifetime = std::chrono::seconds{15};
    static constexpr auto ServerSentEventsLifetime = std::chrono::hours{1};

    [[nodiscard]] static Domain::Result<
        std::shared_ptr<DashboardConnectionState>>
    create(
        std::uint64_t generationId,
        DashboardConnectionRuntimeIdentity identity,
        std::unique_ptr<IDashboardConnectionIo> socket,
        DashboardIocpWorkerKernel& kernel,
        WindowsDashboardDeadlineScheduler& deadlineScheduler,
        WindowsDashboardHandlerExecutor& handlerExecutor,
        DashboardConnectionRuntimeServices& runtimeServices,
        std::shared_ptr<Dashboard::IDashboardConnectionApplication>
            application,
        DashboardConnectionResponseCatalog& responseCatalog) noexcept;

    DashboardConnectionState(const DashboardConnectionState&) = delete;
    DashboardConnectionState& operator=(const DashboardConnectionState&) =
        delete;
    DashboardConnectionState(DashboardConnectionState&&) = delete;
    DashboardConnectionState& operator=(DashboardConnectionState&&) =
        delete;
    ~DashboardConnectionState() noexcept override;

    [[nodiscard]] DashboardIoCompletionKey completionKey()
        const noexcept override;
    [[nodiscard]] std::uint64_t registrationId() const noexcept override;
    [[nodiscard]] std::uint64_t generationId() const noexcept override;

    [[nodiscard]] Domain::Result<void> start() noexcept override;

    void dispatchIocp(
        DWORD transferredBytes,
        OVERLAPPED* operation,
        DWORD nativeError) noexcept override;

    void dispatchDeadline(
        WindowsDashboardDeadline deadline) noexcept override;

    void beginShutdown() noexcept override;

    [[nodiscard]] bool isDrained() const noexcept override;
    [[nodiscard]] DashboardConnectionStateSnapshot snapshot()
        const noexcept override;

    [[nodiscard]] std::optional<Domain::Error> fullFailure() const;

    void fatal(
        DashboardConnectionEventFatalNotification notification)
        noexcept override;

private:
    class Impl;

    DashboardConnectionState() noexcept = default;

    std::unique_ptr<Impl> implementation_;
};

} // namespace ForgeConductor::Infrastructure::Windows::Detail
