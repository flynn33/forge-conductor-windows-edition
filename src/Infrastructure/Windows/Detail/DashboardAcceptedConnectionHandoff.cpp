#include "DashboardAcceptedConnectionHandoff.h"

#include "ForgeConductor/Domain/Error.h"

#include <exception>
#include <string>
#include <string_view>
#include <utility>

namespace ForgeConductor::Infrastructure::Windows::Detail {
namespace {

[[nodiscard]] Domain::Error handoffError(
    const std::string_view code,
    std::string message,
    const bool retryable = false)
{
    return Domain::makeError(code, std::move(message), retryable);
}

[[nodiscard]] Domain::Error invalidHandoffError()
{
    return handoffError(
        Domain::ErrorCodes::InvalidRequest,
        "A dashboard accepted-connection handoff requires one nonzero "
        "listener generation and complete bounded runtime dependencies.");
}

[[nodiscard]] Domain::Error invalidAcceptResultError()
{
    return handoffError(
        Domain::ErrorCodes::IntegrityFailure,
        "A dashboard accepted-connection handoff requires the exact "
        "AcceptedAndPaused connection and resume-token pair.");
}

[[nodiscard]] Domain::Error internalHandoffError()
{
    return handoffError(
        Domain::ErrorCodes::InternalFailure,
        "The dashboard accepted connection could not be handed off safely.");
}

[[nodiscard]] Domain::Error completedOverloadWorkError()
{
    return handoffError(
        Domain::ErrorCodes::Conflict,
        "The dashboard overload response ownership was already completed.");
}

} // namespace

DashboardAdmissionOverloadWork::DashboardAdmissionOverloadWork(
    const std::uint64_t originGenerationId,
    const Domain::MonotonicTimePoint admittedAt,
    DashboardAcceptSlotSet& acceptSlots,
    DashboardAcceptedConnection acceptedConnection,
    DashboardAcceptResumeToken resumeToken,
    Domain::Error admissionFailure) noexcept
    : originGenerationId_{originGenerationId},
      admittedAt_{admittedAt},
      acceptSlots_{std::addressof(acceptSlots)},
      acceptedConnection_{
          std::in_place, std::move(acceptedConnection)},
      resumeToken_{std::in_place, std::move(resumeToken)},
      admissionFailure_{std::move(admissionFailure)}
{
}

DashboardAdmissionOverloadWork::DashboardAdmissionOverloadWork(
    DashboardAdmissionOverloadWork&& other) noexcept
    : originGenerationId_{other.originGenerationId_},
      admittedAt_{other.admittedAt_},
      acceptSlots_{other.acceptSlots_},
      acceptedConnection_{std::move(other.acceptedConnection_)},
      resumeToken_{std::move(other.resumeToken_)},
      admissionFailure_{std::move(other.admissionFailure_)}
{
    other.acceptedConnection_.reset();
    other.resumeToken_.reset();
    other.acceptSlots_ = nullptr;
}

DashboardAdmissionOverloadWork::~DashboardAdmissionOverloadWork() noexcept
{
    completeWithoutResult();
}

SOCKET DashboardAdmissionOverloadWork::borrowedNativeSocket() const noexcept
{
    return acceptedConnection_.has_value()
        ? acceptedConnection_->borrowedNativeSocket()
        : INVALID_SOCKET;
}

void DashboardAdmissionOverloadWork::closeNativeSocket() noexcept
{
    if (acceptedConnection_.has_value()) {
        acceptedConnection_->closeNativeSocket();
    }
}

std::optional<DashboardAcceptLifecycleFailure>
DashboardAdmissionOverloadWork::closeOriginAdmission() noexcept
{
    if (!ownsCompletionObligation()) {
        return std::nullopt;
    }
    return acceptSlots_->closeAdmissionAndRequestCancellation();
}

Domain::Result<DashboardAcceptResumeDisposition>
DashboardAdmissionOverloadWork::complete() noexcept
{
    if (!ownsCompletionObligation()) {
        return Domain::Result<DashboardAcceptResumeDisposition>::failure(
            completedOverloadWorkError());
    }

    acceptedConnection_.reset();
    auto resumed = acceptSlots_->resume(std::move(*resumeToken_));
    if (resumeToken_->valid()) {
        std::terminate();
    }
    resumeToken_.reset();
    acceptSlots_ = nullptr;
    return resumed;
}

void DashboardAdmissionOverloadWork::completeWithoutResult() noexcept
{
    if (!ownsCompletionObligation()) {
        acceptedConnection_.reset();
        if (resumeToken_.has_value() && resumeToken_->valid()) {
            std::terminate();
        }
        resumeToken_.reset();
        acceptSlots_ = nullptr;
        return;
    }

    acceptedConnection_.reset();
    const auto resumed = acceptSlots_->resume(std::move(*resumeToken_));
    static_cast<void>(resumed);
    if (resumeToken_->valid()) {
        std::terminate();
    }
    resumeToken_.reset();
    acceptSlots_ = nullptr;
}

Domain::Result<std::shared_ptr<DashboardAcceptedConnectionOwnerFactory>>
DashboardAcceptedConnectionOwnerFactory::create(
    DashboardIocpWorkerKernel& kernel,
    WindowsDashboardDeadlineScheduler& deadlineScheduler,
    WindowsDashboardHandlerExecutor& handlerExecutor,
    DashboardConnectionRuntimeServices& runtimeServices,
    std::shared_ptr<Dashboard::IDashboardConnectionApplication> application,
    DashboardConnectionResponseCatalog& responseCatalog) noexcept
{
    using CreateResult = Domain::Result<std::shared_ptr<
        DashboardAcceptedConnectionOwnerFactory>>;
    if (application == nullptr) {
        return CreateResult::failure(invalidHandoffError());
    }

    try {
        return CreateResult::success(
            std::shared_ptr<DashboardAcceptedConnectionOwnerFactory>{
                new DashboardAcceptedConnectionOwnerFactory{
                    kernel,
                    deadlineScheduler,
                    handlerExecutor,
                    runtimeServices,
                    std::move(application),
                    responseCatalog}});
    } catch (...) {
        return CreateResult::failure(internalHandoffError());
    }
}

DashboardAcceptedConnectionOwnerFactory::
DashboardAcceptedConnectionOwnerFactory(
    DashboardIocpWorkerKernel& kernel,
    WindowsDashboardDeadlineScheduler& deadlineScheduler,
    WindowsDashboardHandlerExecutor& handlerExecutor,
    DashboardConnectionRuntimeServices& runtimeServices,
    std::shared_ptr<Dashboard::IDashboardConnectionApplication> application,
    DashboardConnectionResponseCatalog& responseCatalog) noexcept
    : kernel_{std::addressof(kernel)},
      deadlineScheduler_{std::addressof(deadlineScheduler)},
      handlerExecutor_{std::addressof(handlerExecutor)},
      runtimeServices_{std::addressof(runtimeServices)},
      application_{std::move(application)},
      responseCatalog_{std::addressof(responseCatalog)}
{
}

Domain::Result<std::shared_ptr<IDashboardConnectionDispatchTarget>>
DashboardAcceptedConnectionOwnerFactory::createOwner(
    const std::uint64_t generationId,
    const DashboardConnectionRuntimeIdentity identity,
    const Domain::MonotonicTimePoint admittedAt,
    DashboardAdmissionController::Lease admissionLease,
    DashboardAcceptedConnection acceptedConnection) noexcept
{
    using OwnerResult = Domain::Result<
        std::shared_ptr<IDashboardConnectionDispatchTarget>>;
    auto socket = DashboardConnectionSocket::create(
        std::move(acceptedConnection), *kernel_, identity.completionKey);
    if (!socket) {
        return OwnerResult::failure(std::move(socket).error());
    }

    auto state = DashboardConnectionState::create(
        generationId,
        identity,
        admittedAt,
        std::move(admissionLease),
        std::move(socket).value(),
        *kernel_,
        *deadlineScheduler_,
        *handlerExecutor_,
        *runtimeServices_,
        application_,
        *responseCatalog_);
    if (!state) {
        return OwnerResult::failure(std::move(state).error());
    }
    return OwnerResult::success(std::move(state).value());
}

DashboardConnectionRegistryRegistrar::DashboardConnectionRegistryRegistrar(
    DashboardConnectionRegistry& registry) noexcept
    : registry_{std::addressof(registry)}
{
}

Domain::Result<void>
DashboardConnectionRegistryRegistrar::registerConnection(
    std::shared_ptr<IDashboardConnectionDispatchTarget> target) noexcept
{
    return registry_->registerConnection(std::move(target));
}

Domain::Result<std::unique_ptr<DashboardAcceptedConnectionHandoff>>
DashboardAcceptedConnectionHandoff::create(
    const std::uint64_t generationId,
    DashboardAcceptSlotSet& acceptSlots,
    DashboardAdmissionController& admissionController,
    DashboardConnectionRuntimeServices& runtimeServices,
    std::shared_ptr<IDashboardAcceptedConnectionOwnerFactory> ownerFactory,
    std::shared_ptr<IDashboardConnectionRegistrar> registrar,
    std::shared_ptr<IDashboardAdmissionOverloadResponder> overloadResponder)
    noexcept
{
    using CreateResult = Domain::Result<std::unique_ptr<
        DashboardAcceptedConnectionHandoff>>;
    const auto* const applicationPolicyIdentity = ownerFactory == nullptr
        ? nullptr
        : ownerFactory->applicationPolicyIdentity();
    if (generationId == 0U || ownerFactory == nullptr ||
        applicationPolicyIdentity == nullptr || registrar == nullptr ||
        overloadResponder == nullptr) {
        return CreateResult::failure(invalidHandoffError());
    }

    try {
        return CreateResult::success(
            std::unique_ptr<DashboardAcceptedConnectionHandoff>{
                new DashboardAcceptedConnectionHandoff{
                    generationId,
                    acceptSlots,
                    admissionController,
                    runtimeServices,
                    std::move(ownerFactory),
                    applicationPolicyIdentity,
                    std::move(registrar),
                    std::move(overloadResponder)}});
    } catch (...) {
        return CreateResult::failure(internalHandoffError());
    }
}

DashboardAcceptedConnectionHandoff::DashboardAcceptedConnectionHandoff(
    const std::uint64_t generationId,
    DashboardAcceptSlotSet& acceptSlots,
    DashboardAdmissionController& admissionController,
    DashboardConnectionRuntimeServices& runtimeServices,
    std::shared_ptr<IDashboardAcceptedConnectionOwnerFactory> ownerFactory,
    const void* const applicationPolicyIdentity,
    std::shared_ptr<IDashboardConnectionRegistrar> registrar,
    std::shared_ptr<IDashboardAdmissionOverloadResponder> overloadResponder)
    noexcept
    : generationId_{generationId},
      acceptSlots_{std::addressof(acceptSlots)},
      admissionController_{std::addressof(admissionController)},
      runtimeServices_{std::addressof(runtimeServices)},
      ownerFactory_{std::move(ownerFactory)},
      applicationPolicyIdentity_{applicationPolicyIdentity},
      registrar_{std::move(registrar)},
      overloadResponder_{std::move(overloadResponder)}
{
}

Domain::Result<DashboardAcceptedConnectionHandoffDisposition>
DashboardAcceptedConnectionHandoff::consume(
    DashboardAcceptReapResult accepted) noexcept
{
    auto acceptedConnection = accepted.takeAcceptedConnection();
    auto resumeToken = accepted.takeResumeToken();
    if (accepted.disposition() !=
            DashboardAcceptReapDisposition::AcceptedAndPaused ||
        !acceptedConnection.has_value() || !resumeToken.has_value() ||
        !resumeToken->valid()) {
        return failAfterLocalOwnership(
            invalidAcceptResultError(),
            acceptedConnection,
            resumeToken,
            nullptr);
    }

    const auto admittedAt = runtimeServices_->monotonicNow();
    auto admitted = admissionController_->tryAccept();
    if (!admitted) {
        overloadResponder_->respond(DashboardAdmissionOverloadWork{
            generationId_,
            admittedAt,
            *acceptSlots_,
            std::move(*acceptedConnection),
            std::move(*resumeToken),
            std::move(admitted).error()});
        acceptedConnection.reset();
        resumeToken.reset();
        return Domain::Result<
            DashboardAcceptedConnectionHandoffDisposition>::success(
            DashboardAcceptedConnectionHandoffDisposition::
                OverloadResponseTransferred);
    }

    auto admissionLease = std::move(admitted).value();
    auto identity = runtimeServices_->allocateConnectionIdentity();
    if (!identity) {
        return failAfterLocalOwnership(
            std::move(identity).error(),
            acceptedConnection,
            resumeToken,
            std::addressof(admissionLease));
    }

    auto owner = ownerFactory_->createOwner(
        generationId_,
        std::move(identity).value(),
        admittedAt,
        std::move(admissionLease),
        std::move(*acceptedConnection));
    acceptedConnection.reset();
    if (!owner) {
        return failAfterLocalOwnership(
            std::move(owner).error(),
            acceptedConnection,
            resumeToken,
            nullptr);
    }
    if (owner.value() == nullptr) {
        return failAfterLocalOwnership(
            internalHandoffError(),
            acceptedConnection,
            resumeToken,
            nullptr);
    }

    auto target = std::move(owner).value();
    auto registered = registrar_->registerConnection(target);
    target.reset();

    auto resumed = acceptSlots_->resume(std::move(*resumeToken));
    if (resumeToken->valid()) {
        std::terminate();
    }
    resumeToken.reset();

    if (!registered) {
        return Domain::Result<
            DashboardAcceptedConnectionHandoffDisposition>::failure(
            std::move(registered).error());
    }
    if (!resumed) {
        return Domain::Result<
            DashboardAcceptedConnectionHandoffDisposition>::failure(
            std::move(resumed).error());
    }

    return Domain::Result<
        DashboardAcceptedConnectionHandoffDisposition>::success(
        DashboardAcceptedConnectionHandoffDisposition::Registered);
}

Domain::Result<DashboardAcceptedConnectionHandoffDisposition>
DashboardAcceptedConnectionHandoff::failAfterLocalOwnership(
    Domain::Error error,
    std::optional<DashboardAcceptedConnection>& acceptedConnection,
    std::optional<DashboardAcceptResumeToken>& resumeToken,
    DashboardAdmissionController::Lease* const admissionLease) noexcept
{
    acceptedConnection.reset();
    if (admissionLease != nullptr) {
        admissionLease->release();
    }
    if (!resumeToken.has_value() || !resumeToken->valid()) {
        return Domain::Result<
            DashboardAcceptedConnectionHandoffDisposition>::failure(
            std::move(error));
    }

    auto resumed = acceptSlots_->resume(std::move(*resumeToken));
    if (resumeToken->valid()) {
        std::terminate();
    }
    resumeToken.reset();
    if (!resumed) {
        return Domain::Result<
            DashboardAcceptedConnectionHandoffDisposition>::failure(
            std::move(resumed).error());
    }
    return Domain::Result<
        DashboardAcceptedConnectionHandoffDisposition>::failure(
        std::move(error));
}

} // namespace ForgeConductor::Infrastructure::Windows::Detail
