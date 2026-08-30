#include "UnavailableLmStudioDeploymentService.h"

#include "ForgeConductor/Domain/Error.h"

#include <stdexcept>
#include <utility>

namespace ForgeConductor::Composition::Windows {
namespace {

template <typename Value>
[[nodiscard]] Domain::Result<Value> unavailable(const std::string& reason)
{
    return Domain::Result<Value>::failure(Domain::makeError(
        Domain::ErrorCodes::HostCapabilityUnavailable, reason, true));
}

[[nodiscard]] Domain::Error internalError()
{
    return Domain::makeError(
        Domain::ErrorCodes::InternalFailure,
        "The unavailable LM Studio deployment capability failed safely.");
}

} // namespace

UnavailableLmStudioDeploymentService::UnavailableLmStudioDeploymentService(
    const Contracts::IClock& clock,
    std::string reason)
    : clock_{clock}, reason_{std::move(reason)}
{
    if (reason_.empty() || reason_.size() > MaximumReasonBytes) {
        throw std::invalid_argument{
            "UnavailableLmStudioDeploymentService requires a bounded reason."};
    }
}

UnavailableLmStudioDeploymentService::~UnavailableLmStudioDeploymentService()
    noexcept
{
    shutdown();
}

Domain::Result<void>
UnavailableLmStudioDeploymentService::validateContext(
    const Domain::OperationContext& context) const noexcept
{
    try {
        if (context.isCancellationRequested()) {
            return Domain::Result<void>::failure(Domain::makeError(
                Domain::ErrorCodes::Cancelled,
                "The unavailable LM Studio deployment operation was cancelled."));
        }
        if (context.isExpired(clock_.monotonicNow())) {
            return Domain::Result<void>::failure(Domain::makeError(
                Domain::ErrorCodes::DeadlineExceeded,
                "The unavailable LM Studio deployment operation deadline expired.",
                true));
        }

        std::scoped_lock lock{stateMutex_};
        if (shutdown_) {
            return Domain::Result<void>::failure(Domain::makeError(
                Domain::ErrorCodes::Cancelled,
                "The unavailable LM Studio deployment service is shut down."));
        }
        if (cancelledOperation_ &&
            cancelledOperation_.value() == context.operationId) {
            return Domain::Result<void>::failure(Domain::makeError(
                Domain::ErrorCodes::Cancelled,
                "The unavailable LM Studio deployment operation was cancelled."));
        }
        return Domain::Result<void>::success();
    } catch (...) {
        return Domain::Result<void>::failure(internalError());
    }
}

Domain::Result<Domain::LMStudioPluginStatus>
UnavailableLmStudioDeploymentService::status(
    const Domain::LMStudioDeploymentRequest& request,
    const Contracts::WorkspaceAuthority& readAuthority,
    const Domain::OperationContext& context) noexcept
{
    try {
        static_cast<void>(request);
        static_cast<void>(readAuthority);
        auto active = validateContext(context);
        if (!active) {
            return Domain::Result<Domain::LMStudioPluginStatus>::failure(
                std::move(active).error());
        }
        return unavailable<Domain::LMStudioPluginStatus>(reason_);
    } catch (...) {
        return Domain::Result<Domain::LMStudioPluginStatus>::failure(
            internalError());
    }
}

Domain::Result<Domain::LMStudioInstallResult>
UnavailableLmStudioDeploymentService::deploy(
    const Domain::LMStudioDeploymentRequest& request,
    const Contracts::WorkspaceAuthority& writeAuthority,
    const Contracts::AuthorizedToolCall& authorization,
    const Domain::OperationContext& context) noexcept
{
    try {
        static_cast<void>(request);
        static_cast<void>(writeAuthority);
        static_cast<void>(authorization);
        auto active = validateContext(context);
        if (!active) {
            return Domain::Result<Domain::LMStudioInstallResult>::failure(
                std::move(active).error());
        }
        return unavailable<Domain::LMStudioInstallResult>(reason_);
    } catch (...) {
        return Domain::Result<Domain::LMStudioInstallResult>::failure(
            internalError());
    }
}

Domain::Result<Domain::LMStudioHostActivationResult>
UnavailableLmStudioDeploymentService::activate(
    const Domain::LMStudioHostActivationRequest& request,
    const Contracts::WorkspaceAuthority& executionAuthority,
    const Contracts::AuthorizedToolCall& authorization,
    const Domain::OperationContext& context) noexcept
{
    try {
        static_cast<void>(request);
        static_cast<void>(executionAuthority);
        static_cast<void>(authorization);
        auto active = validateContext(context);
        if (!active) {
            return Domain::Result<Domain::LMStudioHostActivationResult>::failure(
                std::move(active).error());
        }
        return unavailable<Domain::LMStudioHostActivationResult>(reason_);
    } catch (...) {
        return Domain::Result<
            Domain::LMStudioHostActivationResult>::failure(internalError());
    }
}

void UnavailableLmStudioDeploymentService::cancel(
    const Domain::OperationId& operationId) noexcept
{
    try {
        std::scoped_lock lock{stateMutex_};
        if (!shutdown_) {
            cancelledOperation_ = operationId;
        }
    } catch (...) {
    }
}

void UnavailableLmStudioDeploymentService::shutdown() noexcept
{
    try {
        std::scoped_lock lock{stateMutex_};
        shutdown_ = true;
        cancelledOperation_.reset();
    } catch (...) {
    }
}

} // namespace ForgeConductor::Composition::Windows
