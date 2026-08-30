#pragma once

#include "ForgeConductor/Contracts/IClientPresenceRepository.h"
#include "ForgeConductor/Contracts/IDiagnosticsServices.h"
#include "ForgeConductor/Contracts/IFoundationServices.h"

#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <thread>

namespace ForgeConductor::Application {

struct ClientPresenceTiming final {
    std::chrono::milliseconds heartbeatInterval{
        std::chrono::seconds{10}};
    std::chrono::milliseconds operationTimeout{
        std::chrono::seconds{2}};
};

// Application-owned orchestration for one process presence identity. The
// injected services are retained by shared ownership so the worker cannot
// outlive a borrowed repository, clock, UUID source, or ownership registry.
// The lifecycle owns exactly one stop-aware worker thread and joins it before
// issuing the final compare-and-delete.
class ClientPresenceLifecycle final {
public:
    static constexpr auto DefaultHeartbeatInterval =
        std::chrono::seconds{10};
    static constexpr auto DefaultOperationTimeout =
        std::chrono::seconds{2};

    ClientPresenceLifecycle(
        std::shared_ptr<Contracts::IClientPresenceRepository> repository,
        std::shared_ptr<Contracts::IClock> clock,
        std::shared_ptr<Contracts::IUuidGenerator> uuidGenerator,
        std::shared_ptr<Contracts::IRuntimeDiagnostics> runtimeDiagnostics,
        ClientPresenceTiming timing = {});
    ~ClientPresenceLifecycle() noexcept;

    ClientPresenceLifecycle(const ClientPresenceLifecycle&) = delete;
    ClientPresenceLifecycle& operator=(const ClientPresenceLifecycle&) = delete;
    ClientPresenceLifecycle(ClientPresenceLifecycle&&) = delete;
    ClientPresenceLifecycle& operator=(ClientPresenceLifecycle&&) = delete;

    [[nodiscard]] Domain::Result<void> start(
        Domain::ClientPresenceIdentity identity,
        Domain::PathText workingDirectory,
        const Domain::OperationContext& context) noexcept;

    [[nodiscard]] Domain::Result<void> stop(
        const Domain::OperationContext& context) noexcept;

private:
    enum class State {
        Idle,
        Starting,
        Running,
        Stopping,
        Stopped
    };

    [[nodiscard]] Domain::Result<Domain::OperationContext> makeWorkerContext(
        std::stop_token cancellation) noexcept;
    [[nodiscard]] Domain::Result<Domain::OperationContext> makeCleanupContext()
        noexcept;
    void workerLoop(
        Domain::ClientPresenceIdentity identity,
        Domain::PathText workingDirectory,
        std::stop_token cancellation,
        Contracts::RuntimeOwnershipLease ownershipLease) noexcept;
    [[nodiscard]] bool stopWorker() noexcept;
    void shutdown() noexcept;

    std::shared_ptr<Contracts::IClientPresenceRepository> repository_;
    std::shared_ptr<Contracts::IClock> clock_;
    std::shared_ptr<Contracts::IUuidGenerator> uuidGenerator_;
    std::shared_ptr<Contracts::IRuntimeDiagnostics> runtimeDiagnostics_;
    ClientPresenceTiming timing_;
    Domain::CorrelationId correlationId_;

    std::mutex apiMutex_;
    std::mutex waitMutex_;
    std::condition_variable_any wake_;
    std::optional<Domain::ClientPresenceIdentity> identity_;
    std::optional<Domain::PathText> workingDirectory_;
    State state_{State::Idle};
    bool startPublished_{};
    bool registered_{};
    bool stopRequested_{};
    // Declared last so automatic destruction requests stop and joins before
    // any predicate state, mutex, identity, or injected dependency is torn
    // down if an earlier explicit join could not be confirmed.
    std::jthread worker_;
};

} // namespace ForgeConductor::Application
