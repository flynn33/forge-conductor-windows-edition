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
        "The dashboard connection identity sequence is exhausted.");
}

} // namespace

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

} // namespace ForgeConductor::Infrastructure::Windows::Detail
