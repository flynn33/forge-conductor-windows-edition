#include "DashboardHandlerOperations.h"

#include "ForgeConductor/Dashboard/DashboardResponseComposer.h"
#include "ForgeConductor/Domain/Error.h"

#include <string>
#include <string_view>
#include <utility>

namespace ForgeConductor::Infrastructure::Windows::Detail {
namespace {

[[nodiscard]] Domain::Error handlerOperationError(
    const std::string_view code,
    std::string message)
{
    return Domain::makeError(code, std::move(message));
}

[[nodiscard]] DashboardHandlerCompletion preparedFailure(
    const std::string_view code,
    std::string message)
{
    return DashboardHandlerCompletion::prepared(
        Domain::Result<Dashboard::DashboardPreparedExchange>::failure(
            handlerOperationError(code, std::move(message))));
}

[[nodiscard]] DashboardHandlerCompletion postDeliveryFailure(
    const std::string_view code,
    std::string message)
{
    return DashboardHandlerCompletion::postDelivery(
        Domain::Result<void>::failure(
            handlerOperationError(code, std::move(message))));
}

} // namespace

Domain::Result<std::unique_ptr<DashboardPrepareHandlerOperation>>
DashboardPrepareHandlerOperation::createRequest(
    std::shared_ptr<Dashboard::IDashboardConnectionApplication> application,
    Dashboard::DashboardHttpRequest request,
    const bool operationalServiceActive) noexcept
{
    using CreationResult = Domain::Result<
        std::unique_ptr<DashboardPrepareHandlerOperation>>;
    try {
        if (application == nullptr) {
            return CreationResult::failure(handlerOperationError(
                Domain::ErrorCodes::InvalidRequest,
                "A dashboard prepare operation requires an application."));
        }
        return CreationResult::success(
            std::unique_ptr<DashboardPrepareHandlerOperation>{
                new DashboardPrepareHandlerOperation{
                    std::move(application),
                    std::move(request),
                    operationalServiceActive}});
    } catch (...) {
        return CreationResult::failure(handlerOperationError(
            Domain::ErrorCodes::InternalFailure,
            "The dashboard prepare operation could not be created."));
    }
}

Domain::Result<std::unique_ptr<DashboardPrepareHandlerOperation>>
DashboardPrepareHandlerOperation::createRejection(
    Dashboard::DashboardHttpRejection rejection) noexcept
{
    using CreationResult = Domain::Result<
        std::unique_ptr<DashboardPrepareHandlerOperation>>;
    try {
        return CreationResult::success(
            std::unique_ptr<DashboardPrepareHandlerOperation>{
                new DashboardPrepareHandlerOperation{
                    std::move(rejection)}});
    } catch (...) {
        return CreationResult::failure(handlerOperationError(
            Domain::ErrorCodes::InternalFailure,
            "The dashboard rejection operation could not be created."));
    }
}

DashboardPrepareHandlerOperation::DashboardPrepareHandlerOperation(
    std::shared_ptr<Dashboard::IDashboardConnectionApplication> application,
    Dashboard::DashboardHttpRequest request,
    const bool operationalServiceActive)
    : application_{std::move(application)},
      request_{std::make_unique<Dashboard::DashboardHttpRequest>(
          std::move(request))},
      operationalServiceActive_{operationalServiceActive}
{
}

DashboardPrepareHandlerOperation::DashboardPrepareHandlerOperation(
    Dashboard::DashboardHttpRejection rejection)
    : rejection_{std::make_unique<Dashboard::DashboardHttpRejection>(
          std::move(rejection))}
{
}

DashboardHandlerCompletionKind
DashboardPrepareHandlerOperation::completionKind() const noexcept
{
    return DashboardHandlerCompletionKind::PreparedExchange;
}

DashboardHandlerCompletion DashboardPrepareHandlerOperation::execute(
    const Domain::OperationContext& context)
{
    if (executed_) {
        return preparedFailure(
            Domain::ErrorCodes::IntegrityFailure,
            "The dashboard prepare operation was executed more than once.");
    }
    executed_ = true;

    if (request_ != nullptr && application_ != nullptr &&
        rejection_ == nullptr) {
        auto request = std::move(*request_);
        request_.reset();
        return DashboardHandlerCompletion::prepared(application_->prepare(
            std::move(request), operationalServiceActive_, context));
    }
    if (rejection_ != nullptr && request_ == nullptr &&
        application_ == nullptr) {
        auto rejection = std::move(*rejection_);
        rejection_.reset();
        return DashboardHandlerCompletion::prepared(
            Dashboard::DashboardResponseComposer::rejection(rejection));
    }

    request_.reset();
    rejection_.reset();
    application_.reset();
    return preparedFailure(
        Domain::ErrorCodes::IntegrityFailure,
        "The dashboard prepare operation lost its one-shot payload.");
}

Domain::Result<std::unique_ptr<DashboardPostDeliveryHandlerOperation>>
DashboardPostDeliveryHandlerOperation::create(
    std::shared_ptr<Dashboard::IDashboardConnectionApplication> application,
    const Dashboard::DashboardPostDeliveryAction action) noexcept
{
    using CreationResult = Domain::Result<
        std::unique_ptr<DashboardPostDeliveryHandlerOperation>>;
    try {
        if (application == nullptr ||
            action !=
                Dashboard::DashboardPostDeliveryAction::RequestManagerShutdown) {
            return CreationResult::failure(handlerOperationError(
                Domain::ErrorCodes::InvalidRequest,
                "A dashboard post-delivery operation requires an application "
                "and the sole defined nonempty action."));
        }
        return CreationResult::success(
            std::unique_ptr<DashboardPostDeliveryHandlerOperation>{
                new DashboardPostDeliveryHandlerOperation{
                    std::move(application), action}});
    } catch (...) {
        return CreationResult::failure(handlerOperationError(
            Domain::ErrorCodes::InternalFailure,
            "The dashboard post-delivery operation could not be created."));
    }
}

DashboardPostDeliveryHandlerOperation::
    DashboardPostDeliveryHandlerOperation(
        std::shared_ptr<Dashboard::IDashboardConnectionApplication>
            application,
        const Dashboard::DashboardPostDeliveryAction action) noexcept
    : application_{std::move(application)}, action_{action}
{
}

DashboardHandlerCompletionKind
DashboardPostDeliveryHandlerOperation::completionKind() const noexcept
{
    return DashboardHandlerCompletionKind::PostDelivery;
}

DashboardHandlerCompletion DashboardPostDeliveryHandlerOperation::execute(
    const Domain::OperationContext& context)
{
    if (executed_) {
        return postDeliveryFailure(
            Domain::ErrorCodes::IntegrityFailure,
            "The dashboard post-delivery operation was executed more than "
            "once.");
    }
    executed_ = true;

    if (application_ == nullptr ||
        action_ !=
            Dashboard::DashboardPostDeliveryAction::RequestManagerShutdown) {
        application_.reset();
        action_ = Dashboard::DashboardPostDeliveryAction::None;
        return postDeliveryFailure(
            Domain::ErrorCodes::IntegrityFailure,
            "The dashboard post-delivery operation lost its one-shot "
            "payload.");
    }

    const auto action = std::exchange(
        action_, Dashboard::DashboardPostDeliveryAction::None);
    return DashboardHandlerCompletion::postDelivery(
        application_->executePostDelivery(action, context));
}

} // namespace ForgeConductor::Infrastructure::Windows::Detail
