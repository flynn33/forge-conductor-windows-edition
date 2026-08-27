#include "ForgeConductor/Infrastructure/Windows/WindowsManagerNamedPipeClient.h"

#include "ForgeConductor/Manager/ManagerDeadlineMapper.h"
#include "ForgeConductor/Manager/ManagerProtocolCodec.h"
#include "Detail/ManagerPipeIo.h"
#include "Detail/UniqueHandle.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

namespace ForgeConductor::Infrastructure::Windows {
namespace {

using namespace std::chrono_literals;

constexpr std::wstring_view LocalPipePrefix = L"\\\\.\\pipe\\";
constexpr std::size_t HardMaximumClientRequests = 16U;
constexpr auto HardMaximumRequestLifetime = 5min;
constexpr auto HardMaximumConnectTimeout = 2s;
constexpr auto HardMaximumShutdownDrainTimeout = 5s;

[[nodiscard]] Domain::MonotonicTimePoint boundedDeadline(
    const Domain::MonotonicTimePoint now,
    const std::chrono::milliseconds requestedTimeout) noexcept
{
    const auto timeout = (std::min)(
        (std::max)(requestedTimeout, std::chrono::milliseconds::zero()),
        std::chrono::duration_cast<std::chrono::milliseconds>(
            HardMaximumConnectTimeout));
    const auto increment =
        std::chrono::duration_cast<Domain::MonotonicTimePoint::duration>(
            timeout);
    const auto maximum = Domain::MonotonicTimePoint::max();
    if (now >= maximum - increment) {
        return maximum;
    }
    return now + increment;
}

[[nodiscard]] Domain::Error clientError(
    const std::string_view code,
    std::string message,
    const bool retryable = false)
{
    return Domain::makeError(code, std::move(message), retryable);
}

template <typename T>
[[nodiscard]] Domain::Result<T> failure(Domain::Error error)
{
    return Domain::Result<T>::failure(std::move(error));
}

[[nodiscard]] Domain::Result<void> validatePipeName(
    const std::wstring_view pipeName) noexcept
{
    try {
        if (!pipeName.starts_with(LocalPipePrefix) ||
            pipeName.size() <= LocalPipePrefix.size() ||
            pipeName.size() > Detail::MaximumManagerPipeNameCharacters ||
            pipeName.find(L'\0') != std::wstring_view::npos) {
            return Domain::Result<void>::failure(clientError(
                Domain::ErrorCodes::InvalidRequest,
                "The manager client requires one bounded local named-pipe path."));
        }
        return Domain::Result<void>::success();
    } catch (...) {
        return Domain::Result<void>::failure(clientError(
            Domain::ErrorCodes::InternalFailure,
            "The manager client pipe name could not be validated."));
    }
}

[[nodiscard]] Domain::Result<void> validateLimits(
    const Manager::ManagerTransportLimits& limits) noexcept
{
    if (limits.maximumFrameBytes == 0U ||
        limits.maximumFrameBytes >
            Detail::DefaultManagerPipeMaximumPayloadBytes ||
        limits.maximumRequestLifetime <= 0ms ||
        limits.maximumRequestLifetime > HardMaximumRequestLifetime ||
        limits.connectTimeout <= 0ms ||
        limits.connectTimeout > HardMaximumConnectTimeout ||
        limits.shutdownDrainTimeout <= 0ms ||
        limits.shutdownDrainTimeout > HardMaximumShutdownDrainTimeout ||
        limits.maximumConcurrentClientRequests == 0U ||
        limits.maximumConcurrentClientRequests > HardMaximumClientRequests ||
        limits.maximumActiveRegularOperations == 0U) {
        return Domain::Result<void>::failure(clientError(
            Domain::ErrorCodes::InvalidRequest,
            "The manager client transport limits are outside their bounded policy."));
    }
    return Domain::Result<void>::success();
}

[[nodiscard]] bool isLocalCancellationFailure(
    const Domain::Error& error) noexcept
{
    return error.code == Domain::ErrorCodes::Cancelled ||
        error.code == Domain::ErrorCodes::DeadlineExceeded;
}

[[nodiscard]] Domain::Error wrongResponseTypeError()
{
    return clientError(
        Domain::ErrorCodes::MalformedMessage,
        "The manager response carried a result type that does not match the request.");
}

} // namespace

class WindowsManagerNamedPipeClient::Impl final {
public:
    Impl(
        std::shared_ptr<Contracts::IClock> clock,
        std::wstring pipeName,
        Domain::Sha256Digest nonce,
        Manager::ManagerTransportLimits limits,
        Detail::UniqueHandle shutdownEvent) noexcept
        : clock_{std::move(clock)},
          pipeName_{std::move(pipeName)},
          nonce_{std::move(nonce)},
          limits_{limits},
          shutdownEvent_{std::move(shutdownEvent)}
    {
    }

    ~Impl() noexcept { shutdown(); }

    Impl(const Impl&) = delete;
    Impl& operator=(const Impl&) = delete;
    Impl(Impl&&) = delete;
    Impl& operator=(Impl&&) = delete;

    [[nodiscard]] Domain::Result<void> start() noexcept
    {
        try {
            cancellationWorker_ = std::jthread{
                [this](std::stop_token) noexcept { cancellationWorkerMain(); }};
            return Domain::Result<void>::success();
        } catch (...) {
            return Domain::Result<void>::failure(clientError(
                Domain::ErrorCodes::InternalFailure,
                "The manager client cancellation owner could not start."));
        }
    }

    [[nodiscard]] Domain::Result<Domain::ManagerStatus> status(
        const Domain::OperationContext& context) noexcept
    {
        auto result = exchange(Manager::ManagerStatusRequest{}, context);
        if (!result) {
            return failure<Domain::ManagerStatus>(std::move(result).error());
        }
        auto* status = std::get_if<Domain::ManagerStatus>(&result.value());
        if (status == nullptr) {
            return failure<Domain::ManagerStatus>(wrongResponseTypeError());
        }
        return Domain::Result<Domain::ManagerStatus>::success(std::move(*status));
    }

    [[nodiscard]] Domain::Result<Domain::ManagerSettings> settings(
        const Domain::OperationContext& context) noexcept
    {
        auto result = exchange(Manager::ManagerSettingsRequest{}, context);
        if (!result) {
            return failure<Domain::ManagerSettings>(std::move(result).error());
        }
        auto* settings = std::get_if<Domain::ManagerSettings>(&result.value());
        if (settings == nullptr) {
            return failure<Domain::ManagerSettings>(wrongResponseTypeError());
        }
        return Domain::Result<Domain::ManagerSettings>::success(
            std::move(*settings));
    }

    [[nodiscard]] Domain::Result<Domain::ManagerStatus> control(
        const Domain::ManagerControlRequest& request,
        const Domain::OperationContext& context) noexcept
    {
        auto result = exchange(request, context);
        if (!result) {
            return failure<Domain::ManagerStatus>(std::move(result).error());
        }
        auto* status = std::get_if<Domain::ManagerStatus>(&result.value());
        if (status == nullptr) {
            return failure<Domain::ManagerStatus>(wrongResponseTypeError());
        }
        return Domain::Result<Domain::ManagerStatus>::success(std::move(*status));
    }

    [[nodiscard]] Domain::Result<Domain::ManagerSettingsUpdateOutcome>
    updateSettings(
        const Domain::ManagerSettingsPatch& patch,
        const bool applyImmediately,
        const Domain::OperationContext& context) noexcept
    {
        auto result = exchange(
            Manager::ManagerSettingsUpdateRequest{patch, applyImmediately},
            context);
        if (!result) {
            return failure<Domain::ManagerSettingsUpdateOutcome>(
                std::move(result).error());
        }
        auto* outcome =
            std::get_if<Domain::ManagerSettingsUpdateOutcome>(&result.value());
        if (outcome == nullptr) {
            return failure<Domain::ManagerSettingsUpdateOutcome>(
                wrongResponseTypeError());
        }
        return Domain::Result<Domain::ManagerSettingsUpdateOutcome>::success(
            std::move(*outcome));
    }

    [[nodiscard]] Domain::Result<void> requestShutdown(
        const Domain::OperationContext& context) noexcept
    {
        auto result = exchange(Manager::ManagerShutdownRequest{}, context);
        if (!result) {
            return Domain::Result<void>::failure(std::move(result).error());
        }
        const auto* acknowledgement =
            std::get_if<Manager::ManagerAcknowledgement>(&result.value());
        if (acknowledgement == nullptr || !acknowledgement->acknowledged) {
            return Domain::Result<void>::failure(wrongResponseTypeError());
        }
        return Domain::Result<void>::success();
    }

    void shutdown() noexcept
    {
        try {
            const std::lock_guard shutdownLock{shutdownMutex_};
            {
                std::lock_guard lock{stateMutex_};
                shutdown_ = true;
            }
            if (shutdownEvent_) {
                static_cast<void>(::SetEvent(shutdownEvent_.get()));
            }
            {
                std::lock_guard lock{cancellationMutex_};
                cancellationStopping_ = true;
                pendingCancellations_.clear();
            }
            cancellationChanged_.notify_all();
            if (cancellationWorker_.joinable()) {
                cancellationWorker_.join();
            }
            std::unique_lock lock{stateMutex_};
            static_cast<void>(stateChanged_.wait_for(
                lock,
                limits_.shutdownDrainTimeout,
                [&] { return activeRequests_ == 0U; }));
        } catch (...) {
        }
    }

private:
    class ActiveRequest final {
    public:
        ActiveRequest() noexcept = default;
        explicit ActiveRequest(Impl* owner) noexcept : owner_{owner} {}
        ~ActiveRequest() noexcept { reset(); }

        ActiveRequest(const ActiveRequest&) = delete;
        ActiveRequest& operator=(const ActiveRequest&) = delete;

        ActiveRequest(ActiveRequest&& other) noexcept
            : owner_{std::exchange(other.owner_, nullptr)}
        {
        }

        ActiveRequest& operator=(ActiveRequest&& other) noexcept
        {
            if (this != &other) {
                reset();
                owner_ = std::exchange(other.owner_, nullptr);
            }
            return *this;
        }

    private:
        void reset() noexcept
        {
            if (owner_ != nullptr) {
                owner_->releaseRequest();
                owner_ = nullptr;
            }
        }

        Impl* owner_{};
    };

    struct Admission final {
        ActiveRequest activeRequest;
        std::int64_t wireDeadlineUtcMilliseconds{};
    };

    struct PendingCancellation final {
        Domain::OperationId operationId;
        Domain::CorrelationId correlationId;
    };

    [[nodiscard]] Domain::Result<Admission> admit(
        const Domain::OperationContext& context) noexcept
    {
        try {
            auto wireDeadline =
                Manager::toManagerWireDeadline(context, *clock_, limits_);
            if (!wireDeadline) {
                return failure<Admission>(std::move(wireDeadline).error());
            }

            std::lock_guard lock{stateMutex_};
            if (shutdown_) {
                return failure<Admission>(clientError(
                    Domain::ErrorCodes::TransportClosed,
                    "The manager client transport is shut down."));
            }
            if (activeRequests_ >= limits_.maximumConcurrentClientRequests) {
                return failure<Admission>(clientError(
                    Domain::ErrorCodes::LimitExceeded,
                    "The manager client has reached its concurrent request limit.",
                    true));
            }
            ++activeRequests_;
            return Domain::Result<Admission>::success(Admission{
                ActiveRequest{this}, wireDeadline.value()});
        } catch (...) {
            return failure<Admission>(clientError(
                Domain::ErrorCodes::InternalFailure,
                "The manager client could not admit the bounded request."));
        }
    }

    void releaseRequest() noexcept
    {
        try {
            {
                std::lock_guard lock{stateMutex_};
                if (activeRequests_ > 0U) {
                    --activeRequests_;
                }
            }
            stateChanged_.notify_all();
        } catch (...) {
        }
    }

    [[nodiscard]] Domain::OperationContext connectionContext(
        const Domain::OperationContext& context) const noexcept
    {
        const auto now = clock_->monotonicNow();
        const auto connectionDeadline =
            boundedDeadline(now, limits_.connectTimeout);
        return Domain::OperationContext{
            context.operationId,
            (std::min)(context.deadline, connectionDeadline),
            context.cancellation,
            context.correlationId};
    }

    [[nodiscard]] Domain::Result<Manager::ManagerResult> exchange(
        Manager::ManagerRequestPayload payload,
        const Domain::OperationContext& context) noexcept
    {
        try {
            auto admission = admit(context);
            if (!admission) {
                return failure<Manager::ManagerResult>(
                    std::move(admission).error());
            }

            auto requestId = Domain::RequestId::parse(context.operationId.value());
            if (!requestId) {
                return failure<Manager::ManagerResult>(std::move(requestId).error());
            }
            const Domain::RequestId expectedRequestId = requestId.value();
            const Domain::CorrelationId expectedCorrelationId = context.correlationId;
            const Manager::ManagerRequest request{
                Manager::ManagerProtocolVersion,
                std::move(requestId).value(),
                expectedCorrelationId,
                admission.value().wireDeadlineUtcMilliseconds,
                nonce_,
                std::move(payload)};
            auto encoded = Manager::ManagerProtocolCodec::encodeRequest(
                request, limits_.maximumFrameBytes);
            if (!encoded) {
                return failure<Manager::ManagerResult>(std::move(encoded).error());
            }

            const auto connectContext = connectionContext(context);
            auto opened = Detail::openManagerPipe(
                pipeName_, GENERIC_READ | GENERIC_WRITE, connectContext,
                shutdownEvent_.get());
            if (!opened) {
                return failure<Manager::ManagerResult>(std::move(opened).error());
            }
            Detail::UniqueHandle pipe{std::move(opened).value()};

            auto written = Detail::writeManagerPipeFrame(
                pipe.get(), encoded.value(), context, shutdownEvent_.get(),
                limits_.maximumFrameBytes);
            if (!written) {
                auto error = std::move(written).error();
                pipe.reset();
                // CancelIoEx can win the local completion race after the peer
                // has already received a complete request. An idempotent
                // control-lane cancellation therefore follows either a
                // cancelled write or read.
                if (isLocalCancellationFailure(error)) {
                    enqueueBestEffortCancel(context);
                }
                return failure<Manager::ManagerResult>(std::move(error));
            }

            auto received = Detail::readManagerPipeFrame(
                pipe.get(), context, shutdownEvent_.get(),
                limits_.maximumFrameBytes);
            if (!received) {
                auto error = std::move(received).error();
                pipe.reset();
                if (isLocalCancellationFailure(error)) {
                    enqueueBestEffortCancel(context);
                }
                return failure<Manager::ManagerResult>(std::move(error));
            }
            auto receipt = Detail::writeManagerPipeResponseReceipt(
                pipe.get(), context, shutdownEvent_.get());
            if (!receipt) {
                return failure<Manager::ManagerResult>(
                    std::move(receipt).error());
            }
            pipe.reset();

            auto decoded = Manager::ManagerProtocolCodec::decodeResponse(
                received.value(), limits_.maximumFrameBytes);
            if (!decoded) {
                return failure<Manager::ManagerResult>(std::move(decoded).error());
            }
            auto response = std::move(decoded).value();
            if (response.version != Manager::ManagerProtocolVersion ||
                response.requestId != expectedRequestId ||
                response.correlationId != expectedCorrelationId) {
                return failure<Manager::ManagerResult>(clientError(
                    Domain::ErrorCodes::MalformedMessage,
                    "The manager response envelope does not match the request."));
            }
            if (auto* error = std::get_if<Domain::Error>(&response.body)) {
                return failure<Manager::ManagerResult>(std::move(*error));
            }
            auto* result = std::get_if<Manager::ManagerResult>(&response.body);
            if (result == nullptr) {
                return failure<Manager::ManagerResult>(clientError(
                    Domain::ErrorCodes::MalformedMessage,
                    "The manager response did not contain one result or typed error."));
            }
            return Domain::Result<Manager::ManagerResult>::success(
                std::move(*result));
        } catch (...) {
            return failure<Manager::ManagerResult>(clientError(
                Domain::ErrorCodes::InternalFailure,
                "The manager client request failed unexpectedly."));
        }
    }

    void enqueueBestEffortCancel(
        const Domain::OperationContext& original) noexcept
    {
        try {
            {
                std::lock_guard lock{stateMutex_};
                if (shutdown_) {
                    return;
                }
            }

            {
                std::lock_guard lock{cancellationMutex_};
                if (cancellationStopping_ ||
                    pendingCancellations_.size() >=
                        limits_.maximumConcurrentClientRequests) {
                    return;
                }
                pendingCancellations_.push_back(PendingCancellation{
                    original.operationId, original.correlationId});
            }
            cancellationChanged_.notify_one();
        } catch (...) {
        }
    }

    void cancellationWorkerMain() noexcept
    {
        try {
            for (;;) {
                std::optional<PendingCancellation> pending;
                {
                    std::unique_lock lock{cancellationMutex_};
                    cancellationChanged_.wait(lock, [&] {
                        return cancellationStopping_ ||
                            !pendingCancellations_.empty();
                    });
                    if (cancellationStopping_) {
                        return;
                    }
                    pending.emplace(
                        std::move(pendingCancellations_.front()));
                    pendingCancellations_.pop_front();
                }
                performBestEffortCancel(*pending);
            }
        } catch (...) {
        }
    }

    void performBestEffortCancel(
        const PendingCancellation& original) noexcept
    {
        try {

            const auto now = clock_->monotonicNow();
            const Domain::OperationContext cancelContext{
                original.operationId,
                boundedDeadline(now, limits_.connectTimeout),
                std::stop_token{},
                original.correlationId};
            auto wireDeadline = Manager::toManagerWireDeadline(
                cancelContext, *clock_, limits_);
            auto requestId = Domain::RequestId::parse(
                original.operationId.value());
            if (!wireDeadline || !requestId) {
                return;
            }
            const Manager::ManagerRequest request{
                Manager::ManagerProtocolVersion,
                std::move(requestId).value(),
                original.correlationId,
                wireDeadline.value(),
                nonce_,
                Manager::ManagerCancelRequest{original.operationId}};
            auto encoded = Manager::ManagerProtocolCodec::encodeRequest(
                request, limits_.maximumFrameBytes);
            if (!encoded) {
                return;
            }
            auto opened = Detail::openManagerPipe(
                pipeName_, GENERIC_READ | GENERIC_WRITE, cancelContext,
                shutdownEvent_.get());
            if (!opened) {
                return;
            }
            Detail::UniqueHandle pipe{std::move(opened).value()};
            auto written = Detail::writeManagerPipeFrame(
                pipe.get(), encoded.value(), cancelContext,
                shutdownEvent_.get(), limits_.maximumFrameBytes);
            if (!written) {
                return;
            }
            auto received = Detail::readManagerPipeFrame(
                pipe.get(), cancelContext, shutdownEvent_.get(),
                limits_.maximumFrameBytes);
            if (!received) {
                return;
            }
            auto receipt = Detail::writeManagerPipeResponseReceipt(
                pipe.get(), cancelContext, shutdownEvent_.get());
            if (!receipt) {
                return;
            }
            auto decoded = Manager::ManagerProtocolCodec::decodeResponse(
                received.value(), limits_.maximumFrameBytes);
            if (!decoded) {
                return;
            }
            const auto& response = decoded.value();
            if (response.version != Manager::ManagerProtocolVersion ||
                response.requestId.value() != original.operationId.value() ||
                response.correlationId != original.correlationId) {
                return;
            }
            const auto* body =
                std::get_if<Manager::ManagerResult>(&response.body);
            if (body == nullptr) {
                return;
            }
            const auto* acknowledgement =
                std::get_if<Manager::ManagerAcknowledgement>(body);
            static_cast<void>(acknowledgement != nullptr &&
                              acknowledgement->acknowledged);
        } catch (...) {
        }
    }

    std::shared_ptr<Contracts::IClock> clock_;
    std::wstring pipeName_;
    Domain::Sha256Digest nonce_;
    Manager::ManagerTransportLimits limits_;
    Detail::UniqueHandle shutdownEvent_;
    std::mutex stateMutex_;
    std::condition_variable stateChanged_;
    std::size_t activeRequests_{};
    bool shutdown_{};
    std::mutex shutdownMutex_;
    std::mutex cancellationMutex_;
    std::condition_variable cancellationChanged_;
    std::deque<PendingCancellation> pendingCancellations_;
    bool cancellationStopping_{};
    std::jthread cancellationWorker_;
};

WindowsManagerNamedPipeClient::WindowsManagerNamedPipeClient(
    std::shared_ptr<Impl> implementation) noexcept
    : implementation_{std::move(implementation)}
{
}

WindowsManagerNamedPipeClient::~WindowsManagerNamedPipeClient() noexcept
{
    const auto implementation = implementation_;
    if (implementation) {
        implementation->shutdown();
    }
}

Domain::Result<std::unique_ptr<WindowsManagerNamedPipeClient>>
WindowsManagerNamedPipeClient::create(
    std::shared_ptr<Contracts::IClock> clock,
    std::wstring pipeName,
    Domain::Sha256Digest nonce,
    Manager::ManagerTransportLimits limits) noexcept
{
    try {
        if (!clock) {
            return failure<std::unique_ptr<WindowsManagerNamedPipeClient>>(
                clientError(
                    Domain::ErrorCodes::InvalidRequest,
                    "The manager client requires an owned clock."));
        }
        auto validName = validatePipeName(pipeName);
        if (!validName) {
            return failure<std::unique_ptr<WindowsManagerNamedPipeClient>>(
                std::move(validName).error());
        }
        auto validLimits = validateLimits(limits);
        if (!validLimits) {
            return failure<std::unique_ptr<WindowsManagerNamedPipeClient>>(
                std::move(validLimits).error());
        }

        Detail::UniqueHandle shutdownEvent{
            ::CreateEventW(nullptr, TRUE, FALSE, nullptr)};
        if (!shutdownEvent) {
            return failure<std::unique_ptr<WindowsManagerNamedPipeClient>>(
                clientError(
                    Domain::ErrorCodes::InternalFailure,
                    "The manager client shutdown event could not be created."));
        }
        auto implementation = std::make_shared<Impl>(
            std::move(clock), std::move(pipeName), std::move(nonce), limits,
            std::move(shutdownEvent));
        auto started = implementation->start();
        if (!started) {
            return failure<std::unique_ptr<WindowsManagerNamedPipeClient>>(
                std::move(started).error());
        }
        return Domain::Result<std::unique_ptr<WindowsManagerNamedPipeClient>>::success(
            std::unique_ptr<WindowsManagerNamedPipeClient>{
                new WindowsManagerNamedPipeClient{std::move(implementation)}});
    } catch (...) {
        return failure<std::unique_ptr<WindowsManagerNamedPipeClient>>(
            clientError(
                Domain::ErrorCodes::InternalFailure,
                "The bounded manager client could not be created."));
    }
}

Domain::Result<Domain::ManagerStatus>
WindowsManagerNamedPipeClient::status(
    const Domain::OperationContext& context) noexcept
{
    try {
        const auto implementation = implementation_;
        if (!implementation) {
            return failure<Domain::ManagerStatus>(clientError(
                Domain::ErrorCodes::TransportClosed,
                "The manager client transport is unavailable."));
        }
        return implementation->status(context);
    } catch (...) {
        return failure<Domain::ManagerStatus>(clientError(
            Domain::ErrorCodes::InternalFailure,
            "The manager status request failed unexpectedly."));
    }
}

Domain::Result<Domain::ManagerSettings>
WindowsManagerNamedPipeClient::settings(
    const Domain::OperationContext& context) noexcept
{
    try {
        const auto implementation = implementation_;
        if (!implementation) {
            return failure<Domain::ManagerSettings>(clientError(
                Domain::ErrorCodes::TransportClosed,
                "The manager client transport is unavailable."));
        }
        return implementation->settings(context);
    } catch (...) {
        return failure<Domain::ManagerSettings>(clientError(
            Domain::ErrorCodes::InternalFailure,
            "The manager settings request failed unexpectedly."));
    }
}

Domain::Result<Domain::ManagerStatus>
WindowsManagerNamedPipeClient::control(
    const Domain::ManagerControlRequest& request,
    const Domain::OperationContext& context) noexcept
{
    try {
        const auto implementation = implementation_;
        if (!implementation) {
            return failure<Domain::ManagerStatus>(clientError(
                Domain::ErrorCodes::TransportClosed,
                "The manager client transport is unavailable."));
        }
        return implementation->control(request, context);
    } catch (...) {
        return failure<Domain::ManagerStatus>(clientError(
            Domain::ErrorCodes::InternalFailure,
            "The manager control request failed unexpectedly."));
    }
}

Domain::Result<Domain::ManagerSettingsUpdateOutcome>
WindowsManagerNamedPipeClient::updateSettings(
    const Domain::ManagerSettingsPatch& patch,
    const bool applyImmediately,
    const Domain::OperationContext& context) noexcept
{
    try {
        const auto implementation = implementation_;
        if (!implementation) {
            return failure<Domain::ManagerSettingsUpdateOutcome>(clientError(
                Domain::ErrorCodes::TransportClosed,
                "The manager client transport is unavailable."));
        }
        return implementation->updateSettings(
            patch, applyImmediately, context);
    } catch (...) {
        return failure<Domain::ManagerSettingsUpdateOutcome>(clientError(
            Domain::ErrorCodes::InternalFailure,
            "The manager settings update failed unexpectedly."));
    }
}

Domain::Result<void> WindowsManagerNamedPipeClient::requestShutdown(
    const Domain::OperationContext& context) noexcept
{
    try {
        const auto implementation = implementation_;
        if (!implementation) {
            return Domain::Result<void>::failure(clientError(
                Domain::ErrorCodes::TransportClosed,
                "The manager client transport is unavailable."));
        }
        return implementation->requestShutdown(context);
    } catch (...) {
        return Domain::Result<void>::failure(clientError(
            Domain::ErrorCodes::InternalFailure,
            "The remote manager shutdown request failed unexpectedly."));
    }
}

void WindowsManagerNamedPipeClient::shutdown() noexcept
{
    const auto implementation = implementation_;
    if (implementation) {
        implementation->shutdown();
    }
}

} // namespace ForgeConductor::Infrastructure::Windows
