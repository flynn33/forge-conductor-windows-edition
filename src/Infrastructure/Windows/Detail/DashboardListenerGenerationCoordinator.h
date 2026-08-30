#pragma once

#include "DashboardListenerGeneration.h"

#include "ForgeConductor/Domain/Result.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>

namespace ForgeConductor::Infrastructure::Windows::Detail {

class DashboardOverloadResponderSet;

class IDashboardListenerGenerationOverloadDrainSource {
public:
    virtual ~IDashboardListenerGenerationOverloadDrainSource() noexcept =
        default;

    [[nodiscard]] virtual Domain::Result<void> bindOverloadDrainObserver(
        std::weak_ptr<IDashboardAdmissionOverloadDrainObserver> observer)
        noexcept = 0;
};

class DashboardListenerGenerationOverloadDrainSource final
    : public IDashboardListenerGenerationOverloadDrainSource {
public:
    explicit DashboardListenerGenerationOverloadDrainSource(
        DashboardOverloadResponderSet& overloadResponders) noexcept;

    [[nodiscard]] Domain::Result<void> bindOverloadDrainObserver(
        std::weak_ptr<IDashboardAdmissionOverloadDrainObserver> observer)
        noexcept override;

private:
    DashboardOverloadResponderSet* overloadResponders_{};
};

// Registration boundary kept separate from preparation so deterministic
// tests can prove exact two-router rollback and unregister ordering.
class IDashboardListenerGenerationRegistrationHost {
public:
    virtual ~IDashboardListenerGenerationRegistrationHost() noexcept =
        default;

    [[nodiscard]] virtual Domain::Result<void>
    bindConnectionDrainObserver(
        std::weak_ptr<IDashboardConnectionGenerationDrainObserver> observer)
        noexcept = 0;
    [[nodiscard]] virtual Domain::Result<void> registerCompletionTarget(
        std::shared_ptr<IDashboardListenerGeneration> generation) noexcept = 0;
    [[nodiscard]] virtual Domain::Result<void> registerDeadlineTarget(
        std::shared_ptr<IDashboardListenerGeneration> generation) noexcept = 0;
    [[nodiscard]] virtual bool unregisterDeadlineTarget(
        const std::shared_ptr<IDashboardListenerGeneration>& generation)
        noexcept = 0;
    [[nodiscard]] virtual bool unregisterCompletionTarget(
        const std::shared_ptr<IDashboardListenerGeneration>& generation)
        noexcept = 0;
};

class DashboardListenerGenerationRegistrationHost final
    : public IDashboardListenerGenerationRegistrationHost {
public:
    DashboardListenerGenerationRegistrationHost(
        DashboardIocpCompletionRouter& completionRouter,
        DashboardConnectionRegistry& connectionRegistry) noexcept;

    [[nodiscard]] Domain::Result<void> bindConnectionDrainObserver(
        std::weak_ptr<IDashboardConnectionGenerationDrainObserver> observer)
        noexcept override;
    [[nodiscard]] Domain::Result<void> registerCompletionTarget(
        std::shared_ptr<IDashboardListenerGeneration> generation)
        noexcept override;
    [[nodiscard]] Domain::Result<void> registerDeadlineTarget(
        std::shared_ptr<IDashboardListenerGeneration> generation)
        noexcept override;
    [[nodiscard]] bool unregisterDeadlineTarget(
        const std::shared_ptr<IDashboardListenerGeneration>& generation)
        noexcept override;
    [[nodiscard]] bool unregisterCompletionTarget(
        const std::shared_ptr<IDashboardListenerGeneration>& generation)
        noexcept override;

private:
    DashboardIocpCompletionRouter* completionRouter_{};
    DashboardConnectionRegistry* connectionRegistry_{};
};

// prepareGeneration completes identity allocation, immutable application
// policy construction, native socket creation, bind, listen, exact endpoint
// validation, and accept-owner construction. It must not start admission.
class IDashboardListenerGenerationFactory {
public:
    virtual ~IDashboardListenerGenerationFactory() noexcept = default;

    [[nodiscard]] virtual Domain::Result<std::shared_ptr<
        IDashboardListenerGeneration>>
    prepareGeneration(
        std::shared_ptr<DashboardListenerGenerationTransitionGate>
            transitionGate) noexcept = 0;
};

// Process-wide one-shot edge emitted after graceful or hard shutdown has
// closed coordinator admission and every listener generation has relinquished
// both routing registrations. Composition retains the observer strongly until
// the edge; the coordinator keeps only a weak reference to avoid a cycle. The
// callback is a latch only and must not destroy coordinator dependencies.
class IDashboardListenerGenerationCoordinatorDrainObserver {
public:
    virtual ~IDashboardListenerGenerationCoordinatorDrainObserver()
        noexcept = default;

    virtual void listenerGenerationsMayHaveDrained() noexcept = 0;
};

class DashboardListenerGenerationCoordinatorSnapshot final {
public:
    [[nodiscard]] std::optional<std::uint64_t> activeRegistrationId()
        const noexcept
    {
        return activeRegistrationId_;
    }

    [[nodiscard]] std::optional<std::uint64_t> retiringRegistrationId()
        const noexcept
    {
        return retiringRegistrationId_;
    }

    [[nodiscard]] bool preparationInProgress() const noexcept
    {
        return preparationInProgress_;
    }

    [[nodiscard]] bool collectionInProgress() const noexcept
    {
        return collectionInProgress_;
    }

    [[nodiscard]] bool shutdownRequested() const noexcept
    {
        return shutdownRequested_;
    }

    [[nodiscard]] bool gracefulShutdownRequested() const noexcept
    {
        return gracefulShutdownRequested_;
    }

    [[nodiscard]] bool hardShutdownRequested() const noexcept
    {
        return hardShutdownRequested_;
    }

    [[nodiscard]] bool fatal() const noexcept { return fatal_; }

    [[nodiscard]] bool hasFailure() const noexcept { return hasFailure_; }

    [[nodiscard]] std::uint64_t publicationCount() const noexcept
    {
        return publicationCount_;
    }

    [[nodiscard]] std::uint64_t retirementCount() const noexcept
    {
        return retirementCount_;
    }

private:
    friend class DashboardListenerGenerationCoordinator;

    DashboardListenerGenerationCoordinatorSnapshot(
        std::optional<std::uint64_t> activeRegistrationId,
        std::optional<std::uint64_t> retiringRegistrationId,
        bool preparationInProgress,
        bool collectionInProgress,
        bool shutdownRequested,
        bool gracefulShutdownRequested,
        bool hardShutdownRequested,
        bool fatal,
        bool hasFailure,
        std::uint64_t publicationCount,
        std::uint64_t retirementCount) noexcept;

    std::optional<std::uint64_t> activeRegistrationId_;
    std::optional<std::uint64_t> retiringRegistrationId_;
    bool preparationInProgress_{};
    bool collectionInProgress_{};
    bool shutdownRequested_{};
    bool gracefulShutdownRequested_{};
    bool hardShutdownRequested_{};
    bool fatal_{};
    bool hasFailure_{};
    std::uint64_t publicationCount_{};
    std::uint64_t retirementCount_{};
};

// Process-owned two-generation state machine. Preparation happens outside its
// mutex while one explicit preparing claim blocks concurrent rebinds. The
// shared transition gate provides the new-start-before-old-close ordering and
// prevents successful handoff from crossing publication or force-close.
class DashboardListenerGenerationCoordinator final
    : public IDashboardListenerGenerationDrainObserver,
      public IDashboardConnectionGenerationDrainObserver,
      public IDashboardAdmissionOverloadDrainObserver,
      public std::enable_shared_from_this<
          DashboardListenerGenerationCoordinator> {
public:
    [[nodiscard]] static Domain::Result<std::shared_ptr<
        DashboardListenerGenerationCoordinator>>
    create(
        std::shared_ptr<IDashboardListenerGenerationRegistrationHost>
            registrationHost,
        std::shared_ptr<IDashboardListenerGenerationOverloadDrainSource>
            overloadDrainSource,
        std::shared_ptr<IDashboardListenerGenerationFactory> factory)
        noexcept;

    DashboardListenerGenerationCoordinator(
        const DashboardListenerGenerationCoordinator&) = delete;
    DashboardListenerGenerationCoordinator& operator=(
        const DashboardListenerGenerationCoordinator&) = delete;
    DashboardListenerGenerationCoordinator(
        DashboardListenerGenerationCoordinator&&) = delete;
    DashboardListenerGenerationCoordinator& operator=(
        DashboardListenerGenerationCoordinator&&) = delete;
    ~DashboardListenerGenerationCoordinator() noexcept override;

    [[nodiscard]] Domain::Result<void> startInitial() noexcept;
    [[nodiscard]] Domain::Result<void> rebind() noexcept;

    [[nodiscard]] Domain::Result<void> bindShutdownDrainObserver(
        std::weak_ptr<
            IDashboardListenerGenerationCoordinatorDrainObserver> observer)
        noexcept;

    void beginGracefulShutdown() noexcept;
    void beginShutdown() noexcept;
    void fatal(DWORD nativeError) noexcept;

    void generationMayHaveDrained(
        std::uint64_t registrationId) noexcept override;
    void generationConnectionsMayHaveDrained(
        std::uint64_t generationId) noexcept override;
    void overloadOwnerBeganShutdown() noexcept override;
    void overloadOwnerBecameTerminal() noexcept override;
    void overloadGenerationMayHaveDrained(
        std::uint64_t generationId) noexcept override;
    void overloadGenerationTerminalPending(
        std::uint64_t generationId) noexcept override;
    void overloadGenerationCompletionPending(
        std::uint64_t generationId) noexcept override;
    void overloadGenerationCompletionSettled(
        std::uint64_t generationId) noexcept override;
    void overloadGenerationBecameTerminal(
        std::uint64_t generationId) noexcept override;

    [[nodiscard]] DashboardListenerGenerationCoordinatorSnapshot snapshot()
        const noexcept;
    [[nodiscard]] std::optional<Domain::Error> fullFailure() const;

private:
    enum class ShutdownFanoutAction : std::uint8_t {
        None,
        Graceful,
        Hard,
        Fatal,
    };

    struct GenerationEntry final {
        std::shared_ptr<IDashboardListenerGeneration> generation;
        bool completionRegistered{};
        bool deadlineRegistered{};
    };

    DashboardListenerGenerationCoordinator(
        std::shared_ptr<IDashboardListenerGenerationRegistrationHost>
            registrationHost,
        std::shared_ptr<IDashboardListenerGenerationOverloadDrainSource>
            overloadDrainSource,
        std::shared_ptr<IDashboardListenerGenerationFactory> factory,
        std::shared_ptr<DashboardListenerGenerationTransitionGate>
            transitionGate) noexcept;

    [[nodiscard]] Domain::Result<void> prepareAndRegister(
        GenerationEntry& entry) noexcept;
    void rollbackPrepared(GenerationEntry entry) noexcept;
    void finishPreparation() noexcept;
    void collectDrained() noexcept;
    void ownershipMayHaveDrained(std::uint64_t generationId) noexcept;
    void rememberTerminalPendingLocked(
        std::uint64_t generationId) noexcept;
    [[nodiscard]] bool terminalPendingLocked(
        std::uint64_t generationId) const noexcept;
    [[nodiscard]] bool completionPendingLocked(
        std::uint64_t generationId) const noexcept;
    [[nodiscard]] Domain::Result<void> unregisterEntry(
        GenerationEntry& entry) noexcept;
    void retainUnregisterFailure(
        GenerationEntry entry,
        bool fromRetiring,
        Domain::Error error) noexcept;
    [[nodiscard]] Domain::Result<void> claimPreparation(
        bool requireActive) noexcept;
    [[nodiscard]] DashboardListenerGenerationCoordinatorSnapshot
    snapshotLocked() const noexcept;
    [[nodiscard]] std::shared_ptr<
        IDashboardListenerGenerationCoordinatorDrainObserver>
    takeShutdownDrainObserverLocked() noexcept;
    void notifyShutdownDrainObserverIfReady() noexcept;
    [[nodiscard]] bool claimShutdownFanoutDispatcherLocked() noexcept;
    void dispatchShutdownFanouts() noexcept;

    const std::shared_ptr<IDashboardListenerGenerationRegistrationHost>
        registrationHost_;
    const std::shared_ptr<IDashboardListenerGenerationOverloadDrainSource>
        overloadDrainSource_;
    const std::shared_ptr<IDashboardListenerGenerationFactory> factory_;
    const std::shared_ptr<DashboardListenerGenerationTransitionGate>
        transitionGate_;

    std::optional<GenerationEntry> active_;
    std::optional<GenerationEntry> retiring_;
    std::weak_ptr<IDashboardListenerGenerationCoordinatorDrainObserver>
        shutdownDrainObserver_;
    std::uint64_t publicationCount_{};
    std::uint64_t retirementCount_{};
    bool preparationInProgress_{};
    bool drainCheckPending_{};
    bool collectionInProgress_{};
    bool unregisterFailed_{};
    bool shutdownRequested_{};
    bool gracefulShutdownFanoutStarted_{};
    bool shutdownFanoutStarted_{};
    bool shutdownFanoutDispatchInProgress_{};
    bool gracefulShutdownFanoutCompleted_{};
    bool hardShutdownFanoutCompleted_{};
    bool fatalShutdownFanoutRequested_{};
    bool fatalShutdownFanoutCompleted_{};
    DWORD fatalShutdownNativeError_{};
    bool shutdownDrainObserverEverBound_{};
    bool shutdownDrainNotificationSent_{};
    bool fatal_{};
    std::array<std::uint64_t, 2U> terminalPendingGenerationIds_{};
    std::size_t terminalPendingGenerationCount_{};
    std::array<
        std::uint64_t,
        2U * DashboardAcceptSlotSet::SlotCount>
        completionPendingGenerationIds_{};
    std::size_t completionPendingGenerationCount_{};
    std::optional<Domain::Error> firstFailure_;
    mutable std::mutex mutex_;
};

} // namespace ForgeConductor::Infrastructure::Windows::Detail
