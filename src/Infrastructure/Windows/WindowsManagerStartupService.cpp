#include "ForgeConductor/Infrastructure/Windows/WindowsManagerStartupService.h"

#include "Detail/IWindowsTaskSchedulerStartupPlatform.h"
#include "Detail/ManagerStartupComWorker.h"
#include "Detail/WindowsManagerStartupComHandler.h"

#include <exception>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

namespace ForgeConductor::Infrastructure::Windows {
namespace {

template <typename T>
[[nodiscard]] Domain::Result<T> closedService()
{
    return Domain::Result<T>::failure(Domain::makeError(
        Domain::ErrorCodes::TransportClosed,
        "The Windows Manager startup service is not available."));
}

template <typename T>
[[nodiscard]] Domain::Result<T> wrongResponse()
{
    return Domain::Result<T>::failure(Domain::makeError(
        Domain::ErrorCodes::IntegrityFailure,
        "The Windows Manager startup worker returned the wrong response type."));
}

template <typename T>
[[nodiscard]] Domain::Result<T> boundaryFailure(
    const std::string_view operation,
    const std::exception* exception = nullptr)
{
    try {
        std::string message =
            "The Windows Manager startup service failed while attempting to ";
        message.append(operation);
        if (exception != nullptr) {
            message += ": ";
            message += exception->what();
        } else {
            message += " with an unknown exception";
        }
        message += '.';
        return Domain::Result<T>::failure(Domain::makeError(
            Domain::ErrorCodes::InternalFailure,
            std::move(message)));
    } catch (...) {
        return Domain::Result<T>::failure(Domain::makeError(
            Domain::ErrorCodes::InternalFailure,
            "The Windows Manager startup service failed at its native boundary."));
    }
}

} // namespace

class WindowsManagerStartupService::Impl final {
public:
    Impl(
        WindowsManagerStartupServiceOptions options,
        std::shared_ptr<Detail::IWindowsTaskSchedulerStartupPlatform> platform)
        : purposeSuffix_{std::move(options.purposeSuffix)}
    {
        try {
            if (!platform) {
                initializationError_ = Domain::makeError(
                    Domain::ErrorCodes::IntegrityFailure,
                    "The Windows Manager startup service requires a Task Scheduler platform.");
                return;
            }
            auto handler =
                std::make_shared<Detail::WindowsManagerStartupComHandler>(
                    std::move(platform));
            auto worker = Detail::ManagerStartupComWorker::create(
                std::move(handler));
            if (!worker) {
                initializationError_ = std::move(worker).error();
                return;
            }
            worker_ = std::move(worker).value();
        } catch (const std::exception& exception) {
            initializationError_ = Domain::makeError(
                Domain::ErrorCodes::InternalFailure,
                std::string{
                    "The Windows Manager startup service could not initialize: "} +
                    exception.what());
        } catch (...) {
            initializationError_ = Domain::makeError(
                Domain::ErrorCodes::InternalFailure,
                "The Windows Manager startup service could not initialize.");
        }
    }

    [[nodiscard]] Domain::Result<Domain::ManagerStartupStatus> inspect(
        const Domain::ManagerStartupDefinition& expected,
        const Domain::OperationContext& context)
    {
        auto response = execute(
            Detail::ManagerStartupComOperationKind::Inspect,
            expected,
            false,
            context);
        if (!response) {
            return Domain::Result<Domain::ManagerStartupStatus>::failure(
                std::move(response).error());
        }
        auto* status =
            std::get_if<Domain::ManagerStartupStatus>(&response.value());
        if (status == nullptr) {
            return wrongResponse<Domain::ManagerStartupStatus>();
        }
        return Domain::Result<Domain::ManagerStartupStatus>::success(
            std::move(*status));
    }

    [[nodiscard]] Domain::Result<Domain::ManagerStartupOutcome> mutate(
        const Detail::ManagerStartupComOperationKind kind,
        const Domain::ManagerStartupDefinition& expected,
        const bool enabled,
        const Domain::OperationContext& context)
    {
        auto response = execute(kind, expected, enabled, context);
        if (!response) {
            return Domain::Result<Domain::ManagerStartupOutcome>::failure(
                std::move(response).error());
        }
        auto* outcome =
            std::get_if<Domain::ManagerStartupOutcome>(&response.value());
        if (outcome == nullptr) {
            return wrongResponse<Domain::ManagerStartupOutcome>();
        }
        return Domain::Result<Domain::ManagerStartupOutcome>::success(
            std::move(*outcome));
    }

    void cancel(const Domain::OperationId& operationId) noexcept
    {
        if (worker_) {
            worker_->cancel(operationId);
        }
    }

    void shutdown() noexcept
    {
        if (worker_) {
            worker_->shutdown();
        }
    }

private:
    [[nodiscard]] Detail::ManagerStartupComResult execute(
        const Detail::ManagerStartupComOperationKind kind,
        const Domain::ManagerStartupDefinition& expected,
        const bool enabled,
        const Domain::OperationContext& context)
    {
        if (initializationError_.has_value()) {
            return Detail::ManagerStartupComResult::failure(
                *initializationError_);
        }
        if (!worker_) {
            return Detail::ManagerStartupComResult::failure(
                closedService<Detail::ManagerStartupComResponse>().error());
        }
        return worker_->execute(Detail::ManagerStartupComRequest{
            kind,
            expected,
            purposeSuffix_,
            enabled,
            context});
    }

    std::string purposeSuffix_;
    std::unique_ptr<Detail::ManagerStartupComWorker> worker_;
    std::optional<Domain::Error> initializationError_;
};

WindowsManagerStartupService::WindowsManagerStartupService(
    WindowsManagerStartupServiceOptions options)
    : WindowsManagerStartupService(
          std::move(options),
          Detail::createWindowsTaskSchedulerStartupPlatform())
{
}

WindowsManagerStartupService::WindowsManagerStartupService(
    WindowsManagerStartupServiceOptions options,
    std::shared_ptr<Detail::IWindowsTaskSchedulerStartupPlatform> platform)
    : implementation_{std::make_shared<Impl>(
          std::move(options),
          std::move(platform))}
{
}

WindowsManagerStartupService::~WindowsManagerStartupService()
{
    auto implementation = std::move(implementation_);
    if (implementation) {
        implementation->shutdown();
    }
}

Domain::Result<Domain::ManagerStartupStatus>
WindowsManagerStartupService::inspect(
    const Domain::ManagerStartupDefinition& expected,
    const Domain::OperationContext& context) noexcept
{
    try {
        const auto implementation = implementation_;
        return implementation
            ? implementation->inspect(expected, context)
            : closedService<Domain::ManagerStartupStatus>();
    } catch (const std::exception& exception) {
        return boundaryFailure<Domain::ManagerStartupStatus>(
            "inspect startup registration", &exception);
    } catch (...) {
        return boundaryFailure<Domain::ManagerStartupStatus>(
            "inspect startup registration");
    }
}

Domain::Result<Domain::ManagerStartupOutcome>
WindowsManagerStartupService::registerAtLogon(
    const Domain::ManagerStartupDefinition& expected,
    const Domain::OperationContext& context) noexcept
{
    try {
        const auto implementation = implementation_;
        return implementation
            ? implementation->mutate(
                  Detail::ManagerStartupComOperationKind::Register,
                  expected,
                  true,
                  context)
            : closedService<Domain::ManagerStartupOutcome>();
    } catch (const std::exception& exception) {
        return boundaryFailure<Domain::ManagerStartupOutcome>(
            "register startup at logon", &exception);
    } catch (...) {
        return boundaryFailure<Domain::ManagerStartupOutcome>(
            "register startup at logon");
    }
}

Domain::Result<Domain::ManagerStartupOutcome>
WindowsManagerStartupService::repair(
    const Domain::ManagerStartupDefinition& expected,
    const Domain::OperationContext& context) noexcept
{
    try {
        const auto implementation = implementation_;
        return implementation
            ? implementation->mutate(
                  Detail::ManagerStartupComOperationKind::Repair,
                  expected,
                  false,
                  context)
            : closedService<Domain::ManagerStartupOutcome>();
    } catch (const std::exception& exception) {
        return boundaryFailure<Domain::ManagerStartupOutcome>(
            "repair startup registration", &exception);
    } catch (...) {
        return boundaryFailure<Domain::ManagerStartupOutcome>(
            "repair startup registration");
    }
}

Domain::Result<Domain::ManagerStartupOutcome>
WindowsManagerStartupService::setEnabled(
    const Domain::ManagerStartupDefinition& expected,
    const bool enabled,
    const Domain::OperationContext& context) noexcept
{
    try {
        const auto implementation = implementation_;
        return implementation
            ? implementation->mutate(
                  Detail::ManagerStartupComOperationKind::SetEnabled,
                  expected,
                  enabled,
                  context)
            : closedService<Domain::ManagerStartupOutcome>();
    } catch (const std::exception& exception) {
        return boundaryFailure<Domain::ManagerStartupOutcome>(
            "change startup enablement", &exception);
    } catch (...) {
        return boundaryFailure<Domain::ManagerStartupOutcome>(
            "change startup enablement");
    }
}

Domain::Result<Domain::ManagerStartupOutcome>
WindowsManagerStartupService::startNow(
    const Domain::ManagerStartupDefinition& expected,
    const Domain::OperationContext& context) noexcept
{
    try {
        const auto implementation = implementation_;
        return implementation
            ? implementation->mutate(
                  Detail::ManagerStartupComOperationKind::StartNow,
                  expected,
                  true,
                  context)
            : closedService<Domain::ManagerStartupOutcome>();
    } catch (const std::exception& exception) {
        return boundaryFailure<Domain::ManagerStartupOutcome>(
            "start the registered Manager", &exception);
    } catch (...) {
        return boundaryFailure<Domain::ManagerStartupOutcome>(
            "start the registered Manager");
    }
}

Domain::Result<Domain::ManagerStartupOutcome>
WindowsManagerStartupService::remove(
    const Domain::ManagerStartupDefinition& expected,
    const Domain::OperationContext& context) noexcept
{
    try {
        const auto implementation = implementation_;
        return implementation
            ? implementation->mutate(
                  Detail::ManagerStartupComOperationKind::Remove,
                  expected,
                  false,
                  context)
            : closedService<Domain::ManagerStartupOutcome>();
    } catch (const std::exception& exception) {
        return boundaryFailure<Domain::ManagerStartupOutcome>(
            "remove startup registration", &exception);
    } catch (...) {
        return boundaryFailure<Domain::ManagerStartupOutcome>(
            "remove startup registration");
    }
}

void WindowsManagerStartupService::cancel(
    const Domain::OperationId& operationId) noexcept
{
    const auto implementation = implementation_;
    if (implementation) {
        implementation->cancel(operationId);
    }
}

void WindowsManagerStartupService::shutdown() noexcept
{
    const auto implementation = implementation_;
    if (implementation) {
        implementation->shutdown();
    }
}

} // namespace ForgeConductor::Infrastructure::Windows
