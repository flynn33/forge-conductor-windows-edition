#include "ForgeConductor/Application/ClientPresenceLifecycle.h"

#include <stdexcept>
#include <string>
#include <utility>

namespace ForgeConductor::Application {
namespace {

constexpr auto MaximumHeartbeatInterval = std::chrono::minutes{5};
constexpr auto MaximumOperationTimeout = std::chrono::seconds{30};
constexpr std::string_view PresenceCorrelationId{"mcp-presence-heartbeat"};

[[nodiscard]] Domain::Error internalError(std::string message)
{
    return Domain::makeError(
        Domain::ErrorCodes::InternalFailure, std::move(message));
}

[[nodiscard]] Domain::CorrelationId parseCorrelationId()
{
    auto parsed = Domain::CorrelationId::parse(PresenceCorrelationId);
    if (!parsed) {
        throw std::invalid_argument(
            "The client presence correlation identifier is invalid.");
    }
    return std::move(parsed).value();
}

} // namespace

ClientPresenceLifecycle::ClientPresenceLifecycle(
    std::shared_ptr<Contracts::IClientPresenceRepository> repository,
    std::shared_ptr<Contracts::IClock> clock,
    std::shared_ptr<Contracts::IUuidGenerator> uuidGenerator,
    std::shared_ptr<Contracts::IRuntimeDiagnostics> runtimeDiagnostics,
    const ClientPresenceTiming timing)
    : repository_{std::move(repository)},
      clock_{std::move(clock)},
      uuidGenerator_{std::move(uuidGenerator)},
      runtimeDiagnostics_{std::move(runtimeDiagnostics)}, timing_{timing},
      correlationId_{parseCorrelationId()}
{
    if (!repository_ || !clock_ || !uuidGenerator_ || !runtimeDiagnostics_) {
        throw std::invalid_argument(
            "The client presence lifecycle requires every injected service.");
    }
    if (timing_.heartbeatInterval <= std::chrono::milliseconds::zero() ||
        timing_.heartbeatInterval > MaximumHeartbeatInterval) {
        throw std::invalid_argument(
            "The client presence heartbeat interval must be within (0, 5 minutes].");
    }
    if (timing_.operationTimeout <= std::chrono::milliseconds::zero() ||
        timing_.operationTimeout > MaximumOperationTimeout) {
        throw std::invalid_argument(
            "The client presence operation timeout must be within (0, 30 seconds].");
    }
}

ClientPresenceLifecycle::~ClientPresenceLifecycle() noexcept
{
    shutdown();
}

Domain::Result<void> ClientPresenceLifecycle::start(
    Domain::ClientPresenceIdentity identity,
    Domain::PathText workingDirectory,
    const Domain::OperationContext& context) noexcept
{
    try {
        std::lock_guard apiLock{apiMutex_};
        if (state_ != State::Idle) {
            return Domain::Result<void>::failure(Domain::makeError(
                Domain::ErrorCodes::Conflict,
                "The client presence lifecycle has already started or stopped."));
        }
        auto valid = Domain::validateClientPresenceIdentity(identity);
        if (!valid) {
            return valid;
        }
        auto validWorkingDirectory =
            Domain::PathText::create(workingDirectory.value());
        if (!validWorkingDirectory) {
            return Domain::Result<void>::failure(
                std::move(validWorkingDirectory).error());
        }
        workingDirectory = std::move(validWorkingDirectory).value();
        state_ = State::Starting;

        auto ownership = runtimeDiagnostics_->acquire(
            Contracts::RuntimeOwnerKind::BackgroundThread, context);
        if (!ownership) {
            state_ = State::Idle;
            return Domain::Result<void>::failure(std::move(ownership).error());
        }

        {
            std::lock_guard waitLock{waitMutex_};
            startPublished_ = false;
            registered_ = false;
            stopRequested_ = false;
        }
        identity_.emplace(identity);
        workingDirectory_.emplace(workingDirectory);
        try {
            worker_ = std::jthread{
                [this, workerIdentity = std::move(identity),
                 workerDirectory = std::move(workingDirectory),
                 lease = std::move(ownership).value()](
                    const std::stop_token cancellation) mutable noexcept {
                    workerLoop(
                        std::move(workerIdentity), std::move(workerDirectory), cancellation,
                        std::move(lease));
                }};
        } catch (...) {
            workingDirectory_.reset();
            identity_.reset();
            state_ = State::Idle;
            return Domain::Result<void>::failure(internalError(
                "The client presence heartbeat thread could not be created."));
        }

        const auto now = clock_->utcNow();
        auto registered = repository_->upsert(
            Domain::ClientPresenceRegistration{
                *identity_, *workingDirectory_, now, now},
            context);
        {
            std::lock_guard waitLock{waitMutex_};
            // Presence storage is deliberately best-effort so a transiently
            // locked store never blocks the MCP handshake. A failed or
            // ambiguously committed initial upsert is retried by the owned
            // worker at the next bounded interval.
            registered_ = registered.hasValue();
            startPublished_ = true;
        }
        state_ = State::Running;
        wake_.notify_all();
        return Domain::Result<void>::success();
    } catch (...) {
        static_cast<void>(stopWorker());
        state_ = identity_ ? State::Stopping : State::Idle;
        return Domain::Result<void>::failure(internalError(
            "The client presence lifecycle could not start safely."));
    }
}

Domain::Result<void> ClientPresenceLifecycle::stop(
    const Domain::OperationContext& context) noexcept
{
    try {
        std::lock_guard apiLock{apiMutex_};
        if (state_ == State::Stopped || state_ == State::Idle) {
            state_ = State::Stopped;
            return Domain::Result<void>::success();
        }
        state_ = State::Stopping;
        if (!stopWorker()) {
            return Domain::Result<void>::failure(internalError(
                "The client presence heartbeat worker could not be joined."));
        }

        if (identity_) {
            auto removed = repository_->remove(*identity_, context);
            if (!removed) {
                // The worker is already joined, but the exact owner must stay
                // available for a later explicit stop or destructor cleanup
                // attempt after a transient/cancelled repository failure.
                state_ = State::Stopping;
                return Domain::Result<void>::failure(
                    std::move(removed).error());
            }
        }
        workingDirectory_.reset();
        identity_.reset();
        state_ = State::Stopped;
        return Domain::Result<void>::success();
    } catch (...) {
        static_cast<void>(stopWorker());
        state_ = identity_ ? State::Stopping : State::Stopped;
        return Domain::Result<void>::failure(internalError(
            "The client presence lifecycle could not stop safely."));
    }
}

Domain::Result<Domain::OperationContext>
ClientPresenceLifecycle::makeWorkerContext(
    const std::stop_token cancellation) noexcept
{
    try {
        auto generated = uuidGenerator_->next();
        if (!generated) {
            return Domain::Result<Domain::OperationContext>::failure(
                std::move(generated).error());
        }
        return Domain::Result<Domain::OperationContext>::success(
            Domain::OperationContext{
                Domain::OperationId{std::move(generated).value()},
                clock_->monotonicNow() + timing_.operationTimeout,
                cancellation,
                correlationId_});
    } catch (...) {
        return Domain::Result<Domain::OperationContext>::failure(internalError(
            "A client presence heartbeat context could not be created."));
    }
}

Domain::Result<Domain::OperationContext>
ClientPresenceLifecycle::makeCleanupContext() noexcept
{
    return makeWorkerContext(std::stop_token{});
}

void ClientPresenceLifecycle::workerLoop(
    Domain::ClientPresenceIdentity identity,
    Domain::PathText workingDirectory,
    const std::stop_token cancellation,
    Contracts::RuntimeOwnershipLease ownershipLease) noexcept
{
    static_cast<void>(ownershipLease);
    try {
        bool registered{};
        {
            std::unique_lock waitLock{waitMutex_};
            static_cast<void>(wake_.wait(
                waitLock,
                cancellation,
                [this] { return startPublished_ || stopRequested_; }));
            if (cancellation.stop_requested() || stopRequested_ ||
                !startPublished_) {
                return;
            }
            registered = registered_;
        }

        while (!cancellation.stop_requested()) {
            {
                std::unique_lock waitLock{waitMutex_};
                const bool stopped = wake_.wait_for(
                    waitLock,
                    cancellation,
                    timing_.heartbeatInterval,
                    [this] { return stopRequested_; });
                if (stopped || cancellation.stop_requested() || stopRequested_) {
                    return;
                }
            }

            auto context = makeWorkerContext(cancellation);
            if (!context) {
                continue;
            }
            if (!registered) {
                const auto now = clock_->utcNow();
                auto registration = repository_->upsert(
                    Domain::ClientPresenceRegistration{
                        identity, workingDirectory, now, now},
                    context.value());
                if (registration) {
                    registered = true;
                }
                continue;
            }
            auto heartbeat = repository_->heartbeat(
                identity, clock_->utcNow(), context.value());
            if (heartbeat && !heartbeat.value()) {
                return;
            }
            // Transient typed failures are retried at the next interval. Calls
            // are synchronous, so no heartbeat can overlap its predecessor.
        }
    } catch (...) {
        // The background boundary is fail-safe. The exact owner remains for
        // stop() to remove, and the ownership lease releases with this frame.
    }
}

bool ClientPresenceLifecycle::stopWorker() noexcept
{
    try {
        std::lock_guard waitLock{waitMutex_};
        stopRequested_ = true;
    } catch (...) {
    }
    worker_.request_stop();
    wake_.notify_all();
    try {
        if (worker_.joinable()) {
            worker_.join();
        }
    } catch (...) {
        return false;
    }
    return !worker_.joinable();
}

void ClientPresenceLifecycle::shutdown() noexcept
{
    try {
        std::lock_guard apiLock{apiMutex_};
        if (state_ == State::Stopped || state_ == State::Idle) {
            state_ = State::Stopped;
            return;
        }
        state_ = State::Stopping;
        if (!stopWorker()) {
            return;
        }
        if (identity_) {
            auto context = makeCleanupContext();
            if (context) {
                static_cast<void>(repository_->remove(
                    *identity_, context.value()));
            }
        }
        workingDirectory_.reset();
        identity_.reset();
        state_ = State::Stopped;
    } catch (...) {
        static_cast<void>(stopWorker());
        state_ = identity_ ? State::Stopping : State::Stopped;
    }
}

} // namespace ForgeConductor::Application
