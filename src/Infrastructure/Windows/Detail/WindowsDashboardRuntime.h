#pragma once

#include "ForgeConductor/Contracts/IFoundationServices.h"
#include "ForgeConductor/Dashboard/IDashboardConnectionApplication.h"
#include "ForgeConductor/Domain/ConfigurationModels.h"
#include "ForgeConductor/Domain/Result.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>

namespace ForgeConductor::Infrastructure::Windows::Detail {

class ManagerDashboardOperationalState;

struct WindowsDashboardRuntimeBinding final {
    Domain::DashboardConfig configuration;
    std::shared_ptr<Dashboard::IDashboardConnectionApplication> application;
};

enum class WindowsDashboardRuntimeLifecycle : std::uint8_t {
    Ready,
    Listening,
    ShuttingDown,
    Drained,
    Failed,
};

// Allocation-owning, bounded observation of the process transport graph. The
// only string is the validated literal loopback host retained by the listener
// factory; no connection, request, response, or queue contents are copied.
class WindowsDashboardRuntimeSnapshot final {
public:
    [[nodiscard]] WindowsDashboardRuntimeLifecycle lifecycle() const noexcept;
    [[nodiscard]] bool operationalServiceActive() const noexcept;
    [[nodiscard]] const std::optional<Domain::DashboardConfig>& configuration()
        const noexcept;
    [[nodiscard]] const void* applicationIdentity() const noexcept;
    [[nodiscard]] std::uint64_t bindingPublicationSequence() const noexcept;
    [[nodiscard]] std::optional<std::uint64_t> activeRegistrationId()
        const noexcept;
    [[nodiscard]] std::optional<std::uint64_t> retiringRegistrationId()
        const noexcept;
    [[nodiscard]] std::uint64_t listenerPublicationCount() const noexcept;
    [[nodiscard]] std::uint64_t listenerRetirementCount() const noexcept;
    [[nodiscard]] std::size_t registeredConnectionCount() const noexcept;
    [[nodiscard]] std::size_t registeredAuxiliaryDeadlineTargetCount()
        const noexcept;
    [[nodiscard]] std::size_t fixedCompletionTargetCount() const noexcept;
    [[nodiscard]] std::size_t startedWorkerCount() const noexcept;
    [[nodiscard]] std::size_t exitedWorkerCount() const noexcept;
    [[nodiscard]] bool shutdownDrainInstalled() const noexcept;
    [[nodiscard]] bool shutdownDrainFatal() const noexcept;

private:
    friend class WindowsDashboardRuntime;

    WindowsDashboardRuntimeSnapshot(
        WindowsDashboardRuntimeLifecycle lifecycle,
        bool operationalServiceActive,
        std::optional<Domain::DashboardConfig> configuration,
        const void* applicationIdentity,
        std::uint64_t bindingPublicationSequence,
        std::optional<std::uint64_t> activeRegistrationId,
        std::optional<std::uint64_t> retiringRegistrationId,
        std::uint64_t listenerPublicationCount,
        std::uint64_t listenerRetirementCount,
        std::size_t registeredConnectionCount,
        std::size_t registeredAuxiliaryDeadlineTargetCount,
        std::size_t fixedCompletionTargetCount,
        std::size_t startedWorkerCount,
        std::size_t exitedWorkerCount,
        bool shutdownDrainInstalled,
        bool shutdownDrainFatal) noexcept;

    WindowsDashboardRuntimeLifecycle lifecycle_{
        WindowsDashboardRuntimeLifecycle::Ready};
    bool operationalServiceActive_{};
    std::optional<Domain::DashboardConfig> configuration_;
    const void* applicationIdentity_{};
    std::uint64_t bindingPublicationSequence_{};
    std::optional<std::uint64_t> activeRegistrationId_;
    std::optional<std::uint64_t> retiringRegistrationId_;
    std::uint64_t listenerPublicationCount_{};
    std::uint64_t listenerRetirementCount_{};
    std::size_t registeredConnectionCount_{};
    std::size_t registeredAuxiliaryDeadlineTargetCount_{};
    std::size_t fixedCompletionTargetCount_{};
    std::size_t startedWorkerCount_{};
    std::size_t exitedWorkerCount_{};
    bool shutdownDrainInstalled_{};
    bool shutdownDrainFatal_{};
};

// Process-owned lower dashboard transport. The PImpl retains the entire
// Winsock/IOCP/deadline/admission/listener/drain graph through the final wait.
// It deliberately owns no application data beyond one immutable listener
// binding and exposes no restart policy; the higher manager runtime decides
// whether to create a successor transport after terminal shutdown.
class WindowsDashboardRuntime final {
public:
    [[nodiscard]] static Domain::Result<
        std::unique_ptr<WindowsDashboardRuntime>>
    create(
        std::shared_ptr<Contracts::IClock> clock,
        std::shared_ptr<Contracts::IUuidGenerator> uuidGenerator,
        std::shared_ptr<ManagerDashboardOperationalState>
            operationalState) noexcept;

    ~WindowsDashboardRuntime() noexcept;

    WindowsDashboardRuntime(const WindowsDashboardRuntime&) = delete;
    WindowsDashboardRuntime& operator=(const WindowsDashboardRuntime&) =
        delete;
    WindowsDashboardRuntime(WindowsDashboardRuntime&&) = delete;
    WindowsDashboardRuntime& operator=(WindowsDashboardRuntime&&) = delete;

    [[nodiscard]] Domain::Result<void> start(
        WindowsDashboardRuntimeBinding binding) noexcept;
    [[nodiscard]] Domain::Result<void> rebind(
        WindowsDashboardRuntimeBinding binding) noexcept;

    void pauseOperationalService() noexcept;
    void resumeOperationalService() noexcept;

    [[nodiscard]] Domain::Result<WindowsDashboardRuntimeSnapshot> snapshot()
        const noexcept;

    // Both calls are idempotent. wait rejects use before shutdown is
    // requested, preventing an accidental unbounded join on the live driver.
    void requestGracefulShutdown() noexcept;
    [[nodiscard]] Domain::Result<void> wait() noexcept;

private:
    class Impl;

    explicit WindowsDashboardRuntime(std::unique_ptr<Impl> implementation)
        noexcept;

    std::unique_ptr<Impl> implementation_;
};

} // namespace ForgeConductor::Infrastructure::Windows::Detail
