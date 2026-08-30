#pragma once

#include "DashboardAcceptedConnectionHandoff.h"
#include "DashboardConnectionRegistry.h"
#include "DashboardListenerGenerationCoordinator.h"

#include "ForgeConductor/Dashboard/IDashboardConnectionApplication.h"
#include "ForgeConductor/Domain/ConfigurationModels.h"
#include "ForgeConductor/Domain/Result.h"

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>

namespace ForgeConductor::Infrastructure::Windows {

class WindowsDashboardDeadlineScheduler;
class WindowsDashboardHandlerExecutor;

namespace Detail {

class DashboardAdmissionController;
class DashboardConnectionResponseCatalog;
class DashboardConnectionRuntimeServices;
class DashboardIocpWorkerKernel;
class DashboardListenerCompletionKeyLeasePool;
class DashboardWinsockRuntime;

// Allocation-owning observation of the currently published listener binding.
// It intentionally exposes only the immutable application identity, never a
// mutable or owning application pointer.
class WindowsDashboardListenerBindingSnapshot final {
public:
    [[nodiscard]] const Domain::DashboardConfig& configuration()
        const noexcept
    {
        return configuration_;
    }

    [[nodiscard]] const void* applicationIdentity() const noexcept
    {
        return applicationIdentity_;
    }

    [[nodiscard]] std::uint64_t publicationSequence() const noexcept
    {
        return publicationSequence_;
    }

private:
    friend class WindowsDashboardListenerGenerationFactory;

    WindowsDashboardListenerBindingSnapshot(
        Domain::DashboardConfig configuration,
        const void* applicationIdentity,
        std::uint64_t publicationSequence) noexcept;

    Domain::DashboardConfig configuration_;
    const void* applicationIdentity_{};
    std::uint64_t publicationSequence_{};
};

// Process-owned production factory for immutable listener generations. A
// binding publication is copied and validated before it becomes visible.
// prepareGeneration retains one exact publication snapshot while composing a
// generation, so concurrent replacement or clearing can affect only a later
// preparation. Preparation binds and listens but deliberately does not start
// AcceptEx admission; the coordinator starts admission only after registering
// the prepared generation with both routing owners.
class WindowsDashboardListenerGenerationFactory final
    : public IDashboardListenerGenerationFactory {
public:
    [[nodiscard]] static Domain::Result<std::shared_ptr<
        WindowsDashboardListenerGenerationFactory>>
    create(
        DashboardWinsockRuntime& winsockRuntime,
        DashboardIocpWorkerKernel& kernel,
        WindowsDashboardDeadlineScheduler& deadlineScheduler,
        WindowsDashboardHandlerExecutor& handlerExecutor,
        DashboardConnectionRuntimeServices& runtimeServices,
        DashboardAdmissionController& admissionController,
        DashboardListenerCompletionKeyLeasePool& completionKeyLeases,
        std::shared_ptr<DashboardConnectionRegistry> connectionRegistry,
        std::shared_ptr<IDashboardAdmissionOverloadResponder>
            overloadResponder,
        DashboardConnectionResponseCatalog& responseCatalog) noexcept;

    ~WindowsDashboardListenerGenerationFactory() noexcept override = default;

    WindowsDashboardListenerGenerationFactory(
        const WindowsDashboardListenerGenerationFactory&) = delete;
    WindowsDashboardListenerGenerationFactory& operator=(
        const WindowsDashboardListenerGenerationFactory&) = delete;
    WindowsDashboardListenerGenerationFactory(
        WindowsDashboardListenerGenerationFactory&&) = delete;
    WindowsDashboardListenerGenerationFactory& operator=(
        WindowsDashboardListenerGenerationFactory&&) = delete;

    // Replaces the binding for future preparations only. The supplied
    // configuration is copied; a successful caller cannot mutate the
    // retained publication. Invalid replacement leaves the prior publication
    // unchanged.
    [[nodiscard]] Domain::Result<void> publishBinding(
        Domain::DashboardConfig configuration,
        std::shared_ptr<Dashboard::IDashboardConnectionApplication>
            application) noexcept;

    // Closes future preparation until another valid publication. Already
    // snapshotted or prepared generations retain their exact binding.
    void clearBinding() noexcept;

    [[nodiscard]] Domain::Result<std::optional<
        WindowsDashboardListenerBindingSnapshot>>
    bindingSnapshot() const noexcept;

    [[nodiscard]] Domain::Result<std::shared_ptr<
        IDashboardListenerGeneration>>
    prepareGeneration(
        std::shared_ptr<DashboardListenerGenerationTransitionGate>
            transitionGate) noexcept override;

private:
    struct Binding;

    WindowsDashboardListenerGenerationFactory(
        DashboardWinsockRuntime& winsockRuntime,
        DashboardIocpWorkerKernel& kernel,
        WindowsDashboardDeadlineScheduler& deadlineScheduler,
        WindowsDashboardHandlerExecutor& handlerExecutor,
        DashboardConnectionRuntimeServices& runtimeServices,
        DashboardAdmissionController& admissionController,
        DashboardListenerCompletionKeyLeasePool& completionKeyLeases,
        std::shared_ptr<DashboardConnectionRegistry> connectionRegistry,
        std::shared_ptr<IDashboardAdmissionOverloadResponder>
            overloadResponder,
        DashboardConnectionResponseCatalog& responseCatalog,
        std::shared_ptr<IDashboardListenerGenerationConnectionControl>
            connectionControl,
        std::shared_ptr<IDashboardConnectionRegistrar> registrar) noexcept;

    DashboardWinsockRuntime* winsockRuntime_{};
    DashboardIocpWorkerKernel* kernel_{};
    WindowsDashboardDeadlineScheduler* deadlineScheduler_{};
    WindowsDashboardHandlerExecutor* handlerExecutor_{};
    DashboardConnectionRuntimeServices* runtimeServices_{};
    DashboardAdmissionController* admissionController_{};
    DashboardListenerCompletionKeyLeasePool* completionKeyLeases_{};
    const std::shared_ptr<DashboardConnectionRegistry> connectionRegistry_;
    const std::shared_ptr<IDashboardAdmissionOverloadResponder>
        overloadResponder_;
    DashboardConnectionResponseCatalog* responseCatalog_{};
    const std::shared_ptr<IDashboardListenerGenerationConnectionControl>
        connectionControl_;
    const std::shared_ptr<IDashboardConnectionRegistrar> registrar_;

    std::atomic<std::shared_ptr<const Binding>> binding_;
    std::mutex publicationMutex_;
    std::uint64_t nextPublicationSequence_{1U};
    bool publicationSequenceExhausted_{};
};

} // namespace Detail
} // namespace ForgeConductor::Infrastructure::Windows
