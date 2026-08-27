#include "ForgeConductor/Manager/ManagerRequestDispatcher.h"

#include "ForgeConductor/Manager/ManagerDeadlineMapper.h"

#include <algorithm>
#include <condition_variable>
#include <map>
#include <memory>
#include <mutex>
#include <stop_token>
#include <stdexcept>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace ForgeConductor::Manager {
namespace {

[[nodiscard]] Domain::Error error(
    const std::string_view code,
    const char* const message,
    const bool retryable = false)
{
    return Domain::makeError(code, message, retryable);
}

[[nodiscard]] ManagerResponse responseWithError(
    const ManagerRequest& request,
    Domain::Error failure)
{
    return ManagerResponse{
        ManagerProtocolVersion,
        request.requestId,
        request.correlationId,
        ManagerResponseBody{std::in_place_type<Domain::Error>,
                            std::move(failure)}};
}

template <typename T>
[[nodiscard]] ManagerResponse responseWithResult(
    const ManagerRequest& request,
    T value)
{
    return ManagerResponse{
        ManagerProtocolVersion,
        request.requestId,
        request.correlationId,
        ManagerResponseBody{
            std::in_place_type<ManagerResult>,
            ManagerResult{std::in_place_type<T>, std::move(value)}}};
}

[[nodiscard]] ManagerResponse acknowledgement(const ManagerRequest& request)
{
    return responseWithResult(request, ManagerAcknowledgement{});
}

[[nodiscard]] std::chrono::milliseconds nonnegativeRemaining(
    const Domain::MonotonicTimePoint deadline,
    const Domain::MonotonicTimePoint now) noexcept
{
    if (now >= deadline) {
        return std::chrono::milliseconds::zero();
    }
    const auto remaining = deadline - now;
    auto rounded = std::chrono::ceil<std::chrono::milliseconds>(remaining);
    if (rounded < std::chrono::milliseconds::zero()) {
        rounded = std::chrono::milliseconds::zero();
    }
    return rounded;
}

} // namespace

class ManagerRequestDispatcher::Implementation final {
public:
    Implementation(
        std::shared_ptr<Contracts::IManagerController> controller,
        std::shared_ptr<Contracts::IClock> clock,
        ManagerTransportLimits limits)
        : controller_{std::move(controller)},
          clock_{std::move(clock)},
          limits_{std::move(limits)}
    {
        limits_.maximumActiveRegularOperations = (std::min)(
            limits_.maximumActiveRegularOperations,
            MaximumActiveRegularOperations);
    }

    [[nodiscard]] ManagerResponse dispatch(
        const ManagerRequest& request) noexcept
    {
        try {
            if (request.version != ManagerProtocolVersion) {
                return responseWithError(
                    request,
                    error(
                        Domain::ErrorCodes::UnsupportedVersion,
                        "The manager request protocol version is unsupported."));
            }

            auto operationId = Domain::OperationId::parse(
                request.requestId.value());
            if (!operationId) {
                return responseWithError(request, std::move(operationId).error());
            }
            auto deadline = fromManagerWireDeadline(
                request.deadlineUtcMilliseconds, *clock_, limits_);
            if (!deadline) {
                return responseWithError(request, std::move(deadline).error());
            }

            if (const auto* cancelRequest =
                    std::get_if<ManagerCancelRequest>(&request.payload)) {
                cancel(cancelRequest->operationId);
                return acknowledgement(request);
            }
            if (std::holds_alternative<ManagerShutdownRequest>(request.payload)) {
                return dispatchShutdown(
                    request,
                    std::move(operationId).value(),
                    deadline.value());
            }

            auto admitted = admit(std::move(operationId).value());
            if (!admitted) {
                return responseWithError(request, std::move(admitted).error());
            }
            auto active = std::move(admitted).value();
            ActiveLease lease{*this, active};
            const Domain::OperationContext context{
                active->operationId,
                deadline.value(),
                active->cancellation.get_token(),
                request.correlationId};

            if (auto current = validateContext(context); !current) {
                return responseWithError(request, std::move(current).error());
            }
            auto response = dispatchRegular(request, context);
            if (auto current = validateContext(context); !current) {
                return responseWithError(request, std::move(current).error());
            }
            return response;
        } catch (...) {
            return responseWithError(
                request,
                error(
                    Domain::ErrorCodes::InternalFailure,
                    "The manager request dispatcher failed safely."));
        }
    }

    void beginShutdown() noexcept
    {
        try {
            std::vector<std::shared_ptr<ActiveOperation>> active;
            {
                std::lock_guard lock{stateMutex_};
                accepting_ = false;
                active.reserve(activeOperations_.size());
                for (const auto& entry : activeOperations_) {
                    active.push_back(entry.second);
                }
            }
            for (const auto& operation : active) {
                operation->cancellation.request_stop();
            }
            stateChanged_.notify_all();
        } catch (...) {
            // Shutdown remains best effort at a noexcept ownership boundary.
        }
    }

    void cancel(const Domain::OperationId& operationId) noexcept
    {
        try {
            std::shared_ptr<ActiveOperation> active;
            {
                std::lock_guard lock{stateMutex_};
                const auto found = activeOperations_.find(operationId);
                if (found != activeOperations_.end()) {
                    active = found->second;
                }
            }
            if (active) {
                active->cancellation.request_stop();
            }
        } catch (...) {
        }
    }

    [[nodiscard]] bool waitUntilIdle(
        const std::chrono::milliseconds timeout) noexcept
    {
        if (timeout < std::chrono::milliseconds::zero()) {
            return false;
        }
        try {
            std::unique_lock lock{stateMutex_};
            return stateChanged_.wait_for(
                lock,
                timeout,
                [this] { return activeOperations_.empty(); });
        } catch (...) {
            return false;
        }
    }

    [[nodiscard]] std::size_t activeOperationCount() const noexcept
    {
        try {
            std::lock_guard lock{stateMutex_};
            return activeOperations_.size();
        } catch (...) {
            return 0U;
        }
    }

    [[nodiscard]] bool isAccepting() const noexcept
    {
        try {
            std::lock_guard lock{stateMutex_};
            return accepting_;
        } catch (...) {
            return false;
        }
    }

    void shutdown() noexcept
    {
        beginShutdown();
        static_cast<void>(waitUntilIdle(limits_.shutdownDrainTimeout));
        bool closeController{};
        {
            try {
                std::lock_guard lock{stateMutex_};
                closeControllerWhenIdle_ = true;
                if (activeOperations_.empty() && !controllerClosed_) {
                    controllerClosed_ = true;
                    closeController = true;
                }
            } catch (...) {
            }
        }
        if (closeController) {
            try {
                controller_->shutdown();
            } catch (...) {
            }
        }
    }

private:
    static constexpr std::size_t MaximumActiveRegularOperations = 3U;

    struct ActiveOperation final {
        explicit ActiveOperation(Domain::OperationId id)
            : operationId{std::move(id)}
        {
        }

        Domain::OperationId operationId;
        std::stop_source cancellation;
    };

    class ActiveLease final {
    public:
        ActiveLease(
            Implementation& owner,
            std::shared_ptr<ActiveOperation> active) noexcept
            : owner_{owner}, active_{std::move(active)}
        {
        }

        ~ActiveLease() noexcept { owner_.release(active_); }

        ActiveLease(const ActiveLease&) = delete;
        ActiveLease& operator=(const ActiveLease&) = delete;
        ActiveLease(ActiveLease&&) = delete;
        ActiveLease& operator=(ActiveLease&&) = delete;

    private:
        Implementation& owner_;
        std::shared_ptr<ActiveOperation> active_;
    };

    [[nodiscard]] Domain::Result<std::shared_ptr<ActiveOperation>> admit(
        Domain::OperationId operationId)
    {
        try {
            auto active = std::make_shared<ActiveOperation>(
                std::move(operationId));
            std::lock_guard lock{stateMutex_};
            if (!accepting_) {
                return Domain::Result<std::shared_ptr<ActiveOperation>>::failure(
                    error(
                        Domain::ErrorCodes::TransportClosed,
                        "The manager dispatcher is no longer accepting work."));
            }
            if (activeOperations_.contains(active->operationId)) {
                return Domain::Result<std::shared_ptr<ActiveOperation>>::failure(
                    error(
                        Domain::ErrorCodes::Conflict,
                        "The manager request identifier is already active."));
            }
            if (activeOperations_.size() >=
                limits_.maximumActiveRegularOperations) {
                return Domain::Result<std::shared_ptr<ActiveOperation>>::failure(
                    error(
                        Domain::ErrorCodes::LimitExceeded,
                        "The manager active-operation bound was reached.",
                        true));
            }
            activeOperations_.emplace(active->operationId, active);
            return Domain::Result<std::shared_ptr<ActiveOperation>>::success(
                std::move(active));
        } catch (...) {
            return Domain::Result<std::shared_ptr<ActiveOperation>>::failure(
                error(
                    Domain::ErrorCodes::InternalFailure,
                    "The manager request could not be admitted."));
        }
    }

    void release(const std::shared_ptr<ActiveOperation>& active) noexcept
    {
        bool closeController{};
        try {
            {
                std::lock_guard lock{stateMutex_};
                const auto found = activeOperations_.find(active->operationId);
                if (found != activeOperations_.end() &&
                    found->second == active) {
                    activeOperations_.erase(found);
                }
                if (activeOperations_.empty() && closeControllerWhenIdle_ &&
                    !controllerClosed_) {
                    controllerClosed_ = true;
                    closeController = true;
                }
            }
            stateChanged_.notify_all();
            if (closeController) {
                controller_->shutdown();
            }
        } catch (...) {
        }
    }

    [[nodiscard]] Domain::Result<void> validateContext(
        const Domain::OperationContext& context) const noexcept
    {
        if (context.isCancellationRequested()) {
            return Domain::Result<void>::failure(
                error(
                    Domain::ErrorCodes::Cancelled,
                    "The manager request was cancelled."));
        }
        if (context.isExpired(clock_->monotonicNow())) {
            return Domain::Result<void>::failure(
                error(
                    Domain::ErrorCodes::DeadlineExceeded,
                    "The manager request deadline has expired."));
        }
        return Domain::Result<void>::success();
    }

    template <typename T>
    [[nodiscard]] ManagerResponse controllerResponse(
        const ManagerRequest& request,
        Domain::Result<T> result)
    {
        if (!result) {
            return responseWithError(request, std::move(result).error());
        }
        return responseWithResult(request, std::move(result).value());
    }

    [[nodiscard]] ManagerResponse dispatchRegular(
        const ManagerRequest& request,
        const Domain::OperationContext& context)
    {
        return std::visit(
            [&](const auto& payload) -> ManagerResponse {
                using Payload = std::remove_cvref_t<decltype(payload)>;
                if constexpr (std::is_same_v<Payload, ManagerStatusRequest>) {
                    return controllerResponse(
                        request, controller_->status(context));
                } else if constexpr (
                    std::is_same_v<Payload, ManagerSettingsRequest>) {
                    return controllerResponse(
                        request, controller_->settings(context));
                } else if constexpr (
                    std::is_same_v<Payload, Domain::ManagerControlRequest>) {
                    return controllerResponse(
                        request, controller_->control(payload, context));
                } else if constexpr (
                    std::is_same_v<Payload, ManagerSettingsUpdateRequest>) {
                    return controllerResponse(
                        request,
                        controller_->updateSettings(
                            payload.patch,
                            payload.applyImmediately,
                            context));
                } else {
                    return responseWithError(
                        request,
                        error(
                            Domain::ErrorCodes::InvalidRequest,
                            "The manager control request was dispatched incorrectly."));
                }
            },
            request.payload);
    }

    [[nodiscard]] ManagerResponse dispatchShutdown(
        const ManagerRequest& request,
        Domain::OperationId operationId,
        const Domain::MonotonicTimePoint deadline)
    {
        std::stop_source cancellation;
        const Domain::OperationContext context{
            std::move(operationId),
            deadline,
            cancellation.get_token(),
            request.correlationId};
        beginShutdown();

        const auto remaining = nonnegativeRemaining(
            deadline, clock_->monotonicNow());
        const auto drain = (std::min)(
            limits_.shutdownDrainTimeout, remaining);
        static_cast<void>(waitUntilIdle(drain));
        if (auto current = validateContext(context); !current) {
            return responseWithError(request, std::move(current).error());
        }

        auto shutdownResult = controller_->requestShutdown(context);
        if (!shutdownResult) {
            return responseWithError(
                request, std::move(shutdownResult).error());
        }
        return acknowledgement(request);
    }

    std::shared_ptr<Contracts::IManagerController> controller_;
    std::shared_ptr<Contracts::IClock> clock_;
    ManagerTransportLimits limits_;

    mutable std::mutex stateMutex_;
    std::condition_variable stateChanged_;
    std::map<Domain::OperationId, std::shared_ptr<ActiveOperation>>
        activeOperations_;
    bool accepting_{true};
    bool closeControllerWhenIdle_{};
    bool controllerClosed_{};
};

ManagerRequestDispatcher::ManagerRequestDispatcher(
    std::shared_ptr<Contracts::IManagerController> controller,
    std::shared_ptr<Contracts::IClock> clock,
    ManagerTransportLimits limits)
{
    if (!controller) {
        throw std::invalid_argument{
            "The manager request dispatcher requires a controller."};
    }
    if (!clock) {
        throw std::invalid_argument{
            "The manager request dispatcher requires a clock."};
    }
    implementation_ = std::make_shared<Implementation>(
        std::move(controller), std::move(clock), std::move(limits));
}

ManagerRequestDispatcher::~ManagerRequestDispatcher() noexcept
{
    shutdown();
}

ManagerResponse ManagerRequestDispatcher::dispatch(
    const ManagerRequest& request) noexcept
{
    const auto implementation = implementation_;
    if (!implementation) {
        return responseWithError(
            request,
            error(
                Domain::ErrorCodes::TransportClosed,
                "The manager dispatcher has no implementation."));
    }
    return implementation->dispatch(request);
}

void ManagerRequestDispatcher::beginShutdown() noexcept
{
    if (const auto implementation = implementation_) {
        implementation->beginShutdown();
    }
}

void ManagerRequestDispatcher::cancel(
    const Domain::OperationId& operationId) noexcept
{
    if (const auto implementation = implementation_) {
        implementation->cancel(operationId);
    }
}

bool ManagerRequestDispatcher::waitUntilIdle(
    const std::chrono::milliseconds timeout) noexcept
{
    const auto implementation = implementation_;
    return !implementation || implementation->waitUntilIdle(timeout);
}

std::size_t ManagerRequestDispatcher::activeOperationCount() const noexcept
{
    const auto implementation = implementation_;
    return implementation ? implementation->activeOperationCount() : 0U;
}

bool ManagerRequestDispatcher::isAccepting() const noexcept
{
    const auto implementation = implementation_;
    return implementation && implementation->isAccepting();
}

void ManagerRequestDispatcher::shutdown() noexcept
{
    if (const auto implementation = implementation_) {
        implementation->shutdown();
    }
}

} // namespace ForgeConductor::Manager
