#include "ForgeConductor/Infrastructure/Windows/WindowsManagerNamedPipeServer.h"

#include "Detail/CurrentUserPipeSecurity.h"
#include "Detail/ManagerPipeIo.h"
#include "Detail/UniqueHandle.h"
#include "Detail/Win32Error.h"
#include "ForgeConductor/Contracts/IManagerAuthentication.h"
#include "ForgeConductor/Manager/ManagerDeadlineMapper.h"
#include "ForgeConductor/Manager/ManagerProtocolCodec.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
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
constexpr std::size_t MinimumWorkerCount = 2U;
constexpr std::size_t MaximumWorkerCount = 16U;
constexpr std::size_t HardMaximumClientRequests = 16U;
constexpr auto HardMaximumRequestLifetime = 5min;
constexpr auto HardMaximumConnectTimeout = 2s;
constexpr auto HardMaximumShutdownDrainTimeout = 5s;
constexpr DWORD RunWaitMilliseconds = 25U;

[[nodiscard]] Domain::Error serverError(
    const std::string_view code,
    std::string message,
    const bool retryable = false)
{
    return Domain::makeError(code, std::move(message), retryable);
}

[[nodiscard]] Domain::Result<void> validateOptions(
    const WindowsManagerNamedPipeServerOptions& options) noexcept
{
    try {
        if (!options.pipeName.starts_with(LocalPipePrefix) ||
            options.pipeName.size() <= LocalPipePrefix.size() ||
            options.pipeName.size() >
                Detail::MaximumManagerPipeNameCharacters ||
            options.pipeName.find(L'\0') != std::wstring::npos) {
            return Domain::Result<void>::failure(serverError(
                Domain::ErrorCodes::InvalidRequest,
                "The manager server requires a bounded local named-pipe path."));
        }
        if (options.workerCount < MinimumWorkerCount ||
            options.workerCount > MaximumWorkerCount ||
            options.limits.maximumActiveRegularOperations == 0U ||
            options.limits.maximumActiveRegularOperations >= options.workerCount) {
            return Domain::Result<void>::failure(serverError(
                Domain::ErrorCodes::InvalidRequest,
                "The manager server worker bound must reserve one control worker."));
        }
        if (options.limits.maximumFrameBytes == 0U ||
            options.limits.maximumFrameBytes >
                Manager::ManagerProtocolCodec::DefaultMaximumFrameBytes ||
            options.limits.maximumFrameBytes >
                static_cast<std::size_t>((std::numeric_limits<DWORD>::max)() - 4U)) {
            return Domain::Result<void>::failure(serverError(
                Domain::ErrorCodes::InvalidRequest,
                "The manager server frame bound is invalid."));
        }
        if (options.limits.maximumRequestLifetime <= 0ms ||
            options.limits.maximumRequestLifetime >
                HardMaximumRequestLifetime ||
            options.limits.connectTimeout <= 0ms ||
            options.limits.connectTimeout > HardMaximumConnectTimeout ||
            options.limits.shutdownDrainTimeout <= 0ms ||
            options.limits.shutdownDrainTimeout >
                HardMaximumShutdownDrainTimeout ||
            options.limits.maximumConcurrentClientRequests == 0U ||
            options.limits.maximumConcurrentClientRequests >
                HardMaximumClientRequests ||
            options.ingressTimeout <= std::chrono::milliseconds::zero() ||
            options.ingressTimeout > options.limits.maximumRequestLifetime) {
            return Domain::Result<void>::failure(serverError(
                Domain::ErrorCodes::InvalidRequest,
                "The manager server transport deadlines are outside their bounded policy."));
        }
        return Domain::Result<void>::success();
    } catch (...) {
        return Domain::Result<void>::failure(serverError(
            Domain::ErrorCodes::InternalFailure,
            "The manager server options could not be validated."));
    }
}

[[nodiscard]] bool isShutdownAcknowledgement(
    const Manager::ManagerRequest& request,
    const Manager::ManagerResponse& response) noexcept
{
    if (!std::holds_alternative<Manager::ManagerShutdownRequest>(request.payload)) {
        return false;
    }
    const auto* result = std::get_if<Manager::ManagerResult>(&response.body);
    if (result == nullptr) {
        return false;
    }
    const auto* acknowledgement =
        std::get_if<Manager::ManagerAcknowledgement>(result);
    return acknowledgement != nullptr && acknowledgement->acknowledged;
}

} // namespace

class WindowsManagerNamedPipeServer::Impl final
    : public std::enable_shared_from_this<WindowsManagerNamedPipeServer::Impl> {
public:
    Impl(
        std::shared_ptr<Contracts::IClock> clock,
        std::shared_ptr<Manager::ManagerRequestDispatcher> dispatcher,
        WindowsCurrentUserIdentity ownerIdentity,
        Domain::Sha256Digest nonce,
        WindowsManagerNamedPipeServerOptions options,
        std::unique_ptr<Detail::CurrentUserPipeSecurity> security,
        Detail::UniqueHandle shutdownEvent) noexcept
        : clock_{std::move(clock)},
          dispatcher_{std::move(dispatcher)},
          ownerIdentity_{std::move(ownerIdentity)},
          nonce_{std::move(nonce)},
          options_{std::move(options)},
          security_{std::move(security)},
          shutdownEvent_{std::move(shutdownEvent)}
    {
    }

    ~Impl() noexcept
    {
        shutdown();
        // run() holds this mutex for its complete lifetime. Acquiring it here
        // prevents the implementation from being destroyed under that call.
        const std::lock_guard runLock{runLifetimeMutex_};
    }

    Impl(const Impl&) = delete;
    Impl& operator=(const Impl&) = delete;
    Impl(Impl&&) = delete;
    Impl& operator=(Impl&&) = delete;

    [[nodiscard]] Domain::Result<void> run(
        const Domain::OperationContext& context) noexcept
    {
        const std::lock_guard runLock{runLifetimeMutex_};
        try {
            if (context.isCancellationRequested()) {
                return Domain::Result<void>::failure(serverError(
                    Domain::ErrorCodes::Cancelled,
                    "Manager named-pipe startup was cancelled."));
            }
            if (context.isExpired(clock_->monotonicNow())) {
                return Domain::Result<void>::failure(serverError(
                    Domain::ErrorCodes::DeadlineExceeded,
                    "Manager named-pipe startup exceeded its deadline."));
            }

            {
                const std::lock_guard stateLock{stateMutex_};
                if (started_) {
                    return Domain::Result<void>::failure(serverError(
                        Domain::ErrorCodes::Conflict,
                        "Manager named-pipe ingress may run only once."));
                }
                if (stopping_) {
                    return Domain::Result<void>::failure(serverError(
                        Domain::ErrorCodes::TransportClosed,
                        "Manager named-pipe ingress is closed."));
                }
                started_ = true;
            }

            auto firstPipe = createPipeInstance(true);
            if (!firstPipe) {
                markStopped();
                return Domain::Result<void>::failure(std::move(firstPipe).error());
            }

            const auto owner = shared_from_this();
            std::vector<std::jthread> workers;
            workers.reserve(options_.workerCount);
            WorkerJoinGuard workerJoin{*this, workers};
            const auto launchWorker =
                [this, &workers, &owner, &context](
                    Detail::UniqueHandle pipe) {
                    workerStarted();
                    try {
                        workers.emplace_back(
                            [owner, context, pipe = std::move(pipe)](
                                std::stop_token) mutable noexcept {
                                owner->workerMain(context, std::move(pipe));
                                owner->workerStopped();
                            });
                    } catch (...) {
                        workerStopped();
                        throw;
                    }
                };
            launchWorker(std::move(firstPipe).value());
            for (std::size_t index = 1U; index < options_.workerCount; ++index) {
                launchWorker({});
            }

            std::optional<Domain::Error> contextFailure;
            for (;;) {
                const DWORD waitResult = ::WaitForSingleObject(
                    shutdownEvent_.get(), RunWaitMilliseconds);
                if (waitResult == WAIT_OBJECT_0) {
                    break;
                }
                if (waitResult == WAIT_FAILED) {
                    contextFailure = Detail::makeWin32Error(
                        "wait for manager named-pipe shutdown", ::GetLastError());
                    signalShutdown();
                    break;
                }
                if (context.isCancellationRequested()) {
                    contextFailure = serverError(
                        Domain::ErrorCodes::Cancelled,
                        "Manager named-pipe ingress was cancelled.");
                    signalShutdown();
                    break;
                }
                if (context.isExpired(clock_->monotonicNow())) {
                    contextFailure = serverError(
                        Domain::ErrorCodes::DeadlineExceeded,
                        "Manager named-pipe ingress exceeded its deadline.");
                    signalShutdown();
                    break;
                }
            }

            workerJoin.joinNow();
            markStopped();

            if (auto fatal = fatalError(); fatal.has_value()) {
                return Domain::Result<void>::failure(std::move(fatal).value());
            }
            if (contextFailure.has_value()) {
                return Domain::Result<void>::failure(
                    std::move(contextFailure).value());
            }
            return Domain::Result<void>::success();
        } catch (...) {
            signalShutdown();
            dispatcher_->beginShutdown();
            markStopped();
            return Domain::Result<void>::failure(serverError(
                Domain::ErrorCodes::InternalFailure,
                "Manager named-pipe ingress failed at its process boundary."));
        }
    }

    void cancel(const Domain::OperationId& operationId) noexcept
    {
        dispatcher_->cancel(operationId);
    }

    void shutdown() noexcept
    {
        signalShutdown();
        dispatcher_->beginShutdown();
    }

private:
    class WorkerJoinGuard final {
    public:
        WorkerJoinGuard(
            Impl& owner,
            std::vector<std::jthread>& workers) noexcept
            : owner_{owner}, workers_{workers}
        {
        }

        ~WorkerJoinGuard() noexcept { joinNow(); }

        WorkerJoinGuard(const WorkerJoinGuard&) = delete;
        WorkerJoinGuard& operator=(const WorkerJoinGuard&) = delete;

        void joinNow() noexcept
        {
            if (joined_) {
                return;
            }
            joined_ = true;
            // Cancel application work and native I/O before joining. A
            // non-cooperative controller callback cannot hold run() or
            // destruction indefinitely: after the bounded drain, its worker
            // detaches while retaining shared ownership of this complete
            // implementation until that callback eventually returns.
            owner_.dispatcher_->beginShutdown();
            owner_.signalShutdown();
            if (!owner_.waitForWorkers(
                    owner_.options_.limits.shutdownDrainTimeout)) {
                for (auto& worker : workers_) {
                    if (worker.joinable()) {
                        worker.detach();
                    }
                }
            }
            workers_.clear();
        }

    private:
        Impl& owner_;
        std::vector<std::jthread>& workers_;
        bool joined_{};
    };

    void workerStarted()
    {
        const std::lock_guard lock{workerMutex_};
        ++activeWorkers_;
    }

    void workerStopped() noexcept
    {
        try {
            {
                const std::lock_guard lock{workerMutex_};
                if (activeWorkers_ == 0U) {
                    std::terminate();
                }
                --activeWorkers_;
            }
            workerStateChanged_.notify_all();
        } catch (...) {
            std::terminate();
        }
    }

    [[nodiscard]] bool waitForWorkers(
        const std::chrono::milliseconds timeout) noexcept
    {
        try {
            std::unique_lock lock{workerMutex_};
            return workerStateChanged_.wait_for(
                lock, timeout, [this] { return activeWorkers_ == 0U; });
        } catch (...) {
            return false;
        }
    }

    [[nodiscard]] Domain::Result<Detail::UniqueHandle> createPipeInstance(
        const bool firstInstance) noexcept
    {
        DWORD openMode = PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED;
        if (firstInstance) {
            openMode |= FILE_FLAG_FIRST_PIPE_INSTANCE;
        }
        constexpr DWORD PipeMode =
            PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT |
            PIPE_REJECT_REMOTE_CLIENTS;
        const DWORD bufferBytes = static_cast<DWORD>(
            options_.limits.maximumFrameBytes + 4U);

        Detail::UniqueHandle pipe{::CreateNamedPipeW(
            options_.pipeName.c_str(),
            openMode,
            PipeMode,
            static_cast<DWORD>(options_.workerCount),
            bufferBytes,
            bufferBytes,
            0U,
            security_->attributes())};
        if (!pipe) {
            return Domain::Result<Detail::UniqueHandle>::failure(
                Detail::makeWin32Error(
                    "create the current-user manager named pipe",
                    ::GetLastError(),
                    Domain::ErrorCodes::TransportClosed,
                    true));
        }
        return Domain::Result<Detail::UniqueHandle>::success(std::move(pipe));
    }

    [[nodiscard]] Domain::OperationContext ingressContext(
        const Domain::OperationContext& runContext) const
    {
        const auto now = clock_->monotonicNow();
        const auto maximumOffset =
            std::chrono::duration_cast<Domain::MonotonicTimePoint::duration>(
                options_.ingressTimeout);
        const auto boundedDeadline =
            maximumOffset <= Domain::MonotonicTimePoint::max() - now
            ? now + maximumOffset
            : Domain::MonotonicTimePoint::max();
        return Domain::OperationContext{
            runContext.operationId,
            (std::min)(runContext.deadline, boundedDeadline),
            runContext.cancellation,
            runContext.correlationId};
    }

    [[nodiscard]] Domain::OperationContext responseContext(
        const Manager::ManagerRequest& request,
        const Domain::OperationContext& fallback,
        const Domain::MonotonicTimePoint deadline) const
    {
        auto operationId = Domain::OperationId::parse(request.requestId.value());
        return Domain::OperationContext{
            operationId ? std::move(operationId).value() : fallback.operationId,
            deadline,
            fallback.cancellation,
            request.correlationId};
    }

    [[nodiscard]] Domain::OperationContext deliveryContext(
        const Domain::OperationContext& requestContext) const
    {
        const auto now = clock_->monotonicNow();
        const auto maximumOffset =
            std::chrono::duration_cast<Domain::MonotonicTimePoint::duration>(
                options_.limits.shutdownDrainTimeout);
        const auto maximum = Domain::MonotonicTimePoint::max();
        const auto boundedDeadline = now >= maximum - maximumOffset
            ? maximum
            : now + maximumOffset;
        return Domain::OperationContext{
            requestContext.operationId,
            (std::min)(requestContext.deadline, boundedDeadline),
            requestContext.cancellation,
            requestContext.correlationId};
    }

    [[nodiscard]] bool writeResponseAndAwaitReceipt(
        HANDLE pipe,
        const Manager::ManagerResponse& response,
        const Domain::OperationContext& context) noexcept
    {
        auto encoded = Manager::ManagerProtocolCodec::encodeResponse(
            response, options_.limits.maximumFrameBytes);
        if (!encoded) {
            return false;
        }
        const auto delivery = deliveryContext(context);
        auto written = Detail::writeManagerPipeFrame(
            pipe,
            encoded.value(),
            delivery,
            shutdownEvent_.get(),
            options_.limits.maximumFrameBytes);
        if (!written) {
            return false;
        }
        return static_cast<bool>(Detail::readManagerPipeResponseReceipt(
            pipe, delivery, shutdownEvent_.get()));
    }

    void processConnection(
        HANDLE pipe,
        const Domain::OperationContext& runContext) noexcept
    {
        auto verified = Detail::verifyNamedPipeClientSid(
            pipe, ownerIdentity_.sidBytes());
        if (!verified) {
            return;
        }

        const auto ingress = ingressContext(runContext);
        auto frame = Detail::readManagerPipeFrame(
            pipe,
            ingress,
            shutdownEvent_.get(),
            options_.limits.maximumFrameBytes);
        if (!frame) {
            return;
        }
        auto decoded = Manager::ManagerProtocolCodec::decodeRequest(
            frame.value(), options_.limits.maximumFrameBytes);
        if (!decoded) {
            return;
        }
        Manager::ManagerRequest request = std::move(decoded).value();

        if (!Contracts::constantTimeManagerAuthenticationTokenEquals(
                request.nonce, nonce_)) {
            Manager::ManagerResponse response{
                Manager::ManagerProtocolVersion,
                request.requestId,
                request.correlationId,
                serverError(
                    Domain::ErrorCodes::Unauthorized,
                    "Manager named-pipe authentication failed.")};
            static_cast<void>(writeResponseAndAwaitReceipt(
                pipe, response, ingress));
            return;
        }

        auto mappedDeadline = Manager::fromManagerWireDeadline(
            request.deadlineUtcMilliseconds, *clock_, options_.limits);
        if (!mappedDeadline) {
            Manager::ManagerResponse response{
                Manager::ManagerProtocolVersion,
                request.requestId,
                request.correlationId,
                std::move(mappedDeadline).error()};
            static_cast<void>(writeResponseAndAwaitReceipt(
                pipe, response, ingress));
            return;
        }

        const auto responseOperation = responseContext(
            request, ingress, mappedDeadline.value());
        const Manager::ManagerResponse response = dispatcher_->dispatch(request);
        static_cast<void>(writeResponseAndAwaitReceipt(
            pipe, response, responseOperation));
        if (isShutdownAcknowledgement(request, response)) {
            // The bounded delivery receipt has either confirmed the client
            // read the acknowledgement or timed out before ingress stops.
            signalShutdown();
        }
    }

    void workerMain(
        const Domain::OperationContext& runContext,
        Detail::UniqueHandle initialPipe) noexcept
    {
        try {
            while (!isStopping()) {
                Detail::UniqueHandle pipe = std::move(initialPipe);
                if (!pipe) {
                    auto created = createPipeInstance(false);
                    if (!created) {
                        if (!isStopping()) {
                            recordFatal(std::move(created).error());
                        }
                        return;
                    }
                    pipe = std::move(created).value();
                }

                auto connected = Detail::connectManagerPipe(
                    pipe.get(), runContext, shutdownEvent_.get());
                if (!connected) {
                    if (!isStopping() &&
                        connected.error().code != Domain::ErrorCodes::Cancelled &&
                        connected.error().code !=
                            Domain::ErrorCodes::DeadlineExceeded &&
                        connected.error().code !=
                            Domain::ErrorCodes::TransportClosed) {
                        recordFatal(std::move(connected).error());
                    }
                    continue;
                }

                processConnection(pipe.get(), runContext);
                if (::DisconnectNamedPipe(pipe.get()) == FALSE) {
                    const DWORD error = ::GetLastError();
                    if (error != ERROR_PIPE_NOT_CONNECTED && !isStopping()) {
                        // A client reset is connection-scoped and must not take
                        // down the remaining bounded workers.
                    }
                }
            }
        } catch (...) {
            if (!isStopping()) {
                recordFatal(serverError(
                    Domain::ErrorCodes::InternalFailure,
                    "A manager named-pipe worker failed unexpectedly."));
            }
        }
    }

    [[nodiscard]] bool isStopping() const noexcept
    {
        return ::WaitForSingleObject(shutdownEvent_.get(), 0U) == WAIT_OBJECT_0;
    }

    void signalShutdown() noexcept
    {
        {
            const std::lock_guard stateLock{stateMutex_};
            stopping_ = true;
        }
        if (shutdownEvent_) {
            static_cast<void>(::SetEvent(shutdownEvent_.get()));
        }
    }

    void recordFatal(Domain::Error error) noexcept
    {
        try {
            {
                const std::lock_guard stateLock{stateMutex_};
                if (!fatalError_.has_value()) {
                    fatalError_ = std::move(error);
                }
                stopping_ = true;
            }
            static_cast<void>(::SetEvent(shutdownEvent_.get()));
        } catch (...) {
            std::terminate();
        }
    }

    [[nodiscard]] std::optional<Domain::Error> fatalError() const
    {
        const std::lock_guard stateLock{stateMutex_};
        return fatalError_;
    }

    void markStopped() noexcept
    {
        const std::lock_guard stateLock{stateMutex_};
        stopping_ = true;
        stopped_ = true;
    }

    std::shared_ptr<Contracts::IClock> clock_;
    std::shared_ptr<Manager::ManagerRequestDispatcher> dispatcher_;
    WindowsCurrentUserIdentity ownerIdentity_;
    Domain::Sha256Digest nonce_;
    WindowsManagerNamedPipeServerOptions options_;
    std::unique_ptr<Detail::CurrentUserPipeSecurity> security_;
    Detail::UniqueHandle shutdownEvent_;

    mutable std::mutex stateMutex_;
    std::mutex workerMutex_;
    std::condition_variable workerStateChanged_;
    std::mutex runLifetimeMutex_;
    std::optional<Domain::Error> fatalError_;
    std::size_t activeWorkers_{};
    bool started_{};
    bool stopping_{};
    bool stopped_{};
};

Domain::Result<std::unique_ptr<WindowsManagerNamedPipeServer>>
WindowsManagerNamedPipeServer::create(
    std::shared_ptr<Contracts::IClock> clock,
    std::shared_ptr<Manager::ManagerRequestDispatcher> dispatcher,
    WindowsCurrentUserIdentity ownerIdentity,
    Domain::Sha256Digest nonce,
    WindowsManagerNamedPipeServerOptions options) noexcept
{
    try {
        if (!clock || !dispatcher) {
            return Domain::Result<std::unique_ptr<WindowsManagerNamedPipeServer>>::failure(
                serverError(
                    Domain::ErrorCodes::InvalidRequest,
                    "The manager server requires a clock and dispatcher."));
        }
        auto valid = validateOptions(options);
        if (!valid) {
            return Domain::Result<std::unique_ptr<WindowsManagerNamedPipeServer>>::failure(
                std::move(valid).error());
        }
        auto security = Detail::CurrentUserPipeSecurity::create(ownerIdentity);
        if (!security) {
            return Domain::Result<std::unique_ptr<WindowsManagerNamedPipeServer>>::failure(
                std::move(security).error());
        }
        Detail::UniqueHandle shutdownEvent{
            ::CreateEventW(nullptr, TRUE, FALSE, nullptr)};
        if (!shutdownEvent) {
            return Domain::Result<std::unique_ptr<WindowsManagerNamedPipeServer>>::failure(
                Detail::makeWin32Error(
                    "create the manager named-pipe shutdown event",
                    ::GetLastError()));
        }

        auto implementation = std::make_shared<Impl>(
            std::move(clock),
            std::move(dispatcher),
            std::move(ownerIdentity),
            std::move(nonce),
            std::move(options),
            std::move(security).value(),
            std::move(shutdownEvent));
        return Domain::Result<std::unique_ptr<WindowsManagerNamedPipeServer>>::success(
            std::unique_ptr<WindowsManagerNamedPipeServer>{
                new WindowsManagerNamedPipeServer{std::move(implementation)}});
    } catch (...) {
        return Domain::Result<std::unique_ptr<WindowsManagerNamedPipeServer>>::failure(
            serverError(
                Domain::ErrorCodes::InternalFailure,
                "The manager named-pipe server could not allocate bounded state."));
    }
}

WindowsManagerNamedPipeServer::WindowsManagerNamedPipeServer(
    std::shared_ptr<Impl> implementation) noexcept
    : implementation_{std::move(implementation)}
{
}

WindowsManagerNamedPipeServer::~WindowsManagerNamedPipeServer() noexcept = default;

Domain::Result<void> WindowsManagerNamedPipeServer::run(
    const Domain::OperationContext& context) noexcept
{
    if (!implementation_) {
        return Domain::Result<void>::failure(serverError(
            Domain::ErrorCodes::TransportClosed,
            "Manager named-pipe ingress is closed."));
    }
    return implementation_->run(context);
}

void WindowsManagerNamedPipeServer::cancel(
    const Domain::OperationId& operationId) noexcept
{
    if (implementation_) {
        implementation_->cancel(operationId);
    }
}

void WindowsManagerNamedPipeServer::shutdown() noexcept
{
    if (implementation_) {
        implementation_->shutdown();
    }
}

} // namespace ForgeConductor::Infrastructure::Windows
