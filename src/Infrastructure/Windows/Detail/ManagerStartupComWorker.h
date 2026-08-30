#pragma once

#include "ForgeConductor/Domain/ManagerStartupModels.h"
#include "ForgeConductor/Domain/OperationContext.h"
#include "ForgeConductor/Domain/Result.h"

#include <chrono>
#include <cstddef>
#include <memory>
#include <string>
#include <variant>

namespace ForgeConductor::Infrastructure::Windows::Detail {

enum class ManagerStartupComOperationKind {
    Inspect,
    Register,
    Repair,
    SetEnabled,
    StartNow,
    Remove
};

struct ManagerStartupComRequest final {
    ManagerStartupComOperationKind kind;
    Domain::ManagerStartupDefinition expected;
    std::string purposeSuffix;
    bool enabled;
    Domain::OperationContext context;
};

using ManagerStartupComResponse = std::variant<
    Domain::ManagerStartupStatus,
    Domain::ManagerStartupOutcome>;
using ManagerStartupComResult = Domain::Result<ManagerStartupComResponse>;

// Invoked only on the worker's initialized MTA. Implementations must return
// only after all operation-owned COM interfaces have been released.
class IManagerStartupComHandler {
public:
    virtual ~IManagerStartupComHandler() noexcept = default;

    [[nodiscard]] virtual ManagerStartupComResult handle(
        const ManagerStartupComRequest& request) noexcept = 0;
};

// Deterministic scheduling seam for native concurrency tests. Production
// composition must use ManagerStartupComWorker::create and never supplies it.
class IManagerStartupComWorkerAdmissionGate {
public:
    virtual ~IManagerStartupComWorkerAdmissionGate() noexcept = default;

    virtual void afterAdmissionBeforeDispatch(
        const Domain::OperationId& operationId) noexcept = 0;
};

struct ManagerStartupComWorkerSnapshot final {
    bool accepting{};
    bool workerRunning{};
    std::size_t activeOperationCount{};
    std::size_t queuedOperationCount{};
};

// Owns one MTA worker. Admission is bounded to one active operation and one
// FIFO successor; execute remains synchronous so the caller retains terminal
// completion ownership through cancellation and deadline draining.
class ManagerStartupComWorker final {
public:
    static constexpr std::size_t MaximumActiveOperationCount = 1U;
    static constexpr std::size_t MaximumQueuedOperationCount = 1U;
    static constexpr auto WorkerStartupTimeout = std::chrono::seconds{5};
    static constexpr auto CancellationDrainTimeout = std::chrono::seconds{5};
    static constexpr auto ShutdownDrainTimeout = std::chrono::seconds{5};

    [[nodiscard]] static Domain::Result<
        std::unique_ptr<ManagerStartupComWorker>>
    create(std::shared_ptr<IManagerStartupComHandler> handler) noexcept;

    [[nodiscard]] static Domain::Result<
        std::unique_ptr<ManagerStartupComWorker>>
    createForTesting(
        std::shared_ptr<IManagerStartupComHandler> handler,
        std::shared_ptr<IManagerStartupComWorkerAdmissionGate>
            admissionGate) noexcept;

    ~ManagerStartupComWorker() noexcept;

    ManagerStartupComWorker(const ManagerStartupComWorker&) = delete;
    ManagerStartupComWorker& operator=(
        const ManagerStartupComWorker&) = delete;
    ManagerStartupComWorker(ManagerStartupComWorker&&) = delete;
    ManagerStartupComWorker& operator=(ManagerStartupComWorker&&) = delete;

    [[nodiscard]] ManagerStartupComResult execute(
        ManagerStartupComRequest request) noexcept;

    void cancel(const Domain::OperationId& operationId) noexcept;

    [[nodiscard]] ManagerStartupComWorkerSnapshot snapshot() const noexcept;

    void shutdown() noexcept;

private:
    class Impl;

    explicit ManagerStartupComWorker(
        std::unique_ptr<Impl> implementation) noexcept;

    std::unique_ptr<Impl> implementation_;
};

} // namespace ForgeConductor::Infrastructure::Windows::Detail
