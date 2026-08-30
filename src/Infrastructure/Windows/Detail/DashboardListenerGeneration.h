#pragma once

#include "DashboardAcceptSlotSet.h"
#include "DashboardAcceptedConnectionHandoff.h"
#include "DashboardConnectionRegistry.h"
#include "DashboardConnectionRuntimeServices.h"
#include "DashboardIocpCompletionRouter.h"
#include "DashboardListenerCompletionKeyLease.h"

#include "ForgeConductor/Dashboard/IDashboardConnectionApplication.h"
#include "ForgeConductor/Domain/Result.h"
#include "ForgeConductor/Infrastructure/Windows/WindowsDashboardDeadlineScheduler.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <utility>

namespace ForgeConductor::Infrastructure::Windows::Detail {

// One process-shared transition gate serializes successful accept handoff
// against publication, retirement, and the exact retirement force-close
// snapshot. Its critical sections contain only bounded native state changes.
class DashboardListenerGenerationTransitionGate final {
public:
    class Guard final {
    public:
        Guard(const Guard&) = delete;
        Guard& operator=(const Guard&) = delete;
        Guard(Guard&& other) noexcept
            : owner_{std::exchange(other.owner_, nullptr)},
              lock_{std::move(other.lock_)},
              deferredAction_{
                  std::exchange(other.deferredAction_, nullptr)},
              deferredOwner_{std::move(other.deferredOwner_)}
        {
        }
        Guard& operator=(Guard&&) = delete;
        ~Guard() noexcept
        {
            const auto action = deferredAction_;
            auto retainedOwner = std::move(deferredOwner_);
            if (lock_.owns_lock()) {
                lock_.unlock();
            }
            if (action != nullptr) {
                action(retainedOwner.get());
            }
        }

        [[nodiscard]] bool belongsTo(
            const DashboardListenerGenerationTransitionGate& owner)
            const noexcept
        {
            return owner_ == std::addressof(owner);
        }

    private:
        friend class DashboardListenerGenerationTransitionGate;
        friend class DashboardListenerGeneration;

        using DeferredAction = void (*)(void*) noexcept;

        Guard(
            const DashboardListenerGenerationTransitionGate& owner,
            std::mutex& mutex) noexcept
            : owner_{std::addressof(owner)}, lock_{mutex}
        {
        }

        void deferAfterRelease(
            const DeferredAction action,
            std::shared_ptr<void> owner) noexcept
        {
            if (action == nullptr || owner == nullptr) {
                std::terminate();
            }
            if (deferredAction_ != nullptr) {
                if (deferredAction_ == action &&
                    deferredOwner_.get() == owner.get()) {
                    return;
                }
                std::terminate();
            }
            deferredAction_ = action;
            deferredOwner_ = std::move(owner);
        }

        const DashboardListenerGenerationTransitionGate* owner_{};
        std::unique_lock<std::mutex> lock_;
        DeferredAction deferredAction_{};
        std::shared_ptr<void> deferredOwner_;
    };

    [[nodiscard]] Guard enter() noexcept { return Guard{*this, mutex_}; }

private:
    std::mutex mutex_;
};

// Narrow generation-scoped connection control. The production adapter keeps
// the connection registry as the sole owner of connection iteration.
class IDashboardListenerGenerationConnectionControl {
public:
    virtual ~IDashboardListenerGenerationConnectionControl() noexcept =
        default;

    [[nodiscard]] virtual std::size_t connectionCountForGeneration(
        std::uint64_t generationId) const noexcept = 0;
    virtual void beginShutdownGeneration(
        std::uint64_t generationId) noexcept = 0;
};

class DashboardConnectionRegistryGenerationControl final
    : public IDashboardListenerGenerationConnectionControl {
public:
    explicit DashboardConnectionRegistryGenerationControl(
        DashboardConnectionRegistry& registry) noexcept;

    [[nodiscard]] std::size_t connectionCountForGeneration(
        std::uint64_t generationId) const noexcept override;
    void beginShutdownGeneration(
        std::uint64_t generationId) noexcept override;

private:
    DashboardConnectionRegistry* registry_{};
};

class IDashboardListenerGenerationDrainObserver {
public:
    virtual ~IDashboardListenerGenerationDrainObserver() noexcept = default;
    virtual void generationMayHaveDrained(
        std::uint64_t registrationId) noexcept = 0;
};

// Testable boundary around the exact four-slot accept owner. The production
// implementation below owns the slot set, handoff, and immutable application
// policy as one indivisible listener-generation lifetime.
class IDashboardListenerGenerationAcceptOwner {
public:
    virtual ~IDashboardListenerGenerationAcceptOwner() noexcept = default;

    [[nodiscard]] virtual DashboardConnectionRuntimeIdentity identity()
        const noexcept = 0;

    [[nodiscard]] virtual Domain::Result<void> start() noexcept = 0;
    [[nodiscard]] virtual Domain::Result<void> closeAdmission() noexcept = 0;
    [[nodiscard]] virtual Domain::Result<void>
    forceCloseListener() noexcept = 0;
    [[nodiscard]] virtual Domain::Result<void> consume(
        DashboardIoCompletionPacket packet,
        DWORD nativeError) noexcept = 0;
    [[nodiscard]] virtual bool fullyDrained() const noexcept = 0;
};

class DashboardListenerGenerationAcceptOwner final
    : public IDashboardListenerGenerationAcceptOwner {
public:
    [[nodiscard]] static Domain::Result<std::unique_ptr<
        DashboardListenerGenerationAcceptOwner>>
    create(
        DashboardConnectionRuntimeIdentity identity,
        std::unique_ptr<DashboardAcceptSlotSet> acceptSlots,
        std::unique_ptr<DashboardAcceptedConnectionHandoff> handoff,
        std::shared_ptr<Dashboard::IDashboardConnectionApplication>
            applicationPolicy,
        DashboardIocpWorkerKernel& kernel) noexcept;

    DashboardListenerGenerationAcceptOwner(
        const DashboardListenerGenerationAcceptOwner&) = delete;
    DashboardListenerGenerationAcceptOwner& operator=(
        const DashboardListenerGenerationAcceptOwner&) = delete;
    DashboardListenerGenerationAcceptOwner(
        DashboardListenerGenerationAcceptOwner&&) = delete;
    DashboardListenerGenerationAcceptOwner& operator=(
        DashboardListenerGenerationAcceptOwner&&) = delete;
    ~DashboardListenerGenerationAcceptOwner() noexcept override = default;

    [[nodiscard]] DashboardConnectionRuntimeIdentity identity()
        const noexcept override
    {
        return identity_;
    }

    [[nodiscard]] Domain::Result<void> start() noexcept override;
    [[nodiscard]] Domain::Result<void> closeAdmission() noexcept override;
    [[nodiscard]] Domain::Result<void> forceCloseListener()
        noexcept override;
    [[nodiscard]] Domain::Result<void> consume(
        DashboardIoCompletionPacket packet,
        DWORD nativeError) noexcept override;
    [[nodiscard]] bool fullyDrained() const noexcept override;

private:
    DashboardListenerGenerationAcceptOwner(
        DashboardConnectionRuntimeIdentity identity,
        std::unique_ptr<DashboardAcceptSlotSet> acceptSlots,
        std::unique_ptr<DashboardAcceptedConnectionHandoff> handoff,
        std::shared_ptr<Dashboard::IDashboardConnectionApplication>
            applicationPolicy,
        DashboardIocpWorkerKernel& kernel) noexcept;

    const DashboardConnectionRuntimeIdentity identity_;
    std::unique_ptr<DashboardAcceptSlotSet> acceptSlots_;
    std::unique_ptr<DashboardAcceptedConnectionHandoff> handoff_;
    const std::shared_ptr<Dashboard::IDashboardConnectionApplication>
        applicationPolicy_;
    DashboardIocpWorkerKernel* kernel_{};
};

// Process boundary used only after the exact listener handle was closed and
// its bounded cancellation-reap watchdog expired. Production terminates;
// focused tests inject a nonterminating allocation-free recorder.
class IDashboardListenerGenerationFailFast {
public:
    virtual ~IDashboardListenerGenerationFailFast() noexcept = default;
    virtual void failFast() noexcept = 0;
};

// Narrow scheduling boundary retained by each generation. Production wraps
// the process scheduler; deterministic tests can stop precisely between an
// arm becoming live and its value returning to the generation owner.
class IDashboardListenerGenerationDeadlineScheduler {
public:
    virtual ~IDashboardListenerGenerationDeadlineScheduler() noexcept =
        default;

    [[nodiscard]] virtual Domain::Result<WindowsDashboardDeadline> schedule(
        WindowsDashboardDeadlineRequest request) noexcept = 0;
    [[nodiscard]] virtual bool cancel(
        std::uint64_t registrationId,
        std::uint64_t armSequence) noexcept = 0;
};

enum class DashboardListenerGenerationLifecycle : std::uint8_t {
    Prepared,
    Admitting,
    Retiring,
    ShuttingDown,
    Fatal,
    Drained,
};

class DashboardListenerGenerationSnapshot final {
public:
    [[nodiscard]] DashboardListenerGenerationLifecycle lifecycle()
        const noexcept
    {
        return lifecycle_;
    }

    [[nodiscard]] std::uint64_t registrationId() const noexcept
    {
        return registrationId_;
    }

    [[nodiscard]] DashboardIoCompletionKey completionKey() const noexcept
    {
        return completionKey_;
    }

    [[nodiscard]] const WindowsDashboardDeadline* retirementDeadline()
        const noexcept
    {
        return retirementDeadline_.has_value()
            ? std::addressof(*retirementDeadline_)
            : nullptr;
    }

    [[nodiscard]] const WindowsDashboardDeadline*
    cancellationReapDeadline() const noexcept
    {
        return cancellationReapDeadline_.has_value()
            ? std::addressof(*cancellationReapDeadline_)
            : nullptr;
    }

    [[nodiscard]] std::uint64_t completionCount() const noexcept
    {
        return completionCount_;
    }

    [[nodiscard]] std::uint64_t staleDeadlineCount() const noexcept
    {
        return staleDeadlineCount_;
    }

    [[nodiscard]] bool retirementCancellationRequested() const noexcept
    {
        return retirementCancellationRequested_;
    }

    [[nodiscard]] bool hasFailure() const noexcept { return hasFailure_; }

    [[nodiscard]] bool listenerForceCloseRequested() const noexcept
    {
        return listenerForceCloseRequested_;
    }

    [[nodiscard]] std::uint64_t failFastCount() const noexcept
    {
        return failFastCount_;
    }

private:
    friend class DashboardListenerGeneration;

    DashboardListenerGenerationSnapshot(
        DashboardListenerGenerationLifecycle lifecycle,
        std::uint64_t registrationId,
        DashboardIoCompletionKey completionKey,
        std::optional<WindowsDashboardDeadline> retirementDeadline,
        std::optional<WindowsDashboardDeadline> cancellationReapDeadline,
        std::uint64_t completionCount,
        std::uint64_t staleDeadlineCount,
        bool retirementCancellationRequested,
        bool listenerForceCloseRequested,
        std::uint64_t failFastCount,
        bool hasFailure) noexcept;

    DashboardListenerGenerationLifecycle lifecycle_{
        DashboardListenerGenerationLifecycle::Prepared};
    std::uint64_t registrationId_{};
    DashboardIoCompletionKey completionKey_{0U};
    std::optional<WindowsDashboardDeadline> retirementDeadline_;
    std::optional<WindowsDashboardDeadline> cancellationReapDeadline_;
    std::uint64_t completionCount_{};
    std::uint64_t staleDeadlineCount_{};
    bool retirementCancellationRequested_{};
    bool listenerForceCloseRequested_{};
    std::uint64_t failFastCount_{};
    bool hasFailure_{};
};

class IDashboardListenerGeneration
    : public IDashboardFixedIocpCompletionTarget,
      public IDashboardAuxiliaryDeadlineTarget {
public:
    ~IDashboardListenerGeneration() noexcept override = default;

    [[nodiscard]] virtual Domain::Result<void> bindDrainObserver(
        std::weak_ptr<IDashboardListenerGenerationDrainObserver> observer)
        noexcept = 0;
    [[nodiscard]] virtual Domain::Result<void> startAdmission(
        DashboardListenerGenerationTransitionGate::Guard& transition)
        noexcept = 0;
    [[nodiscard]] virtual Domain::Result<void> beginRetirement(
        DashboardListenerGenerationTransitionGate::Guard& transition)
        noexcept = 0;
    [[nodiscard]] virtual Domain::Result<void> beginGracefulShutdown(
        DashboardListenerGenerationTransitionGate::Guard& transition)
        noexcept = 0;
    [[nodiscard]] virtual bool fullyDrained() const noexcept = 0;
    virtual void ownershipMayHaveDrained() noexcept = 0;
    virtual void beginShutdown() noexcept override = 0;
};

// Exact owner for one prepared, immutable listener generation. The owner is
// registered with both completion and deadline routers before startAdmission
// and remains registered until accept and connection ownership both drain.
class DashboardListenerGeneration final
    : public IDashboardListenerGeneration,
      public std::enable_shared_from_this<DashboardListenerGeneration> {
public:
    static constexpr auto RetirementLifetime = std::chrono::seconds{5};
    static constexpr auto CancellationReapLifetime =
        std::chrono::seconds{5};

    [[nodiscard]] static Domain::Result<std::shared_ptr<
        DashboardListenerGeneration>>
    create(
        DashboardConnectionRuntimeIdentity identity,
        DashboardListenerCompletionKeyLease completionKeyLease,
        std::unique_ptr<IDashboardListenerGenerationAcceptOwner> acceptOwner,
        WindowsDashboardDeadlineScheduler& deadlineScheduler,
        DashboardConnectionRuntimeServices& runtimeServices,
        std::shared_ptr<IDashboardListenerGenerationConnectionControl>
            connectionControl,
        std::shared_ptr<IDashboardAdmissionOverloadResponder>
            overloadResponder,
        std::shared_ptr<DashboardListenerGenerationTransitionGate>
            transitionGate) noexcept;

    // Focused test seam for generations that do not exercise fixed-key lease
    // ownership.
    [[nodiscard]] static Domain::Result<std::shared_ptr<
        DashboardListenerGeneration>>
    create(
        DashboardConnectionRuntimeIdentity identity,
        std::unique_ptr<IDashboardListenerGenerationAcceptOwner> acceptOwner,
        WindowsDashboardDeadlineScheduler& deadlineScheduler,
        DashboardConnectionRuntimeServices& runtimeServices,
        std::shared_ptr<IDashboardListenerGenerationConnectionControl>
            connectionControl,
        std::shared_ptr<IDashboardAdmissionOverloadResponder>
            overloadResponder,
        std::shared_ptr<DashboardListenerGenerationTransitionGate>
            transitionGate) noexcept;

    // Focused test seam for the process fail-fast boundary.
    [[nodiscard]] static Domain::Result<std::shared_ptr<
        DashboardListenerGeneration>>
    create(
        DashboardConnectionRuntimeIdentity identity,
        std::unique_ptr<IDashboardListenerGenerationAcceptOwner> acceptOwner,
        WindowsDashboardDeadlineScheduler& deadlineScheduler,
        DashboardConnectionRuntimeServices& runtimeServices,
        std::shared_ptr<IDashboardListenerGenerationConnectionControl>
            connectionControl,
        std::shared_ptr<IDashboardAdmissionOverloadResponder>
            overloadResponder,
        std::shared_ptr<DashboardListenerGenerationTransitionGate>
            transitionGate,
        std::shared_ptr<IDashboardListenerGenerationFailFast>
            failFast) noexcept;

    // Focused deterministic seam for scheduling publication and failure
    // races. The scheduler boundary is retained for the generation lifetime.
    [[nodiscard]] static Domain::Result<std::shared_ptr<
        DashboardListenerGeneration>>
    create(
        DashboardConnectionRuntimeIdentity identity,
        std::unique_ptr<IDashboardListenerGenerationAcceptOwner> acceptOwner,
        std::shared_ptr<IDashboardListenerGenerationDeadlineScheduler>
            deadlineScheduler,
        DashboardConnectionRuntimeServices& runtimeServices,
        std::shared_ptr<IDashboardListenerGenerationConnectionControl>
            connectionControl,
        std::shared_ptr<IDashboardAdmissionOverloadResponder>
            overloadResponder,
        std::shared_ptr<DashboardListenerGenerationTransitionGate>
            transitionGate,
        std::shared_ptr<IDashboardListenerGenerationFailFast>
            failFast) noexcept;

    DashboardListenerGeneration(const DashboardListenerGeneration&) = delete;
    DashboardListenerGeneration& operator=(
        const DashboardListenerGeneration&) = delete;
    DashboardListenerGeneration(DashboardListenerGeneration&&) = delete;
    DashboardListenerGeneration& operator=(
        DashboardListenerGeneration&&) = delete;
    ~DashboardListenerGeneration() noexcept override;

    [[nodiscard]] DashboardIoCompletionKey completionKey()
        const noexcept override;
    [[nodiscard]] std::uint64_t registrationId() const noexcept override;

    [[nodiscard]] Domain::Result<void> bindDrainObserver(
        std::weak_ptr<IDashboardListenerGenerationDrainObserver> observer)
        noexcept override;
    [[nodiscard]] Domain::Result<void> startAdmission(
        DashboardListenerGenerationTransitionGate::Guard& transition)
        noexcept override;
    [[nodiscard]] Domain::Result<void> beginRetirement(
        DashboardListenerGenerationTransitionGate::Guard& transition)
        noexcept override;
    [[nodiscard]] Domain::Result<void> beginGracefulShutdown(
        DashboardListenerGenerationTransitionGate::Guard& transition)
        noexcept override;

    void consume(
        DashboardIoCompletionPacket packet,
        DWORD nativeError) noexcept override;
    void dispatchDeadline(
        WindowsDashboardDeadline deadline) noexcept override;
    void fatal(DWORD nativeError) noexcept override;
    void beginShutdown() noexcept override;

    [[nodiscard]] bool fullyDrained() const noexcept override;
    void ownershipMayHaveDrained() noexcept override;
    [[nodiscard]] DashboardListenerGenerationSnapshot snapshot()
        const noexcept;
    [[nodiscard]] std::optional<Domain::Error> fullFailure() const;

private:
    [[nodiscard]] static Domain::Result<std::shared_ptr<
        DashboardListenerGeneration>>
    createInternal(
        std::optional<DashboardListenerCompletionKeyLease>
            completionKeyLease,
        DashboardConnectionRuntimeIdentity identity,
        std::unique_ptr<IDashboardListenerGenerationAcceptOwner> acceptOwner,
        std::shared_ptr<IDashboardListenerGenerationDeadlineScheduler>
            deadlineScheduler,
        DashboardConnectionRuntimeServices& runtimeServices,
        std::shared_ptr<IDashboardListenerGenerationConnectionControl>
            connectionControl,
        std::shared_ptr<IDashboardAdmissionOverloadResponder>
            overloadResponder,
        std::shared_ptr<DashboardListenerGenerationTransitionGate>
            transitionGate,
        std::shared_ptr<IDashboardListenerGenerationFailFast>
            failFast) noexcept;

    DashboardListenerGeneration(
        std::optional<DashboardListenerCompletionKeyLease>
            completionKeyLease,
        DashboardConnectionRuntimeIdentity identity,
        std::unique_ptr<IDashboardListenerGenerationAcceptOwner> acceptOwner,
        std::shared_ptr<IDashboardListenerGenerationDeadlineScheduler>
            deadlineScheduler,
        DashboardConnectionRuntimeServices& runtimeServices,
        std::shared_ptr<IDashboardListenerGenerationConnectionControl>
            connectionControl,
        std::shared_ptr<IDashboardAdmissionOverloadResponder>
            overloadResponder,
        std::shared_ptr<DashboardListenerGenerationTransitionGate>
            transitionGate,
        std::shared_ptr<IDashboardListenerGenerationFailFast>
            failFast) noexcept;

    void retainFailure(Domain::Error error) noexcept;
    void closeAcceptAdmission() noexcept;
    void forceCloseAcceptAndArmWatchdog(
        Domain::MonotonicTimePoint watchdogBase) noexcept;
    void requestFailFast() noexcept;
    void completeTransitionWork() noexcept;
    void deferTransitionWork(
        DashboardListenerGenerationTransitionGate::Guard& transition)
        noexcept;
    static void completeTransitionWorkAfterRelease(void* owner) noexcept;
    void closeAdmissionAndConnections(
        DashboardListenerGenerationLifecycle lifecycle) noexcept;
    void tryCompleteDrain() noexcept;
    void notifyDrainObserver(
        std::shared_ptr<IDashboardListenerGenerationDrainObserver> observer)
        noexcept;

    // Members are destroyed in reverse declaration order. Keep the lease
    // first so every other generation-owned callback dependency is gone
    // before its fixed listener key can return to the pool.
    std::optional<DashboardListenerCompletionKeyLease> completionKeyLease_;
    const DashboardConnectionRuntimeIdentity identity_;
    std::unique_ptr<IDashboardListenerGenerationAcceptOwner> acceptOwner_;
    const std::shared_ptr<IDashboardListenerGenerationDeadlineScheduler>
        deadlineScheduler_;
    DashboardConnectionRuntimeServices* runtimeServices_{};
    const std::shared_ptr<IDashboardListenerGenerationConnectionControl>
        connectionControl_;
    const std::shared_ptr<IDashboardAdmissionOverloadResponder>
        overloadResponder_;
    const std::shared_ptr<DashboardListenerGenerationTransitionGate>
        transitionGate_;
    const std::shared_ptr<IDashboardListenerGenerationFailFast> failFast_;

    DashboardListenerGenerationLifecycle lifecycle_{
        DashboardListenerGenerationLifecycle::Prepared};
    std::optional<WindowsDashboardDeadline> retirementDeadline_;
    std::optional<WindowsDashboardDeadline> cancellationReapDeadline_;
    std::optional<Domain::Error> firstFailure_;
    std::weak_ptr<IDashboardListenerGenerationDrainObserver> drainObserver_;
    std::stop_source retirementCancellation_;
    std::uint64_t completionCount_{};
    std::uint64_t staleDeadlineCount_{};
    std::uint64_t failFastCount_{};
    bool listenerForceCloseRequested_{};
    bool cancellationReapWatchdogStarted_{};
    bool acceptOwnershipDrained_{};
    bool failFastPending_{};
    bool observerBound_{};
    bool drainNotificationSent_{};
    bool gracefulShutdownRequested_{};
    mutable std::mutex mutex_;
};

static_assert(
    DashboardListenerGeneration::RetirementLifetime ==
    std::chrono::seconds{5});
static_assert(
    DashboardListenerGeneration::CancellationReapLifetime ==
    std::chrono::seconds{5});

} // namespace ForgeConductor::Infrastructure::Windows::Detail
