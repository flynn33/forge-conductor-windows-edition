#include "Infrastructure/Windows/Detail/DashboardIocpWorkerKernel.h"

#include "ForgeConductor/Domain/Error.h"

#include <array>
#include <condition_variable>
#include <exception>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

namespace ForgeConductor::Infrastructure::Windows::Detail {
namespace {

[[nodiscard]] Domain::Error kernelError(
    const std::string_view code,
    std::string message,
    const bool retryable = false)
{
    return Domain::makeError(code, std::move(message), retryable);
}

[[nodiscard]] bool isReservedKey(
    const DashboardIoCompletionKey key) noexcept
{
    return key.value() == DashboardIocpWorkerKernel::ShutdownKeyValue;
}

} // namespace

DashboardIocpWorkerSnapshot::DashboardIocpWorkerSnapshot(
    const std::size_t startedWorkerCount,
    const std::size_t exitedWorkerCount,
    const bool shuttingDown,
    const bool controlPostFailed,
    std::optional<DWORD> fatalNativeError) noexcept
    : startedWorkerCount_{startedWorkerCount},
      exitedWorkerCount_{exitedWorkerCount},
      shuttingDown_{shuttingDown},
      controlPostFailed_{controlPostFailed},
      fatalNativeError_{std::move(fatalNativeError)}
{
}

class DashboardIocpWorkerKernel::Impl final
    : public std::enable_shared_from_this<
          DashboardIocpWorkerKernel::Impl> {
public:
    Impl(
        std::unique_ptr<DashboardIoCompletionPort> port,
        std::shared_ptr<IDashboardIocpCompletionSink> sink) noexcept
        : port_{std::move(port)}, sink_{std::move(sink)}
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
            for (std::size_t index{};
                 index < DashboardIocpWorkerKernel::WorkerCount;
                 ++index) {
                const auto owner = shared_from_this();
                std::thread worker{
                    [owner, index]() noexcept { owner->workerMain(index); }};
                {
                    const std::lock_guard lock{stateMutex_};
                    workers_[index] = std::move(worker);
                    ++startedWorkerCount_;
                }
            }
            {
                std::unique_lock lock{stateMutex_};
                const bool ready = workerStateChanged_.wait_for(
                    lock,
                    DashboardIocpWorkerKernel::WorkerStartupTimeout,
                    [this] {
                        return enteredWorkerCount_ ==
                                   DashboardIocpWorkerKernel::WorkerCount ||
                               fatalNativeError_.has_value();
                    });
                if (ready &&
                    enteredWorkerCount_ ==
                        DashboardIocpWorkerKernel::WorkerCount &&
                    !fatalNativeError_) {
                    return Domain::Result<void>::success();
                }
            }
            beginShutdown();
            joinStartedWorkers();
            return Domain::Result<void>::failure(kernelError(
                Domain::ErrorCodes::HostCapabilityUnavailable,
                "The dashboard IOCP workers did not all enter their completion loops."));
        } catch (...) {
            beginShutdown();
            joinStartedWorkers();
            return Domain::Result<void>::failure(kernelError(
                Domain::ErrorCodes::InternalFailure,
                "The dashboard IOCP workers could not be started."));
        }
    }

    [[nodiscard]] Domain::Result<void> associateHandle(
        const HANDLE handle,
        const DashboardIoCompletionKey completionKey) noexcept
    {
        return admitAndInvoke(
            completionKey,
            [this, handle, completionKey]() noexcept {
                return port_->associateHandle(handle, completionKey);
            });
    }

    [[nodiscard]] Domain::Result<void> associateSocket(
        const SOCKET socket,
        const DashboardIoCompletionKey completionKey) noexcept
    {
        return admitAndInvoke(
            completionKey,
            [this, socket, completionKey]() noexcept {
                return port_->associateSocket(socket, completionKey);
            });
    }

    [[nodiscard]] Domain::Result<void> postAdmitted(
        const DWORD transferredBytes,
        const DashboardIoCompletionKey completionKey,
        OVERLAPPED* const operation) noexcept
    {
        return admitAndInvoke(
            completionKey,
            [this, transferredBytes, completionKey, operation]() noexcept {
                return port_->post(
                    transferredBytes, completionKey, operation);
            });
    }

    [[nodiscard]] DashboardIocpWorkerSnapshot snapshot() const noexcept
    {
        try {
            const std::lock_guard lock{stateMutex_};
            return DashboardIocpWorkerSnapshot{
                startedWorkerCount_,
                exitedWorkerCount_,
                shuttingDown_,
                controlPostFailed_,
                fatalNativeError_};
        } catch (...) {
            return DashboardIocpWorkerSnapshot{
                0U, 0U, true, true, ERROR_GEN_FAILURE};
        }
    }

    void beginShutdown() noexcept
    {
        try {
            bool postControlPackets{};
            {
                const std::lock_guard lock{stateMutex_};
                shuttingDown_ = true;
                if (activeNativeCallCount_ == 0U &&
                    !controlPacketsPosted_) {
                    controlPacketsPosted_ = true;
                    postControlPackets = true;
                }
            }
            if (!postControlPackets) {
                return;
            }

            postShutdownPackets();
        } catch (...) {
            std::terminate();
        }
    }

    void shutdown() noexcept
    {
        try {
            if (isCurrentWorker()) {
                // A completion sink may request the nonblocking transition,
                // but the process owner must retain and join the kernel from a
                // non-worker thread after the callback returns.
                beginShutdown();
                return;
            }

            const std::lock_guard shutdownLock{shutdownMutex_};
            beginShutdown();
            joinStartedWorkers();
        } catch (...) {
            std::terminate();
        }
    }

private:
    template <typename Invocation>
    [[nodiscard]] Domain::Result<void> admitAndInvoke(
        const DashboardIoCompletionKey completionKey,
        Invocation&& invocation) noexcept
    {
        bool admitted{};
        try {
            if (isReservedKey(completionKey)) {
                return Domain::Result<void>::failure(kernelError(
                    Domain::ErrorCodes::InvalidRequest,
                    "The dashboard IOCP shutdown key is reserved."));
            }
            {
                const std::lock_guard lock{stateMutex_};
                if (shuttingDown_) {
                    return Domain::Result<void>::failure(kernelError(
                        Domain::ErrorCodes::TransportClosed,
                        "The dashboard IOCP worker kernel is shutting down."));
                }
                ++activeNativeCallCount_;
                admitted = true;
            }
            auto result = std::forward<Invocation>(invocation)();
            finishNativeCall();
            admitted = false;
            return result;
        } catch (...) {
            if (admitted) {
                finishNativeCall();
            }
            return Domain::Result<void>::failure(kernelError(
                Domain::ErrorCodes::InternalFailure,
                "The dashboard IOCP operation could not be admitted safely."));
        }
    }

    void finishNativeCall() noexcept
    {
        try {
            bool postControlPackets{};
            {
                const std::lock_guard lock{stateMutex_};
                if (activeNativeCallCount_ == 0U) {
                    std::terminate();
                }
                --activeNativeCallCount_;
                if (shuttingDown_ && activeNativeCallCount_ == 0U &&
                    !controlPacketsPosted_) {
                    controlPacketsPosted_ = true;
                    postControlPackets = true;
                }
            }
            if (postControlPackets) {
                postShutdownPackets();
            }
        } catch (...) {
            std::terminate();
        }
    }

    void postShutdownPackets() noexcept
    {
        try {
            bool failed{};
            for (std::size_t index{};
                 index < DashboardIocpWorkerKernel::WorkerCount;
                 ++index) {
                const auto posted = port_->postControl(
                    DashboardIoCompletionKey{
                        DashboardIocpWorkerKernel::ShutdownKeyValue});
                failed = failed || !posted.succeeded();
            }
            if (failed) {
                const std::lock_guard lock{stateMutex_};
                controlPostFailed_ = true;
            }
        } catch (...) {
            std::terminate();
        }
    }

    [[nodiscard]] bool isCurrentWorker() const noexcept
    {
        try {
            const auto current = std::this_thread::get_id();
            const std::lock_guard lock{stateMutex_};
            for (const auto workerId : workerThreadIds_) {
                if (workerId == current) {
                    return true;
                }
            }
            return false;
        } catch (...) {
            return false;
        }
    }

    [[nodiscard]] bool mayExitAfterTimedWait() const noexcept
    {
        try {
            const std::lock_guard lock{stateMutex_};
            // Successful control posts wake one worker apiece. The finite-wait
            // fallback is needed only after at least one control post failed,
            // and never while an admitted native call is still delaying the
            // control-post phase.
            return shuttingDown_ && controlPacketsPosted_ &&
                controlPostFailed_;
        } catch (...) {
            return true;
        }
    }

    void recordFatal(const DWORD nativeError) noexcept
    {
        try {
            const DWORD retainedError = nativeError == ERROR_SUCCESS
                ? ERROR_GEN_FAILURE
                : nativeError;
            bool notify{};
            {
                const std::lock_guard lock{stateMutex_};
                if (!fatalNativeError_) {
                    fatalNativeError_ = retainedError;
                    notify = true;
                }
            }
            workerStateChanged_.notify_all();
            beginShutdown();
            if (notify) {
                sink_->fatal(retainedError);
            }
        } catch (...) {
            std::terminate();
        }
    }

    void workerMain(const std::size_t workerIndex) noexcept
    {
        try {
            {
                const std::lock_guard lock{stateMutex_};
                workerThreadIds_[workerIndex] = std::this_thread::get_id();
                ++enteredWorkerCount_;
            }
            workerStateChanged_.notify_all();

            for (;;) {
                const auto completion = port_->dequeue(
                    DashboardIocpWorkerKernel::WorkerWaitMilliseconds);
                if (completion.disposition() ==
                    DashboardIoCompletionDequeueDisposition::TimedOut) {
                    if (mayExitAfterTimedWait()) {
                        break;
                    }
                    continue;
                }
                if (completion.disposition() ==
                    DashboardIoCompletionDequeueDisposition::FatalError) {
                    recordFatal(completion.nativeError());
                    break;
                }

                const auto* const packet = completion.packet();
                if (packet == nullptr) {
                    recordFatal(ERROR_INVALID_DATA);
                    break;
                }
                if (isReservedKey(packet->completionKey)) {
                    if (completion.disposition() !=
                            DashboardIoCompletionDequeueDisposition::Succeeded ||
                        completion.nativeError() != ERROR_SUCCESS ||
                        packet->transferredBytes != 0U ||
                        packet->operation != nullptr) {
                        recordFatal(ERROR_INVALID_DATA);
                    }
                    break;
                }

                sink_->consume(
                    *packet,
                    completion.disposition() ==
                            DashboardIoCompletionDequeueDisposition::Succeeded
                        ? ERROR_SUCCESS
                        : completion.nativeError());
            }

            {
                const std::lock_guard lock{stateMutex_};
                workerThreadIds_[workerIndex] = std::thread::id{};
                ++exitedWorkerCount_;
            }
            workerStateChanged_.notify_all();
        } catch (...) {
            std::terminate();
        }
    }

    void joinStartedWorkers() noexcept
    {
        try {
            {
                std::unique_lock lock{stateMutex_};
                const bool exited = workerStateChanged_.wait_for(
                    lock,
                    DashboardIocpWorkerKernel::ShutdownDrainTimeout,
                    [this] {
                        return exitedWorkerCount_ == startedWorkerCount_;
                    });
                if (!exited) {
                    std::terminate();
                }
            }
            for (auto& worker : workers_) {
                if (worker.joinable()) {
                    worker.join();
                }
            }
        } catch (...) {
            std::terminate();
        }
    }

    std::unique_ptr<DashboardIoCompletionPort> port_;
    std::shared_ptr<IDashboardIocpCompletionSink> sink_;
    mutable std::mutex stateMutex_;
    std::mutex shutdownMutex_;
    std::condition_variable workerStateChanged_;
    std::array<std::thread, DashboardIocpWorkerKernel::WorkerCount> workers_{};
    std::array<
        std::thread::id,
        DashboardIocpWorkerKernel::WorkerCount>
        workerThreadIds_{};
    std::size_t startedWorkerCount_{};
    std::size_t enteredWorkerCount_{};
    std::size_t exitedWorkerCount_{};
    std::size_t activeNativeCallCount_{};
    std::optional<DWORD> fatalNativeError_;
    bool shuttingDown_{};
    bool controlPacketsPosted_{};
    bool controlPostFailed_{};
};

Domain::Result<std::unique_ptr<DashboardIocpWorkerKernel>>
DashboardIocpWorkerKernel::create(
    std::unique_ptr<DashboardIoCompletionPort> port,
    std::shared_ptr<IDashboardIocpCompletionSink> sink) noexcept
{
    try {
        if (port == nullptr || sink == nullptr) {
            return Domain::Result<
                std::unique_ptr<DashboardIocpWorkerKernel>>::failure(
                kernelError(
                    Domain::ErrorCodes::InvalidRequest,
                    "The dashboard IOCP worker kernel requires a port and completion sink."));
        }
        auto implementation = std::make_shared<Impl>(
            std::move(port), std::move(sink));
        auto owner = std::unique_ptr<DashboardIocpWorkerKernel>{
            new DashboardIocpWorkerKernel{implementation}};
        auto started = implementation->start();
        if (!started) {
            return Domain::Result<
                std::unique_ptr<DashboardIocpWorkerKernel>>::failure(
                std::move(started).error());
        }
        return Domain::Result<
            std::unique_ptr<DashboardIocpWorkerKernel>>::success(
            std::move(owner));
    } catch (...) {
        return Domain::Result<
            std::unique_ptr<DashboardIocpWorkerKernel>>::failure(
            kernelError(
                Domain::ErrorCodes::InternalFailure,
                "The dashboard IOCP worker kernel could not be created."));
    }
}

DashboardIocpWorkerKernel::DashboardIocpWorkerKernel(
    std::shared_ptr<Impl> implementation) noexcept
    : implementation_{std::move(implementation)}
{
}

DashboardIocpWorkerKernel::~DashboardIocpWorkerKernel() noexcept
{
    shutdown();
}

Domain::Result<void> DashboardIocpWorkerKernel::associateHandle(
    const HANDLE handle,
    const DashboardIoCompletionKey completionKey) noexcept
{
    return implementation_->associateHandle(handle, completionKey);
}

Domain::Result<void> DashboardIocpWorkerKernel::associateSocket(
    const SOCKET socket,
    const DashboardIoCompletionKey completionKey) noexcept
{
    return implementation_->associateSocket(socket, completionKey);
}

Domain::Result<void> DashboardIocpWorkerKernel::postAdmitted(
    const DWORD transferredBytes,
    const DashboardIoCompletionKey completionKey,
    OVERLAPPED* const operation) noexcept
{
    return implementation_->postAdmitted(
        transferredBytes, completionKey, operation);
}

DashboardIocpWorkerSnapshot DashboardIocpWorkerKernel::snapshot()
    const noexcept
{
    return implementation_->snapshot();
}

void DashboardIocpWorkerKernel::beginShutdown() noexcept
{
    implementation_->beginShutdown();
}

void DashboardIocpWorkerKernel::shutdown() noexcept
{
    if (implementation_ != nullptr) {
        implementation_->shutdown();
    }
}

} // namespace ForgeConductor::Infrastructure::Windows::Detail
