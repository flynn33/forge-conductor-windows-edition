#include "DashboardConnectionRuntimeServices.h"

#include "DashboardIocpWorkerKernel.h"

#include "ForgeConductor/Domain/Error.h"

#include <algorithm>
#include <limits>
#include <string>
#include <string_view>
#include <utility>

namespace ForgeConductor::Infrastructure::Windows::Detail {
namespace {

[[nodiscard]] Domain::Error runtimeServicesError(
    const std::string_view code,
    std::string message,
    const bool retryable = false)
{
    return Domain::makeError(code, std::move(message), retryable);
}

[[nodiscard]] Domain::Error invalidDependenciesError()
{
    return runtimeServicesError(
        Domain::ErrorCodes::InvalidRequest,
        "Dashboard connection runtime services require a clock, UUID "
        "generator, operational-state source, and a bounded unique fixed-key "
        "set.");
}

[[nodiscard]] Domain::Error identityExhaustionError()
{
    return runtimeServicesError(
        Domain::ErrorCodes::IntegrityFailure,
        "The dashboard runtime identity sequence is exhausted.");
}

[[nodiscard]] Domain::Error invalidFixedIdentityKeyError()
{
    return runtimeServicesError(
        Domain::ErrorCodes::InvalidRequest,
        "A dashboard fixed identity requires a nonzero fixed completion key "
        "owned by this runtime, excluding the worker shutdown key.");
}

[[nodiscard]] Domain::Error runtimeAdmissionClosedError()
{
    return runtimeServicesError(
        Domain::ErrorCodes::TransportClosed,
        "The dashboard runtime is closed to new work.");
}

} // namespace

Domain::Result<std::unique_ptr<DashboardConnectionRuntimeServices>>
DashboardConnectionRuntimeServices::create(
    std::shared_ptr<Contracts::IClock> clock,
    std::shared_ptr<Contracts::IUuidGenerator> uuidGenerator,
    std::shared_ptr<IDashboardOperationalStateSource> operationalState,
    const DashboardFixedIocpKeyAuthority& fixedKeyAuthority) noexcept
{
    return create(
        std::move(clock),
        std::move(uuidGenerator),
        std::move(operationalState),
        fixedKeyAuthority.completionKeys());
}

Domain::Result<std::unique_ptr<DashboardConnectionRuntimeServices>>
DashboardConnectionRuntimeServices::create(
    std::shared_ptr<Contracts::IClock> clock,
    std::shared_ptr<Contracts::IUuidGenerator> uuidGenerator,
    std::shared_ptr<IDashboardOperationalStateSource> operationalState,
    const std::span<const DashboardIoCompletionKey> fixedCompletionKeys)
    noexcept
{
    using CreationResult = Domain::Result<
        std::unique_ptr<DashboardConnectionRuntimeServices>>;
    try {
        if (clock == nullptr || uuidGenerator == nullptr ||
            operationalState == nullptr ||
            fixedCompletionKeys.size() > MaximumFixedCompletionKeyCount) {
            return CreationResult::failure(invalidDependenciesError());
        }

        std::vector<std::uintptr_t> fixedValues;
        fixedValues.reserve(fixedCompletionKeys.size());
        for (const auto key : fixedCompletionKeys) {
            fixedValues.push_back(key.value());
        }
        std::sort(fixedValues.begin(), fixedValues.end());
        if (std::adjacent_find(fixedValues.begin(), fixedValues.end()) !=
            fixedValues.end()) {
            return CreationResult::failure(invalidDependenciesError());
        }

        return CreationResult::success(
            std::unique_ptr<DashboardConnectionRuntimeServices>{
                new DashboardConnectionRuntimeServices{
                    std::move(clock),
                    std::move(uuidGenerator),
                    std::move(operationalState),
                    std::move(fixedValues)}});
    } catch (...) {
        return CreationResult::failure(runtimeServicesError(
            Domain::ErrorCodes::InternalFailure,
            "Dashboard connection runtime services could not be created."));
    }
}

DashboardConnectionRuntimeServices::DashboardConnectionRuntimeServices(
    std::shared_ptr<Contracts::IClock> clock,
    std::shared_ptr<Contracts::IUuidGenerator> uuidGenerator,
    std::shared_ptr<IDashboardOperationalStateSource> operationalState,
    std::vector<std::uintptr_t> fixedCompletionKeys) noexcept
    : clock_{std::move(clock)},
      uuidGenerator_{std::move(uuidGenerator)},
      operationalState_{std::move(operationalState)},
      fixedCompletionKeys_{std::move(fixedCompletionKeys)}
{
}

Domain::Result<DashboardConnectionRuntimeIdentity>
DashboardConnectionRuntimeServices::allocateConnectionIdentity() noexcept
{
    try {
        const std::lock_guard lock{identityMutex_};
        if (shutdown_) {
            return Domain::Result<DashboardConnectionRuntimeIdentity>::failure(
                runtimeAdmissionClosedError());
        }
        auto stagedRegistrationIds = registrationIds_;
        auto stagedCompletionKeys = completionKeys_;
        const auto registrationId = stagedRegistrationIds.tryTake(
            [](const std::uint64_t value) noexcept {
                return value == 0U;
            });
        const auto completionValue = stagedCompletionKeys.tryTake(
            [this](const std::uintptr_t value) noexcept {
                return isReservedCompletionKey(value);
            });
        if (!registrationId.has_value() || !completionValue.has_value()) {
            return Domain::Result<DashboardConnectionRuntimeIdentity>::failure(
                identityExhaustionError());
        }

        const DashboardConnectionRuntimeIdentity identity{
            *registrationId,
            DashboardIoCompletionKey{*completionValue}};
        registrationIds_ = stagedRegistrationIds;
        completionKeys_ = stagedCompletionKeys;

        return Domain::Result<DashboardConnectionRuntimeIdentity>::success(
            identity);
    } catch (...) {
        return Domain::Result<DashboardConnectionRuntimeIdentity>::failure(
            runtimeServicesError(
                Domain::ErrorCodes::InternalFailure,
                "The dashboard connection identity could not be allocated."));
    }
}

Domain::Result<DashboardConnectionRuntimeIdentity>
DashboardConnectionRuntimeServices::allocateFixedIdentity(
    const DashboardIoCompletionKey completionKey) noexcept
{
    try {
        if (!isFixedCompletionKey(completionKey.value()) ||
            completionKey.value() == 0U ||
            completionKey.value() ==
                DashboardIocpWorkerKernel::ShutdownKeyValue) {
            return Domain::Result<DashboardConnectionRuntimeIdentity>::failure(
                invalidFixedIdentityKeyError());
        }

        const std::lock_guard lock{identityMutex_};
        if (shutdown_) {
            return Domain::Result<DashboardConnectionRuntimeIdentity>::failure(
                runtimeAdmissionClosedError());
        }
        auto stagedRegistrationIds = registrationIds_;
        const auto registrationId = stagedRegistrationIds.tryTake(
            [](const std::uint64_t value) noexcept {
                return value == 0U;
            });
        if (!registrationId.has_value()) {
            return Domain::Result<DashboardConnectionRuntimeIdentity>::failure(
                identityExhaustionError());
        }

        registrationIds_ = stagedRegistrationIds;
        return Domain::Result<DashboardConnectionRuntimeIdentity>::success(
            DashboardConnectionRuntimeIdentity{
                *registrationId,
                completionKey});
    } catch (...) {
        return Domain::Result<DashboardConnectionRuntimeIdentity>::failure(
            runtimeServicesError(
                Domain::ErrorCodes::InternalFailure,
                "The dashboard fixed identity could not be allocated."));
    }
}

Domain::Result<std::uint64_t>
DashboardConnectionRuntimeServices::allocateAuxiliaryRegistrationId() noexcept
{
    try {
        const std::lock_guard lock{identityMutex_};
        if (shutdown_) {
            return Domain::Result<std::uint64_t>::failure(
                runtimeAdmissionClosedError());
        }
        auto stagedRegistrationIds = registrationIds_;
        const auto registrationId = stagedRegistrationIds.tryTake(
            [](const std::uint64_t value) noexcept {
                return value == 0U;
            });
        if (!registrationId.has_value()) {
            return Domain::Result<std::uint64_t>::failure(
                identityExhaustionError());
        }

        registrationIds_ = stagedRegistrationIds;
        return Domain::Result<std::uint64_t>::success(*registrationId);
    } catch (...) {
        return Domain::Result<std::uint64_t>::failure(runtimeServicesError(
            Domain::ErrorCodes::InternalFailure,
            "The dashboard auxiliary registration identity could not be "
            "allocated."));
    }
}

Domain::Result<DashboardRuntimeResponseAdmission>
DashboardConnectionRuntimeServices::acquireResponseAdmission() noexcept
{
    try {
        DashboardRuntimeResponseAdmission admission{identityMutex_};
        if (shutdown_) {
            return Domain::Result<DashboardRuntimeResponseAdmission>::failure(
                runtimeAdmissionClosedError());
        }
        return Domain::Result<DashboardRuntimeResponseAdmission>::success(
            std::move(admission));
    } catch (...) {
        return Domain::Result<DashboardRuntimeResponseAdmission>::failure(
            runtimeServicesError(
                Domain::ErrorCodes::InternalFailure,
                "Dashboard response admission could not be acquired."));
    }
}

void DashboardConnectionRuntimeServices::beginShutdown() noexcept
{
    const std::lock_guard lock{identityMutex_};
    shutdown_ = true;
}

bool DashboardConnectionRuntimeServices::isShuttingDown() const noexcept
{
    const std::lock_guard lock{identityMutex_};
    return shutdown_;
}

Domain::MonotonicTimePoint
DashboardConnectionRuntimeServices::monotonicNow() const noexcept
{
    return clock_->monotonicNow();
}

bool DashboardConnectionRuntimeServices::operationalServiceActive()
    const noexcept
{
    return operationalState_->operationalServiceActive();
}

Domain::Result<Domain::OperationContext>
DashboardConnectionRuntimeServices::createOperationContext(
    const Domain::MonotonicTimePoint absoluteDeadlineCeiling,
    const std::stop_token cancellation) noexcept
{
    try {
        if (isShuttingDown()) {
            return Domain::Result<Domain::OperationContext>::failure(
                runtimeAdmissionClosedError());
        }
        if (cancellation.stop_requested()) {
            return Domain::Result<Domain::OperationContext>::failure(
                runtimeServicesError(
                    Domain::ErrorCodes::Cancelled,
                    "The dashboard operation was cancelled before "
                    "admission."));
        }
        const auto now = clock_->monotonicNow();
        if (absoluteDeadlineCeiling <= now) {
            return Domain::Result<Domain::OperationContext>::failure(
                runtimeServicesError(
                    Domain::ErrorCodes::DeadlineExceeded,
                    "The dashboard operation deadline expired before "
                    "admission."));
        }

        auto operationUuid = uuidGenerator_->next();
        if (!operationUuid) {
            return Domain::Result<Domain::OperationContext>::failure(
                std::move(operationUuid).error());
        }

        if (cancellation.stop_requested()) {
            return Domain::Result<Domain::OperationContext>::failure(
                runtimeServicesError(
                    Domain::ErrorCodes::Cancelled,
                    "The dashboard operation was cancelled while its "
                    "context was being created."));
        }
        if (absoluteDeadlineCeiling <= clock_->monotonicNow()) {
            return Domain::Result<Domain::OperationContext>::failure(
                runtimeServicesError(
                    Domain::ErrorCodes::DeadlineExceeded,
                    "The dashboard operation deadline expired while its "
                    "context was being created."));
        }

        auto correlationUuid = uuidGenerator_->next();
        if (!correlationUuid) {
            return Domain::Result<Domain::OperationContext>::failure(
                std::move(correlationUuid).error());
        }
        auto correlationId = Domain::CorrelationId::parse(
            correlationUuid.value().value());
        if (!correlationId) {
            return Domain::Result<Domain::OperationContext>::failure(
                std::move(correlationId).error());
        }

        const auto finalNow = clock_->monotonicNow();
        if (cancellation.stop_requested()) {
            return Domain::Result<Domain::OperationContext>::failure(
                runtimeServicesError(
                    Domain::ErrorCodes::Cancelled,
                    "The dashboard operation was cancelled while its "
                    "context was being created."));
        }
        if (absoluteDeadlineCeiling <= finalNow) {
            return Domain::Result<Domain::OperationContext>::failure(
                runtimeServicesError(
                    Domain::ErrorCodes::DeadlineExceeded,
                    "The dashboard operation deadline expired while its "
                    "context was being created."));
        }
        const auto finalRemaining = absoluteDeadlineCeiling - finalNow;
        const auto deadline = finalRemaining > MaximumOperationLifetime
            ? finalNow + MaximumOperationLifetime
            : absoluteDeadlineCeiling;

        // The final admission edge shares the same mutex as identity
        // allocation and beginShutdown(). UUID generation stays outside this
        // critical section so an injected or platform generator cannot block
        // unrelated callers or the shutdown cutover.
        const std::lock_guard admissionLock{identityMutex_};
        if (shutdown_) {
            return Domain::Result<Domain::OperationContext>::failure(
                runtimeAdmissionClosedError());
        }

        return Domain::Result<Domain::OperationContext>::success(
            Domain::OperationContext{
                Domain::OperationId{std::move(operationUuid).value()},
                deadline,
                cancellation,
                std::move(correlationId).value()});
    } catch (...) {
        return Domain::Result<Domain::OperationContext>::failure(
            runtimeServicesError(
                Domain::ErrorCodes::InternalFailure,
                "The dashboard operation context could not be created."));
    }
}

bool DashboardConnectionRuntimeServices::isReservedCompletionKey(
    const std::uintptr_t value) const noexcept
{
    if (value == 0U ||
        value == DashboardIocpWorkerKernel::ShutdownKeyValue) {
        return true;
    }
    return std::binary_search(
        fixedCompletionKeys_.begin(), fixedCompletionKeys_.end(), value);
}

bool DashboardConnectionRuntimeServices::isFixedCompletionKey(
    const std::uintptr_t value) const noexcept
{
    return std::binary_search(
        fixedCompletionKeys_.begin(), fixedCompletionKeys_.end(), value);
}

} // namespace ForgeConductor::Infrastructure::Windows::Detail
