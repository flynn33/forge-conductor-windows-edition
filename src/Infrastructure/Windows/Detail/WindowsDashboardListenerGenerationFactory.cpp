#include "WindowsDashboardListenerGenerationFactory.h"

#include "DashboardAcceptSlotSet.h"
#include "DashboardAdmissionController.h"
#include "DashboardConnectionResponseCatalog.h"
#include "DashboardConnectionRuntimeServices.h"
#include "DashboardIocpWorkerKernel.h"
#include "DashboardListenerCompletionKeyLease.h"
#include "DashboardListeningSocket.h"
#include "DashboardLoopbackEndpoint.h"
#include "DashboardWinsockRuntime.h"

#include "ForgeConductor/Infrastructure/Windows/WindowsDashboardDeadlineScheduler.h"
#include "ForgeConductor/Infrastructure/Windows/WindowsDashboardHandlerExecutor.h"

#include <exception>
#include <limits>
#include <string>
#include <string_view>
#include <utility>

namespace ForgeConductor::Infrastructure::Windows::Detail {
namespace {

[[nodiscard]] Domain::Error factoryError(
    const std::string_view code,
    std::string message,
    const bool retryable = false)
{
    return Domain::makeError(code, std::move(message), retryable);
}

[[nodiscard]] Domain::Error invalidFactoryDependenciesError()
{
    return factoryError(
        Domain::ErrorCodes::InvalidRequest,
        "The dashboard listener-generation factory dependencies are invalid.");
}

[[nodiscard]] Domain::Error invalidBindingApplicationError()
{
    return factoryError(
        Domain::ErrorCodes::InvalidRequest,
        "The dashboard listener binding requires an application policy.");
}

[[nodiscard]] Domain::Error invalidBindingRefreshIntervalError()
{
    return factoryError(
        Domain::ErrorCodes::InvalidRequest,
        "The dashboard listener refresh interval must be positive.");
}

[[nodiscard]] Domain::Error missingBindingError()
{
    return factoryError(
        Domain::ErrorCodes::Conflict,
        "No dashboard listener binding is published.",
        true);
}

[[nodiscard]] Domain::Error invalidTransitionGateError()
{
    return factoryError(
        Domain::ErrorCodes::InvalidRequest,
        "The dashboard listener generation requires its coordinator transition gate.");
}

[[nodiscard]] Domain::Error publicationSequenceExhaustedError()
{
    return factoryError(
        Domain::ErrorCodes::LimitExceeded,
        "The dashboard listener binding publication sequence is exhausted.");
}

[[nodiscard]] Domain::Error internalFactoryError()
{
    return factoryError(
        Domain::ErrorCodes::InternalFailure,
        "The dashboard listener-generation factory failed safely.");
}

} // namespace

struct WindowsDashboardListenerGenerationFactory::Binding final {
    Binding(
        Domain::DashboardConfig publishedConfiguration,
        DashboardLoopbackEndpoint publishedEndpoint,
        std::shared_ptr<Dashboard::IDashboardConnectionApplication>
            publishedApplication,
        const std::uint64_t publishedSequence) noexcept
        : configuration{std::move(publishedConfiguration)},
          endpoint{std::move(publishedEndpoint)},
          application{std::move(publishedApplication)},
          publicationSequence{publishedSequence}
    {
    }

    const Domain::DashboardConfig configuration;
    const DashboardLoopbackEndpoint endpoint;
    const std::shared_ptr<Dashboard::IDashboardConnectionApplication>
        application;
    const std::uint64_t publicationSequence{};
};

WindowsDashboardListenerBindingSnapshot::
    WindowsDashboardListenerBindingSnapshot(
        Domain::DashboardConfig configuration,
        const void* const applicationIdentity,
        const std::uint64_t publicationSequence) noexcept
    : configuration_{std::move(configuration)},
      applicationIdentity_{applicationIdentity},
      publicationSequence_{publicationSequence}
{
}

Domain::Result<std::shared_ptr<
    WindowsDashboardListenerGenerationFactory>>
WindowsDashboardListenerGenerationFactory::create(
    DashboardWinsockRuntime& winsockRuntime,
    DashboardIocpWorkerKernel& kernel,
    WindowsDashboardDeadlineScheduler& deadlineScheduler,
    WindowsDashboardHandlerExecutor& handlerExecutor,
    DashboardConnectionRuntimeServices& runtimeServices,
    DashboardAdmissionController& admissionController,
    DashboardListenerCompletionKeyLeasePool& completionKeyLeases,
    std::shared_ptr<DashboardConnectionRegistry> connectionRegistry,
    std::shared_ptr<IDashboardAdmissionOverloadResponder> overloadResponder,
    DashboardConnectionResponseCatalog& responseCatalog) noexcept
{
    using CreateResult = Domain::Result<std::shared_ptr<
        WindowsDashboardListenerGenerationFactory>>;
    if (connectionRegistry == nullptr || overloadResponder == nullptr) {
        return CreateResult::failure(invalidFactoryDependenciesError());
    }

    try {
        auto connectionControl = std::make_shared<
            DashboardConnectionRegistryGenerationControl>(
                *connectionRegistry);
        auto registrar =
            std::make_shared<DashboardConnectionRegistryRegistrar>(
                *connectionRegistry);
        return CreateResult::success(
            std::shared_ptr<WindowsDashboardListenerGenerationFactory>{
                new WindowsDashboardListenerGenerationFactory{
                    winsockRuntime,
                    kernel,
                    deadlineScheduler,
                    handlerExecutor,
                    runtimeServices,
                    admissionController,
                    completionKeyLeases,
                    std::move(connectionRegistry),
                    std::move(overloadResponder),
                    responseCatalog,
                    std::move(connectionControl),
                    std::move(registrar)}});
    } catch (...) {
        return CreateResult::failure(internalFactoryError());
    }
}

WindowsDashboardListenerGenerationFactory::
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
        std::shared_ptr<IDashboardConnectionRegistrar> registrar) noexcept
    : winsockRuntime_{std::addressof(winsockRuntime)},
      kernel_{std::addressof(kernel)},
      deadlineScheduler_{std::addressof(deadlineScheduler)},
      handlerExecutor_{std::addressof(handlerExecutor)},
      runtimeServices_{std::addressof(runtimeServices)},
      admissionController_{std::addressof(admissionController)},
      completionKeyLeases_{std::addressof(completionKeyLeases)},
      connectionRegistry_{std::move(connectionRegistry)},
      overloadResponder_{std::move(overloadResponder)},
      responseCatalog_{std::addressof(responseCatalog)},
      connectionControl_{std::move(connectionControl)},
      registrar_{std::move(registrar)}
{
}

Domain::Result<void>
WindowsDashboardListenerGenerationFactory::publishBinding(
    Domain::DashboardConfig configuration,
    std::shared_ptr<Dashboard::IDashboardConnectionApplication> application)
    noexcept
{
    if (application == nullptr) {
        return Domain::Result<void>::failure(
            invalidBindingApplicationError());
    }
    if (configuration.refreshInterval.count() <= 0) {
        return Domain::Result<void>::failure(
            invalidBindingRefreshIntervalError());
    }

    auto endpoint = DashboardLoopbackEndpoint::create(
        configuration.host, configuration.port);
    if (!endpoint) {
        return Domain::Result<void>::failure(std::move(endpoint).error());
    }

    try {
        const std::lock_guard lock{publicationMutex_};
        if (publicationSequenceExhausted_) {
            return Domain::Result<void>::failure(
                publicationSequenceExhaustedError());
        }

        const auto sequence = nextPublicationSequence_;
        auto published = std::make_shared<const Binding>(
            std::move(configuration),
            std::move(endpoint).value(),
            std::move(application),
            sequence);
        binding_.store(std::move(published), std::memory_order_release);
        if (sequence == (std::numeric_limits<std::uint64_t>::max)()) {
            publicationSequenceExhausted_ = true;
        } else {
            nextPublicationSequence_ = sequence + 1U;
        }
        return Domain::Result<void>::success();
    } catch (...) {
        return Domain::Result<void>::failure(internalFactoryError());
    }
}

void WindowsDashboardListenerGenerationFactory::clearBinding() noexcept
{
    binding_.store(nullptr, std::memory_order_release);
}

Domain::Result<std::optional<
    WindowsDashboardListenerBindingSnapshot>>
WindowsDashboardListenerGenerationFactory::bindingSnapshot() const noexcept
{
    try {
        const auto binding = binding_.load(std::memory_order_acquire);
        if (binding == nullptr) {
            return Domain::Result<std::optional<
                WindowsDashboardListenerBindingSnapshot>>::success(
                    std::nullopt);
        }
        return Domain::Result<std::optional<
            WindowsDashboardListenerBindingSnapshot>>::success(
                WindowsDashboardListenerBindingSnapshot{
                    binding->configuration,
                    binding->application.get(),
                    binding->publicationSequence});
    } catch (...) {
        return Domain::Result<std::optional<
            WindowsDashboardListenerBindingSnapshot>>::failure(
                internalFactoryError());
    }
}

Domain::Result<std::shared_ptr<IDashboardListenerGeneration>>
WindowsDashboardListenerGenerationFactory::prepareGeneration(
    std::shared_ptr<DashboardListenerGenerationTransitionGate>
        transitionGate) noexcept
{
    using PrepareResult =
        Domain::Result<std::shared_ptr<IDashboardListenerGeneration>>;
    if (transitionGate == nullptr) {
        return PrepareResult::failure(invalidTransitionGateError());
    }

    try {
        const auto binding = binding_.load(std::memory_order_acquire);
        if (binding == nullptr) {
            return PrepareResult::failure(missingBindingError());
        }

        auto acquiredLease = completionKeyLeases_->tryAcquire();
        if (!acquiredLease) {
            return PrepareResult::failure(
                std::move(acquiredLease).error());
        }
        auto completionKeyLease = std::move(acquiredLease).value();

        auto allocatedIdentity = runtimeServices_->allocateFixedIdentity(
            completionKeyLease.completionKey());
        if (!allocatedIdentity) {
            return PrepareResult::failure(
                std::move(allocatedIdentity).error());
        }
        const auto identity = std::move(allocatedIdentity).value();

        auto listeningSocket = DashboardListeningSocket::create(
            *winsockRuntime_, binding->endpoint);
        if (!listeningSocket) {
            return PrepareResult::failure(
                std::move(listeningSocket).error());
        }

        auto acceptSlots = DashboardAcceptSlotSet::create(
            *winsockRuntime_,
            std::move(listeningSocket).value(),
            identity.completionKey);
        if (!acceptSlots) {
            return PrepareResult::failure(
                std::move(acceptSlots).error());
        }
        auto slotOwner = std::move(acceptSlots).value();

        auto ownerFactory = DashboardAcceptedConnectionOwnerFactory::create(
            *kernel_,
            *deadlineScheduler_,
            *handlerExecutor_,
            *runtimeServices_,
            binding->application,
            *responseCatalog_);
        if (!ownerFactory) {
            return PrepareResult::failure(
                std::move(ownerFactory).error());
        }

        auto handoff = DashboardAcceptedConnectionHandoff::create(
            identity.registrationId,
            *slotOwner,
            *admissionController_,
            *runtimeServices_,
            std::move(ownerFactory).value(),
            registrar_,
            overloadResponder_);
        if (!handoff) {
            return PrepareResult::failure(std::move(handoff).error());
        }

        auto acceptOwner = DashboardListenerGenerationAcceptOwner::create(
            identity,
            std::move(slotOwner),
            std::move(handoff).value(),
            binding->application,
            *kernel_);
        if (!acceptOwner) {
            return PrepareResult::failure(
                std::move(acceptOwner).error());
        }

        auto generation = DashboardListenerGeneration::create(
            identity,
            std::move(completionKeyLease),
            std::move(acceptOwner).value(),
            *deadlineScheduler_,
            *runtimeServices_,
            connectionControl_,
            overloadResponder_,
            std::move(transitionGate));
        if (!generation) {
            return PrepareResult::failure(
                std::move(generation).error());
        }

        // Coordinator registration and publication intentionally happen after
        // this return. No AcceptEx work is started by this factory.
        std::shared_ptr<IDashboardListenerGeneration> prepared =
            std::move(generation).value();
        return PrepareResult::success(std::move(prepared));
    } catch (...) {
        return PrepareResult::failure(internalFactoryError());
    }
}

} // namespace ForgeConductor::Infrastructure::Windows::Detail
