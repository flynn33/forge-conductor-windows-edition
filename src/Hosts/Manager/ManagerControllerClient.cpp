#include "ManagerControllerClient.h"

#include "ManagerProcessRestartSignal.h"

#include "ForgeConductor/Domain/Error.h"

#include <string>
#include <string_view>
#include <utility>

namespace ForgeConductor::Hosts::Manager {
namespace {

[[nodiscard]] Domain::Error clientError(
    const std::string_view code,
    std::string message)
{
    return Domain::makeError(code, std::move(message));
}

} // namespace

ManagerControllerClient::ManagerControllerClient(
    ManagerProcessRestartSignal& restartSignal) noexcept
    : restartSignal_{restartSignal}
{
}

ManagerControllerClient::~ManagerControllerClient() noexcept
{
    shutdown();
}

Domain::Result<void> ManagerControllerClient::bind(
    const std::shared_ptr<Contracts::IManagerController>& controller) noexcept
{
    try {
        const std::lock_guard lock{mutex_};
        if (shutdown_) {
            return Domain::Result<void>::failure(clientError(
                Domain::ErrorCodes::TransportClosed,
                "The local manager controller client is closed."));
        }
        if (bound_) {
            return Domain::Result<void>::failure(clientError(
                Domain::ErrorCodes::Conflict,
                "The local manager controller client is already bound."));
        }
        if (!controller) {
            return Domain::Result<void>::failure(clientError(
                Domain::ErrorCodes::Conflict,
                "The local manager controller client requires a live controller."));
        }

        controller_ = controller;
        bound_ = true;
        return Domain::Result<void>::success();
    } catch (...) {
        return Domain::Result<void>::failure(clientError(
            Domain::ErrorCodes::InternalFailure,
            "The local manager controller client bind failed."));
    }
}

Domain::Result<std::shared_ptr<Contracts::IManagerController>>
ManagerControllerClient::pinController() noexcept
{
    using Result =
        Domain::Result<std::shared_ptr<Contracts::IManagerController>>;
    try {
        const std::lock_guard lock{mutex_};
        if (shutdown_) {
            return Result::failure(clientError(
                Domain::ErrorCodes::TransportClosed,
                "The local manager controller client is closed."));
        }
        if (!bound_) {
            return Result::failure(clientError(
                Domain::ErrorCodes::TransportClosed,
                "The local manager controller client is not bound."));
        }

        auto controller = controller_.lock();
        if (!controller) {
            return Result::failure(clientError(
                Domain::ErrorCodes::TransportClosed,
                "The local manager controller is no longer available."));
        }
        return Result::success(std::move(controller));
    } catch (...) {
        return Result::failure(clientError(
            Domain::ErrorCodes::InternalFailure,
            "The local manager controller client could not pin its controller."));
    }
}

Domain::Result<Domain::ManagerStatus> ManagerControllerClient::status(
    const Domain::OperationContext& context) noexcept
{
    auto controller = pinController();
    if (!controller) {
        return Domain::Result<Domain::ManagerStatus>::failure(
            std::move(controller).error());
    }
    return controller.value()->status(context);
}

Domain::Result<Domain::ManagerSettings> ManagerControllerClient::settings(
    const Domain::OperationContext& context) noexcept
{
    auto controller = pinController();
    if (!controller) {
        return Domain::Result<Domain::ManagerSettings>::failure(
            std::move(controller).error());
    }
    return controller.value()->settings(context);
}

Domain::Result<Domain::ManagerStatus> ManagerControllerClient::control(
    const Domain::ManagerControlRequest& request,
    const Domain::OperationContext& context) noexcept
{
    auto controller = pinController();
    if (!controller) {
        return Domain::Result<Domain::ManagerStatus>::failure(
            std::move(controller).error());
    }
    return controller.value()->control(request, context);
}

Domain::Result<Domain::ManagerSettingsUpdateOutcome>
ManagerControllerClient::updateSettings(
    const Domain::ManagerSettingsPatch& patch,
    const bool applyImmediately,
    const Domain::OperationContext& context) noexcept
{
    auto controller = pinController();
    if (!controller) {
        return Domain::Result<Domain::ManagerSettingsUpdateOutcome>::failure(
            std::move(controller).error());
    }
    return controller.value()->updateSettings(
        patch, applyImmediately, context);
}

Domain::Result<void> ManagerControllerClient::requestRestart(
    const Domain::OperationContext&) noexcept
{
    try {
        {
            const std::lock_guard lock{mutex_};
            if (shutdown_) {
                return Domain::Result<void>::failure(clientError(
                    Domain::ErrorCodes::TransportClosed,
                    "The local manager controller client is closed."));
            }
            if (!bound_ || controller_.expired()) {
                return Domain::Result<void>::failure(clientError(
                    Domain::ErrorCodes::TransportClosed,
                    "The local manager controller is unavailable for restart."));
            }
        }

        const auto requested = restartSignal_.requestRestart();
        if (requested == ManagerProcessRestartRequestResult::Closed) {
            return Domain::Result<void>::failure(clientError(
                Domain::ErrorCodes::TransportClosed,
                "The manager restart transition edge is closed."));
        }
        return Domain::Result<void>::success();
    } catch (...) {
        return Domain::Result<void>::failure(clientError(
            Domain::ErrorCodes::InternalFailure,
            "The local manager restart request failed."));
    }
}

Domain::Result<void> ManagerControllerClient::requestShutdown(
    const Domain::OperationContext& context) noexcept
{
    auto controller = pinController();
    if (!controller) {
        return Domain::Result<void>::failure(std::move(controller).error());
    }

    auto requested = controller.value()->requestShutdown(context);
    if (!requested) {
        return Domain::Result<void>::failure(std::move(requested).error());
    }
    return Domain::Result<void>::success();
}

void ManagerControllerClient::shutdown() noexcept
{
    try {
        const std::lock_guard lock{mutex_};
        if (shutdown_) {
            return;
        }
        shutdown_ = true;
        controller_.reset();
    } catch (...) {
    }
}

} // namespace ForgeConductor::Hosts::Manager
