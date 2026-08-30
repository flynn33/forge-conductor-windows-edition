#include "ForgeConductor/Infrastructure/Windows/WindowsDashboardHandlerExecutor.h"

#include "ForgeConductor/Dashboard/DashboardHttpResponse.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <latch>
#include <memory>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

namespace {

namespace Dashboard = ForgeConductor::Dashboard;
namespace Domain = ForgeConductor::Domain;
namespace Windows = ForgeConductor::Infrastructure::Windows;

using Completion = Windows::DashboardHandlerCompletion;
using CompletionKind = Windows::DashboardHandlerCompletionKind;
using DrainObserver = Windows::IDashboardHandlerExecutorDrainObserver;
using Executor = Windows::WindowsDashboardHandlerExecutor;
using Operation = Windows::IDashboardHandlerOperation;
using Reservation = Windows::WindowsDashboardHandlerExecutor::Reservation;
using Sink = Windows::IDashboardHandlerCompletionSink;

using namespace std::chrono_literals;

std::atomic_size_t assertionCount{};

void require(const bool condition, const std::string_view message)
{
    assertionCount.fetch_add(1U, std::memory_order_relaxed);
    if (!condition) {
        throw std::runtime_error{std::string{message}};
    }
}

static_assert(std::is_abstract_v<Operation>);
static_assert(std::has_virtual_destructor_v<Operation>);
static_assert(std::is_abstract_v<Sink>);
static_assert(std::has_virtual_destructor_v<Sink>);
static_assert(std::is_abstract_v<DrainObserver>);
static_assert(std::has_virtual_destructor_v<DrainObserver>);
static_assert(!std::is_copy_constructible_v<Completion>);
static_assert(!std::is_copy_assignable_v<Completion>);
static_assert(std::is_nothrow_move_constructible_v<Completion>);
static_assert(std::is_nothrow_move_assignable_v<Completion>);
static_assert(!std::is_copy_constructible_v<Executor>);
static_assert(!std::is_move_constructible_v<Executor>);
static_assert(!std::is_copy_constructible_v<Reservation>);
static_assert(!std::is_copy_assignable_v<Reservation>);
static_assert(std::is_nothrow_move_constructible_v<Reservation>);
static_assert(std::is_nothrow_move_assignable_v<Reservation>);
static_assert(noexcept(std::declval<Sink&>().tryPost(
    std::declval<Completion>())));
static_assert(noexcept(std::declval<Executor&>().trySubmit(
    std::declval<std::unique_ptr<Operation>>(),
    std::declval<Domain::OperationContext>(),
    std::declval<std::weak_ptr<Sink>>())));
static_assert(noexcept(std::declval<Executor&>().tryReservePostDelivery()));
static_assert(noexcept(std::declval<Executor&>().bindShutdownDrainObserver(
    std::declval<std::weak_ptr<DrainObserver>>())));
static_assert(noexcept(std::declval<const Executor&>().fullyDrained()));
static_assert(noexcept(std::declval<Reservation&&>().trySubmit(
    std::declval<std::unique_ptr<Operation>>(),
    std::declval<Domain::OperationContext>(),
    std::declval<std::weak_ptr<Sink>>())));
static_assert(noexcept(std::declval<Reservation&>().release()));

[[nodiscard]] Domain::OperationContext context(
    const std::uint64_t sequence,
    const std::chrono::steady_clock::duration lifetime = 5s,
    const std::stop_token cancellation = {})
{
    std::ostringstream identifier;
    identifier << "10000000-0000-4000-8000-" << std::setfill('0')
               << std::setw(12) << sequence;
    auto operation = Domain::OperationId::parse(identifier.str());
    auto correlation = Domain::CorrelationId::parse(
        "dashboard-handler-test-" + std::to_string(sequence));
    require(operation.hasValue(), "test operation id was invalid");
    require(correlation.hasValue(), "test correlation id was invalid");
    return Domain::OperationContext{
        std::move(operation).value(),
        std::chrono::steady_clock::now() + lifetime,
        cancellation,
        std::move(correlation).value()};
}

[[nodiscard]] std::unique_ptr<Executor> executor()
{
    auto created = Executor::create();
    require(created.hasValue(), "handler executor creation failed");
    return std::move(created).value();
}

struct CompletionRecord final {
    CompletionKind kind{};
    Dashboard::DashboardPreparedExchange::Kind exchangeKind{
        Dashboard::DashboardPreparedExchange::Kind::Empty};
    std::string errorCode;
    bool success{};
};

class RecordingSink final : public Sink {
public:
    explicit RecordingSink(const bool accept = true) noexcept
        : accept_{accept}
    {
    }

    [[nodiscard]] bool tryPost(Completion completion) noexcept override
    {
        try {
            CompletionRecord record;
            record.kind = completion.kind();
            if (auto* prepared = completion.preparedResult();
                prepared != nullptr) {
                record.success = prepared->hasValue();
                if (record.success) {
                    record.exchangeKind = prepared->value().kind();
                } else {
                    record.errorCode = prepared->error().code;
                }
            } else if (auto* postDelivery =
                           completion.postDeliveryResult();
                       postDelivery != nullptr) {
                record.success = postDelivery->hasValue();
                if (!record.success) {
                    record.errorCode = postDelivery->error().code;
                }
            } else {
                malformed_.store(true, std::memory_order_release);
                return false;
            }

            {
                const std::lock_guard lock{mutex_};
                records_.push_back(std::move(record));
            }
            changed_.notify_all();
            return accept_;
        } catch (...) {
            malformed_.store(true, std::memory_order_release);
            changed_.notify_all();
            return false;
        }
    }

    [[nodiscard]] bool waitFor(
        const std::size_t count,
        const std::chrono::milliseconds timeout = 5s) const
    {
        std::unique_lock lock{mutex_};
        return changed_.wait_for(
            lock,
            timeout,
            [this, count] { return records_.size() >= count; });
    }

    [[nodiscard]] std::vector<CompletionRecord> records() const
    {
        const std::lock_guard lock{mutex_};
        return records_;
    }

    [[nodiscard]] bool malformed() const noexcept
    {
        return malformed_.load(std::memory_order_acquire);
    }

private:
    const bool accept_{};
    mutable std::mutex mutex_;
    mutable std::condition_variable changed_;
    std::vector<CompletionRecord> records_;
    std::atomic_bool malformed_{};
};

class WorkerLifecycleProbe final {
public:
    void entered(const std::thread::id worker)
    {
        const std::lock_guard lock{mutex_};
        if (std::find(workerThreads_.begin(), workerThreads_.end(), worker) !=
            workerThreads_.end()) {
            std::terminate();
        }
        workerThreads_.push_back(worker);
        changed_.notify_all();
    }

    void exited(const std::thread::id worker) noexcept
    {
        try {
            const std::lock_guard lock{mutex_};
            if (std::find(
                    workerThreads_.begin(), workerThreads_.end(), worker) ==
                    workerThreads_.end() ||
                std::find(exitedThreads_.begin(), exitedThreads_.end(), worker) !=
                    exitedThreads_.end()) {
                std::terminate();
            }
            exitedThreads_.push_back(worker);
            changed_.notify_all();
        } catch (...) {
            std::terminate();
        }
    }

    [[nodiscard]] bool waitUntilEntered(
        const std::size_t count,
        const std::chrono::milliseconds timeout = 5s) const
    {
        std::unique_lock lock{mutex_};
        return changed_.wait_for(lock, timeout, [this, count] {
            return workerThreads_.size() >= count;
        });
    }

    [[nodiscard]] bool contains(const std::thread::id candidate) const
    {
        const std::lock_guard lock{mutex_};
        return std::find(
                   workerThreads_.begin(),
                   workerThreads_.end(),
                   candidate) != workerThreads_.end();
    }

    [[nodiscard]] std::size_t exitedCount() const
    {
        const std::lock_guard lock{mutex_};
        return exitedThreads_.size();
    }

    [[nodiscard]] bool waitUntilExited(
        const std::size_t count,
        const std::chrono::milliseconds timeout = 5s) const
    {
        std::unique_lock lock{mutex_};
        return changed_.wait_for(lock, timeout, [this, count] {
            return exitedThreads_.size() >= count;
        });
    }

private:
    mutable std::mutex mutex_;
    mutable std::condition_variable changed_;
    std::vector<std::thread::id> workerThreads_;
    std::vector<std::thread::id> exitedThreads_;
};

class RecordingExecutorDrainObserver final : public DrainObserver {
public:
    explicit RecordingExecutorDrainObserver(
        Executor* executor = nullptr,
        const bool reenterShutdown = false,
        std::shared_ptr<WorkerLifecycleProbe> workerLifecycle = nullptr)
        noexcept
        : executor_{executor},
          reenterShutdown_{reenterShutdown},
          workerLifecycle_{std::move(workerLifecycle)}
    {
    }

    void handlerExecutorMayHaveDrained() noexcept override
    {
        try {
            const auto callbackThread = std::this_thread::get_id();
            if (reenterShutdown_ && executor_ != nullptr) {
                executor_->shutdown();
            }
            bool reentrantSnapshotWasDrained{true};
            if (executor_ != nullptr) {
                reentrantSnapshotWasDrained =
                    executor_->isShuttingDown() &&
                    executor_->pendingCount() == 0U &&
                    executor_->activeCount() == 0U &&
                    executor_->reservationCount() == 0U &&
                    executor_->fullyDrained();
            }
            const bool callbackWasOffWorkers =
                workerLifecycle_ == nullptr ||
                !workerLifecycle_->contains(callbackThread);
            const bool callbackFollowedWorkerExits =
                workerLifecycle_ == nullptr ||
                workerLifecycle_->exitedCount() == Executor::WorkerCount;
            {
                const std::lock_guard lock{mutex_};
                ++notificationCount_;
                callbackThread_ = callbackThread;
                reentrantSnapshotWasDrained_ =
                    reentrantSnapshotWasDrained_ &&
                    reentrantSnapshotWasDrained;
                callbackWasOffWorkers_ =
                    callbackWasOffWorkers_ && callbackWasOffWorkers;
                callbackFollowedWorkerExits_ =
                    callbackFollowedWorkerExits_ &&
                    callbackFollowedWorkerExits;
            }
            changed_.notify_all();
        } catch (...) {
            std::terminate();
        }
    }

    [[nodiscard]] bool waitFor(
        const std::size_t count,
        const std::chrono::milliseconds timeout = 5s) const
    {
        std::unique_lock lock{mutex_};
        return changed_.wait_for(lock, timeout, [this, count] {
            return notificationCount_ >= count;
        });
    }

    [[nodiscard]] std::size_t notificationCount() const
    {
        const std::lock_guard lock{mutex_};
        return notificationCount_;
    }

    [[nodiscard]] bool reentrantSnapshotWasDrained() const
    {
        const std::lock_guard lock{mutex_};
        return reentrantSnapshotWasDrained_;
    }

    [[nodiscard]] bool callbackWasOffWorkers() const
    {
        const std::lock_guard lock{mutex_};
        return callbackWasOffWorkers_;
    }

    [[nodiscard]] bool callbackFollowedWorkerExits() const
    {
        const std::lock_guard lock{mutex_};
        return callbackFollowedWorkerExits_;
    }

    [[nodiscard]] std::thread::id callbackThread() const
    {
        const std::lock_guard lock{mutex_};
        return callbackThread_;
    }

private:
    Executor* executor_{};
    bool reenterShutdown_{};
    std::shared_ptr<WorkerLifecycleProbe> workerLifecycle_;
    mutable std::mutex mutex_;
    mutable std::condition_variable changed_;
    std::thread::id callbackThread_{};
    std::size_t notificationCount_{};
    bool reentrantSnapshotWasDrained_{true};
    bool callbackWasOffWorkers_{true};
    bool callbackFollowedWorkerExits_{true};
};

class ExternallyCountedDrainObserver final : public DrainObserver {
public:
    explicit ExternallyCountedDrainObserver(
        std::shared_ptr<std::atomic_size_t> notifications) noexcept
        : notifications_{std::move(notifications)}
    {
    }

    void handlerExecutorMayHaveDrained() noexcept override
    {
        notifications_->fetch_add(1U, std::memory_order_release);
    }

private:
    std::shared_ptr<std::atomic_size_t> notifications_;
};

[[nodiscard]] Completion postDeliverySuccess()
{
    return Completion::postDelivery(Domain::Result<void>::success());
}

[[nodiscard]] Completion postDeliveryFailure(Domain::Error error)
{
    return Completion::postDelivery(
        Domain::Result<void>::failure(std::move(error)));
}

class ImmediateOperation final : public Operation {
public:
    explicit ImmediateOperation(std::atomic_size_t* executions = nullptr)
        noexcept
        : executions_{executions}
    {
    }

    [[nodiscard]] CompletionKind completionKind() const noexcept override
    {
        return CompletionKind::PostDelivery;
    }

    [[nodiscard]] Completion execute(
        const Domain::OperationContext&) override
    {
        if (executions_ != nullptr) {
            executions_->fetch_add(1U, std::memory_order_relaxed);
        }
        return postDeliverySuccess();
    }

private:
    std::atomic_size_t* executions_{};
};

class WaitGate final {
public:
    [[nodiscard]] Domain::Result<void> wait(
        const Domain::OperationContext& operationContext)
    {
        std::stop_callback notifyOnCancellation{
            operationContext.cancellation,
            [this]() noexcept { changed_.notify_all(); }};
        std::unique_lock lock{mutex_};
        ++entered_;
        if (entered_ > maximumEntered_) {
            maximumEntered_ = entered_;
        }
        changed_.notify_all();
        const bool signalled = changed_.wait_until(
            lock,
            operationContext.deadline,
            [this, &operationContext] {
                return released_ ||
                       operationContext.isCancellationRequested();
            });
        --entered_;
        changed_.notify_all();

        if (operationContext.isCancellationRequested()) {
            return Domain::Result<void>::failure(Domain::makeError(
                Domain::ErrorCodes::Cancelled,
                "The test gate operation was cancelled."));
        }
        if (!signalled) {
            return Domain::Result<void>::failure(Domain::makeError(
                Domain::ErrorCodes::DeadlineExceeded,
                "The test gate operation exceeded its deadline."));
        }
        return Domain::Result<void>::success();
    }

    [[nodiscard]] bool waitUntilEntered(
        const std::size_t count,
        const std::chrono::milliseconds timeout = 5s) const
    {
        std::unique_lock lock{mutex_};
        return changed_.wait_for(
            lock,
            timeout,
            [this, count] { return entered_ >= count; });
    }

    void release() noexcept
    {
        try {
            {
                const std::lock_guard lock{mutex_};
                released_ = true;
            }
            changed_.notify_all();
        } catch (...) {
            std::terminate();
        }
    }

    [[nodiscard]] std::size_t maximumEntered() const
    {
        const std::lock_guard lock{mutex_};
        return maximumEntered_;
    }

private:
    mutable std::mutex mutex_;
    mutable std::condition_variable changed_;
    std::size_t entered_{};
    std::size_t maximumEntered_{};
    bool released_{};
};

class GateOperation final : public Operation {
public:
    explicit GateOperation(std::shared_ptr<WaitGate> gate) noexcept
        : gate_{std::move(gate)}
    {
    }

    [[nodiscard]] CompletionKind completionKind() const noexcept override
    {
        return CompletionKind::PostDelivery;
    }

    [[nodiscard]] Completion execute(
        const Domain::OperationContext& operationContext) override
    {
        auto result = gate_->wait(operationContext);
        if (!result) {
            return postDeliveryFailure(std::move(result).error());
        }
        return postDeliverySuccess();
    }

private:
    std::shared_ptr<WaitGate> gate_;
};

class ManualGate final {
public:
    void wait()
    {
        std::unique_lock lock{mutex_};
        ++entered_;
        changed_.notify_all();
        changed_.wait(lock, [this] { return released_; });
    }

    [[nodiscard]] bool waitUntilEntered(
        const std::size_t count = 1U,
        const std::chrono::milliseconds timeout = 5s) const
    {
        std::unique_lock lock{mutex_};
        return changed_.wait_for(
            lock, timeout, [this, count] { return entered_ >= count; });
    }

    void release() noexcept
    {
        try {
            {
                const std::lock_guard lock{mutex_};
                released_ = true;
            }
            changed_.notify_all();
        } catch (...) {
            std::terminate();
        }
    }

private:
    mutable std::mutex mutex_;
    mutable std::condition_variable changed_;
    std::size_t entered_{};
    bool released_{};
};

class ManualGateOperation final : public Operation {
public:
    explicit ManualGateOperation(std::shared_ptr<ManualGate> gate) noexcept
        : gate_{std::move(gate)}
    {
    }

    [[nodiscard]] CompletionKind completionKind() const noexcept override
    {
        return CompletionKind::PostDelivery;
    }

    [[nodiscard]] Completion execute(
        const Domain::OperationContext&) override
    {
        gate_->wait();
        return postDeliverySuccess();
    }

private:
    std::shared_ptr<ManualGate> gate_;
};

class WorkerLifecycleRegistration final {
public:
    ~WorkerLifecycleRegistration() noexcept
    {
        if (probe_ != nullptr) {
            probe_->exited(worker_);
        }
    }

    void bind(std::shared_ptr<WorkerLifecycleProbe> probe)
    {
        if (probe == nullptr || probe_ != nullptr) {
            std::terminate();
        }
        worker_ = std::this_thread::get_id();
        probe_ = std::move(probe);
        probe_->entered(worker_);
    }

private:
    std::shared_ptr<WorkerLifecycleProbe> probe_;
    std::thread::id worker_{};
};

class WorkerLifecycleOperation final : public Operation {
public:
    WorkerLifecycleOperation(
        std::shared_ptr<ManualGate> gate,
        std::shared_ptr<WorkerLifecycleProbe> probe) noexcept
        : gate_{std::move(gate)}, probe_{std::move(probe)}
    {
    }

    [[nodiscard]] CompletionKind completionKind() const noexcept override
    {
        return CompletionKind::PostDelivery;
    }

    [[nodiscard]] Completion execute(
        const Domain::OperationContext&) override
    {
        thread_local WorkerLifecycleRegistration registration;
        registration.bind(probe_);
        gate_->wait();
        return postDeliverySuccess();
    }

private:
    std::shared_ptr<ManualGate> gate_;
    std::shared_ptr<WorkerLifecycleProbe> probe_;
};

class OrderRecorder final {
public:
    void append(const std::size_t value)
    {
        {
            const std::lock_guard lock{mutex_};
            values_.push_back(value);
        }
        changed_.notify_all();
    }

    [[nodiscard]] bool waitFor(
        const std::size_t count,
        const std::chrono::milliseconds timeout = 5s) const
    {
        std::unique_lock lock{mutex_};
        return changed_.wait_for(
            lock,
            timeout,
            [this, count] { return values_.size() >= count; });
    }

    [[nodiscard]] std::vector<std::size_t> values() const
    {
        const std::lock_guard lock{mutex_};
        return values_;
    }

private:
    mutable std::mutex mutex_;
    mutable std::condition_variable changed_;
    std::vector<std::size_t> values_;
};

class OrderedOperation final : public Operation {
public:
    OrderedOperation(
        std::shared_ptr<OrderRecorder> recorder,
        const std::size_t value) noexcept
        : recorder_{std::move(recorder)}, value_{value}
    {
    }

    [[nodiscard]] CompletionKind completionKind() const noexcept override
    {
        return CompletionKind::PostDelivery;
    }

    [[nodiscard]] Completion execute(
        const Domain::OperationContext&) override
    {
        recorder_->append(value_);
        return postDeliverySuccess();
    }

private:
    std::shared_ptr<OrderRecorder> recorder_;
    const std::size_t value_{};
};

class ThrowingOperation final : public Operation {
public:
    [[nodiscard]] CompletionKind completionKind() const noexcept override
    {
        return CompletionKind::PostDelivery;
    }

    [[nodiscard]] Completion execute(
        const Domain::OperationContext&) override
    {
        throw std::runtime_error{"simulated handler failure"};
    }
};

class MismatchedOperation final : public Operation {
public:
    [[nodiscard]] CompletionKind completionKind() const noexcept override
    {
        return CompletionKind::PreparedExchange;
    }

    [[nodiscard]] Completion execute(
        const Domain::OperationContext&) override
    {
        return postDeliverySuccess();
    }
};

[[nodiscard]] std::vector<std::byte> bytes(const std::string_view value)
{
    std::vector<std::byte> result;
    result.reserve(value.size());
    std::transform(
        value.begin(),
        value.end(),
        std::back_inserter(result),
        [](const unsigned char character) {
            return static_cast<std::byte>(character);
        });
    return result;
}

class PreparedOperation final : public Operation {
public:
    [[nodiscard]] CompletionKind completionKind() const noexcept override
    {
        return CompletionKind::PreparedExchange;
    }

    [[nodiscard]] Completion execute(
        const Domain::OperationContext&) override
    {
        auto encoded = Dashboard::DashboardHttpResponseEncoder::encode(
            Dashboard::DashboardHttpResponse{
                200U,
                "application/json; charset=utf-8",
                bytes("{\"ok\":true}")});
        auto prepared = Dashboard::DashboardPreparedExchange::createComplete(
            std::move(encoded),
            Dashboard::DashboardPostDeliveryAction::None);
        return Completion::prepared(std::move(prepared));
    }
};

class UndefinedKindOperation final : public Operation {
public:
    [[nodiscard]] CompletionKind completionKind() const noexcept override
    {
        return static_cast<CompletionKind>(0xffU);
    }

    [[nodiscard]] Completion execute(
        const Domain::OperationContext&) override
    {
        return postDeliverySuccess();
    }
};

void exactBoundsAndImmediateAdmissionValidation()
{
    require(Executor::WorkerCount == 4U, "worker count was not exactly four");
    require(Executor::QueueCapacity == 8U, "queue capacity was not exactly eight");
    require(
        Executor::ShutdownDrainTimeout == 5s,
        "shutdown drain timeout was not five seconds");

    auto owner = executor();
    const auto sink = std::make_shared<RecordingSink>();

    auto nullOperation = owner->trySubmit({}, context(1U), sink);
    require(!nullOperation, "null operation was accepted");
    require(
        nullOperation.error().code == Domain::ErrorCodes::InvalidRequest,
        "null operation returned wrong error");

    std::weak_ptr<Sink> expiredSink;
    {
        const auto temporary = std::make_shared<RecordingSink>();
        expiredSink = temporary;
    }
    auto expiredSinkResult = owner->trySubmit(
        std::make_unique<ImmediateOperation>(),
        context(2U),
        expiredSink);
    require(!expiredSinkResult, "expired completion sink was accepted");
    require(
        expiredSinkResult.error().code == Domain::ErrorCodes::InvalidRequest,
        "expired sink returned wrong error");

    auto undefined = owner->trySubmit(
        std::make_unique<UndefinedKindOperation>(),
        context(3U),
        sink);
    require(!undefined, "undefined task kind was accepted");
    require(
        undefined.error().code == Domain::ErrorCodes::InvalidRequest,
        "undefined task kind returned wrong error");

    std::stop_source cancelledSource;
    static_cast<void>(cancelledSource.request_stop());
    auto cancelled = owner->trySubmit(
        std::make_unique<ImmediateOperation>(),
        context(4U, 5s, cancelledSource.get_token()),
        sink);
    require(!cancelled, "already-cancelled task was accepted");
    require(
        cancelled.error().code == Domain::ErrorCodes::Cancelled,
        "cancelled admission returned wrong error");

    auto expired = owner->trySubmit(
        std::make_unique<ImmediateOperation>(),
        context(5U, -1ms),
        sink);
    require(!expired, "expired task was accepted");
    require(
        expired.error().code == Domain::ErrorCodes::DeadlineExceeded,
        "expired admission returned wrong error");

    owner->shutdown();
}

void fourWorkersAndEightPendingTasksSaturateImmediately()
{
    auto gate = std::make_shared<WaitGate>();
    auto owner = executor();
    const auto sink = std::make_shared<RecordingSink>();

    for (std::size_t index{}; index < Executor::WorkerCount; ++index) {
        auto admitted = owner->trySubmit(
            std::make_unique<GateOperation>(gate),
            context(10U + index),
            sink);
        require(admitted.hasValue(), "active gate task was rejected");
    }
    require(
        gate->waitUntilEntered(Executor::WorkerCount),
        "four workers did not execute in parallel");
    require(owner->activeCount() == 4U, "active count was not four");

    for (std::size_t index{}; index < Executor::QueueCapacity; ++index) {
        auto admitted = owner->trySubmit(
            std::make_unique<GateOperation>(gate),
            context(20U + index),
            sink);
        require(admitted.hasValue(), "bounded pending task was rejected");
    }
    require(owner->pendingCount() == 8U, "pending count was not eight");

    const auto started = std::chrono::steady_clock::now();
    auto saturated = owner->trySubmit(
        std::make_unique<ImmediateOperation>(),
        context(40U),
        sink);
    const auto elapsed = std::chrono::steady_clock::now() - started;
    require(!saturated, "ninth pending task was admitted");
    require(
        saturated.error().code == Domain::ErrorCodes::LimitExceeded,
        "saturation returned wrong error");
    require(saturated.error().retryable, "saturation was not retryable");
    require(elapsed < 250ms, "saturation rejection blocked");
    require(owner->pendingCount() == 8U, "saturation changed queue depth");

    gate->release();
    require(sink->waitFor(12U), "accepted tasks did not drain");
    owner->shutdown();
    require(gate->maximumEntered() == 4U, "more than four tasks ran in parallel");
    require(owner->activeCount() == 0U, "active tasks remained after shutdown");
    require(owner->pendingCount() == 0U, "pending tasks remained after shutdown");
}

void reservationsShareEightPendingPositionsAndConvertOnce()
{
    auto gate = std::make_shared<WaitGate>();
    auto owner = executor();
    const auto sink = std::make_shared<RecordingSink>();

    for (std::size_t index{}; index < Executor::WorkerCount; ++index) {
        require(
            owner->trySubmit(
                     std::make_unique<GateOperation>(gate),
                     context(50U + index),
                     sink)
                .hasValue(),
            "reservation blocker was rejected");
    }
    require(
        gate->waitUntilEntered(Executor::WorkerCount),
        "reservation blockers did not occupy all workers");

    std::vector<Reservation> reservations;
    reservations.reserve(3U);
    for (std::size_t index{}; index < 3U; ++index) {
        auto reserved = owner->tryReservePostDelivery();
        require(reserved.hasValue(), "post-delivery reservation was rejected");
        reservations.push_back(std::move(reserved).value());
    }
    require(owner->reservationCount() == 3U, "reservation count was not three");

    for (std::size_t index{}; index < 5U; ++index) {
        require(
            owner->trySubmit(
                     std::make_unique<ImmediateOperation>(),
                     context(60U + index),
                     sink)
                .hasValue(),
            "mixed-capacity pending task was rejected");
    }
    require(owner->pendingCount() == 5U, "mixed pending count was not five");

    auto saturatedReservation = owner->tryReservePostDelivery();
    require(!saturatedReservation, "ninth mixed position was reserved");
    require(
        saturatedReservation.error().code ==
            Domain::ErrorCodes::LimitExceeded,
        "mixed reservation saturation returned wrong error");
    require(
        saturatedReservation.error().retryable,
        "mixed reservation saturation was not retryable");

    auto saturatedSubmission = owner->trySubmit(
        std::make_unique<ImmediateOperation>(),
        context(70U),
        sink);
    require(!saturatedSubmission, "ninth mixed position was submitted");
    require(
        saturatedSubmission.error().code == Domain::ErrorCodes::LimitExceeded,
        "mixed submission saturation returned wrong error");

    auto nullReservedSubmission = std::move(reservations[0]).trySubmit(
        {}, context(71U), sink);
    require(!nullReservedSubmission, "reserved null operation was accepted");
    require(
        nullReservedSubmission.error().code ==
            Domain::ErrorCodes::InvalidRequest,
        "reserved null operation returned wrong error");
    require(
        reservations[0].valid(),
        "reserved null validation consumed capacity");

    auto wrongKind = std::move(reservations[0]).trySubmit(
        std::make_unique<PreparedOperation>(),
        context(72U),
        sink);
    require(!wrongKind, "prepared work used a post-delivery reservation");
    require(
        wrongKind.error().code == Domain::ErrorCodes::InvalidRequest,
        "reserved wrong kind returned wrong error");
    require(
        reservations[0].valid(),
        "reserved wrong-kind validation consumed capacity");

    std::stop_source cancelledSource;
    static_cast<void>(cancelledSource.request_stop());
    auto cancelled = std::move(reservations[0]).trySubmit(
        std::make_unique<ImmediateOperation>(),
        context(73U, 5s, cancelledSource.get_token()),
        sink);
    require(!cancelled, "cancelled reserved work was accepted");
    require(
        cancelled.error().code == Domain::ErrorCodes::Cancelled,
        "cancelled reserved work returned wrong error");
    require(
        reservations[0].valid(),
        "cancelled reserved validation consumed capacity");

    std::atomic_size_t postDeliveryExecutions{};
    auto converted = std::move(reservations[0]).trySubmit(
        std::make_unique<ImmediateOperation>(&postDeliveryExecutions),
        context(74U),
        sink);
    require(converted.hasValue(), "reserved position did not convert");
    require(!reservations[0].valid(), "converted reservation stayed valid");
    require(owner->reservationCount() == 2U, "conversion did not consume once");
    require(owner->pendingCount() == 6U, "conversion did not enqueue once");

    auto doubleConversion = std::move(reservations[0]).trySubmit(
        std::make_unique<ImmediateOperation>(&postDeliveryExecutions),
        context(75U),
        sink);
    require(!doubleConversion, "reservation converted twice");
    require(
        doubleConversion.error().code == Domain::ErrorCodes::Conflict,
        "double conversion returned wrong error");
    require(owner->pendingCount() == 6U, "double conversion changed the queue");

    Reservation moved{std::move(reservations[1])};
    require(!reservations[1].valid(), "moved-from reservation stayed valid");
    require(moved.valid(), "moved reservation lost capacity");
    auto movedFromUse = std::move(reservations[1]).trySubmit(
        std::make_unique<ImmediateOperation>(),
        context(76U),
        sink);
    require(!movedFromUse, "moved-from reservation submitted work");
    require(
        movedFromUse.error().code == Domain::ErrorCodes::Conflict,
        "moved-from reservation returned wrong error");
    moved.release();
    moved.release();
    require(owner->reservationCount() == 1U, "release was not exactly once");

    Reservation assigned;
    assigned = std::move(reservations[2]);
    require(!reservations[2].valid(), "move-assigned source stayed valid");
    require(assigned.valid(), "move-assigned reservation lost capacity");
    assigned.release();
    require(owner->reservationCount() == 0U, "assigned release leaked capacity");

    std::vector<Reservation> finalReservations;
    finalReservations.reserve(2U);
    for (std::size_t index{}; index < 2U; ++index) {
        auto reserved = owner->tryReservePostDelivery();
        require(reserved.hasValue(), "replacement reservation was rejected");
        finalReservations.push_back(std::move(reserved).value());
    }
    require(
        owner->pendingCount() + owner->reservationCount() ==
            Executor::QueueCapacity,
        "replacement reservations did not restore exact saturation");
    finalReservations[0].release();
    require(
        owner->trySubmit(
                 std::make_unique<ImmediateOperation>(),
                 context(77U),
                 sink)
            .hasValue(),
        "released reservation did not restore ordinary capacity");
    require(
        owner->pendingCount() + owner->reservationCount() ==
            Executor::QueueCapacity,
        "ordinary submission did not reuse exactly one position");

    auto finalConversion = std::move(finalReservations[1]).trySubmit(
        std::make_unique<ImmediateOperation>(&postDeliveryExecutions),
        context(78U),
        sink);
    require(finalConversion.hasValue(), "saturated reservation did not convert");
    require(owner->reservationCount() == 0U, "final conversion leaked reservation");
    require(owner->pendingCount() == 8U, "final conversion changed total capacity");

    auto ninthAfterConversion = owner->trySubmit(
        std::make_unique<ImmediateOperation>(),
        context(79U),
        sink);
    require(!ninthAfterConversion, "conversion opened a ninth queue position");
    require(
        ninthAfterConversion.error().code ==
            Domain::ErrorCodes::LimitExceeded,
        "post-conversion saturation returned wrong error");

    gate->release();
    require(sink->waitFor(12U), "mixed reserved work did not drain");
    owner->shutdown();
    require(
        postDeliveryExecutions.load(std::memory_order_acquire) == 2U,
        "one-shot reservations executed the wrong number of tasks");
    require(owner->pendingCount() == 0U, "reserved work remained pending");
    require(owner->reservationCount() == 0U, "reserved capacity remained live");
}

void shutdownInvalidatesReservationsAndRejectsNewOnes()
{
    auto owner = executor();
    const auto sink = std::make_shared<RecordingSink>();
    auto reserved = owner->tryReservePostDelivery();
    require(reserved.hasValue(), "shutdown reservation was rejected");
    auto reservation = std::move(reserved).value();
    require(reservation.valid(), "fresh shutdown reservation was invalid");
    require(owner->reservationCount() == 1U, "shutdown reservation was not counted");

    owner->beginShutdown();
    require(!reservation.valid(), "shutdown left reservation valid");
    auto submitted = std::move(reservation).trySubmit(
        std::make_unique<ImmediateOperation>(),
        context(80U),
        sink);
    require(!submitted, "shutdown reservation submitted new work");
    require(
        submitted.error().code == Domain::ErrorCodes::TransportClosed,
        "shutdown reservation returned wrong error");
    require(
        owner->reservationCount() == 1U,
        "failed shutdown submission silently consumed reservation");
    reservation.release();
    require(owner->reservationCount() == 0U, "shutdown release leaked capacity");

    auto afterShutdown = owner->tryReservePostDelivery();
    require(!afterShutdown, "shutdown executor issued another reservation");
    require(
        afterShutdown.error().code == Domain::ErrorCodes::TransportClosed,
        "post-shutdown reservation returned wrong error");

    Reservation empty;
    auto emptySubmit = std::move(empty).trySubmit(
        std::make_unique<ImmediateOperation>(),
        context(81U),
        sink);
    require(!emptySubmit, "empty reservation submitted work");
    require(
        emptySubmit.error().code == Domain::ErrorCodes::Conflict,
        "empty reservation returned wrong error");
    owner->shutdown();
}

void unusedReservationSafelyOutlivesOuterExecutor()
{
    Reservation survivor;
    {
        auto owner = executor();
        auto reserved = owner->tryReservePostDelivery();
        require(reserved.hasValue(), "lifetime reservation was rejected");
        survivor = std::move(reserved).value();
        require(survivor.valid(), "lifetime reservation started invalid");
        require(owner->reservationCount() == 1U, "lifetime token was not counted");

        owner.reset();
        require(
            !survivor.valid(),
            "reservation stayed valid after outer executor destruction");
    }

    survivor.release();
    survivor.release();
    require(!survivor.valid(), "released lifetime reservation became valid");

    {
        Reservation destructionOnly;
        auto owner = executor();
        auto reserved = owner->tryReservePostDelivery();
        require(
            reserved.hasValue(),
            "destruction-only lifetime reservation was rejected");
        destructionOnly = std::move(reserved).value();
        owner.reset();
        require(
            !destructionOnly.valid(),
            "destruction-only token stayed valid after outer destruction");
    }
}

void pendingTasksDequeueFifoWhileOtherWorkersRemainBusy()
{
    auto owner = executor();
    const auto sink = std::make_shared<RecordingSink>();
    std::vector<std::shared_ptr<WaitGate>> blockers;
    blockers.reserve(Executor::WorkerCount);

    for (std::size_t index{}; index < Executor::WorkerCount; ++index) {
        auto gate = std::make_shared<WaitGate>();
        blockers.push_back(gate);
        auto admitted = owner->trySubmit(
            std::make_unique<GateOperation>(std::move(gate)),
            context(100U + index),
            sink);
        require(admitted.hasValue(), "FIFO blocker was rejected");
    }
    for (const auto& gate : blockers) {
        require(gate->waitUntilEntered(1U), "FIFO blocker did not start");
    }

    auto order = std::make_shared<OrderRecorder>();
    for (std::size_t index{}; index < Executor::QueueCapacity; ++index) {
        auto admitted = owner->trySubmit(
            std::make_unique<OrderedOperation>(order, index),
            context(110U + index),
            sink);
        require(admitted.hasValue(), "FIFO task was rejected");
    }

    blockers.front()->release();
    require(order->waitFor(8U), "single released worker did not drain FIFO queue");
    const auto observed = order->values();
    require(observed.size() == 8U, "FIFO recorder count was wrong");
    for (std::size_t index{}; index < observed.size(); ++index) {
        require(observed[index] == index, "pending queue was not FIFO");
    }

    for (std::size_t index = 1U; index < blockers.size(); ++index) {
        blockers[index]->release();
    }
    require(sink->waitFor(12U), "FIFO test completions did not drain");
    owner->shutdown();
}

void taskExceptionsAndKindMismatchBecomeTypedCompletions()
{
    auto owner = executor();
    const auto sink = std::make_shared<RecordingSink>();

    require(
        owner->trySubmit(
                 std::make_unique<ThrowingOperation>(),
                 context(200U),
                 sink)
            .hasValue(),
        "throwing operation was not admitted");
    require(
        owner->trySubmit(
                 std::make_unique<MismatchedOperation>(),
                 context(201U),
                 sink)
            .hasValue(),
        "mismatched operation was not admitted");
    require(
        owner->trySubmit(
                 std::make_unique<PreparedOperation>(),
                 context(202U),
                 sink)
            .hasValue(),
        "prepared operation was not admitted");
    require(sink->waitFor(3U), "typed completions did not arrive");
    owner->shutdown();

    const auto records = sink->records();
    require(records.size() == 3U, "typed completion count was wrong");
    std::size_t internalFailures{};
    std::size_t integrityFailures{};
    std::size_t preparedSuccesses{};
    for (const auto& record : records) {
        if (record.errorCode == Domain::ErrorCodes::InternalFailure) {
            ++internalFailures;
        }
        if (record.errorCode == Domain::ErrorCodes::IntegrityFailure) {
            ++integrityFailures;
            require(
                record.kind == CompletionKind::PreparedExchange,
                "kind mismatch failure changed the declared kind");
        }
        if (record.success &&
            record.kind == CompletionKind::PreparedExchange) {
            ++preparedSuccesses;
            require(
                record.exchangeKind ==
                    Dashboard::DashboardPreparedExchange::Kind::Complete,
                "prepared success lost its owned exchange");
        }
    }
    require(internalFailures == 1U, "exception was not one internal failure");
    require(integrityFailures == 1U, "kind mismatch was not one integrity failure");
    require(preparedSuccesses == 1U, "prepared success was not delivered");
    require(!sink->malformed(), "typed completion sink observed malformed state");

    auto rejectingOwner = executor();
    const auto rejecting = std::make_shared<RecordingSink>(false);
    const auto accepting = std::make_shared<RecordingSink>();
    require(
        rejectingOwner
            ->trySubmit(
                std::make_unique<ImmediateOperation>(),
                context(203U),
                rejecting)
            .hasValue(),
        "nonaccepting sink task was rejected");
    require(rejecting->waitFor(1U), "nonaccepting sink was not attempted once");
    require(
        rejectingOwner
            ->trySubmit(
                std::make_unique<ImmediateOperation>(),
                context(204U),
                accepting)
            .hasValue(),
        "task after nonaccepting sink was rejected");
    require(accepting->waitFor(1U), "worker stalled after sink rejection");
    rejectingOwner->shutdown();
}

void callerCancellationAndDeadlineReachRunningOperations()
{
    auto owner = executor();
    const auto sink = std::make_shared<RecordingSink>();

    std::stop_source cancellation;
    auto cancelledGate = std::make_shared<WaitGate>();
    require(
        owner->trySubmit(
                 std::make_unique<GateOperation>(cancelledGate),
                 context(250U, 5s, cancellation.get_token()),
                 sink)
            .hasValue(),
        "caller-cancelled operation was not admitted");
    require(
        cancelledGate->waitUntilEntered(1U),
        "caller-cancelled operation did not start");
    static_cast<void>(cancellation.request_stop());
    require(sink->waitFor(1U), "caller cancellation did not reach operation");

    auto deadlineGate = std::make_shared<WaitGate>();
    require(
        owner->trySubmit(
                 std::make_unique<GateOperation>(deadlineGate),
                 context(251U, 100ms),
                 sink)
            .hasValue(),
        "deadline operation was not admitted");
    require(deadlineGate->waitUntilEntered(1U), "deadline operation did not start");
    require(sink->waitFor(2U), "running operation deadline did not complete");
    owner->shutdown();

    const auto records = sink->records();
    require(records.size() == 2U, "context completion count was wrong");
    require(
        records[0].errorCode == Domain::ErrorCodes::Cancelled,
        "caller cancellation returned wrong completion");
    require(
        records[1].errorCode == Domain::ErrorCodes::DeadlineExceeded,
        "running deadline returned wrong completion");
}

void shutdownCancelsQueuedAndActiveWorkThenJoins()
{
    auto owner = executor();
    const auto sink = std::make_shared<RecordingSink>();
    auto activeGate = std::make_shared<WaitGate>();
    std::atomic_size_t queuedExecutions{};

    for (std::size_t index{}; index < Executor::WorkerCount; ++index) {
        require(
            owner->trySubmit(
                     std::make_unique<GateOperation>(activeGate),
                     context(300U + index),
                     sink)
                .hasValue(),
            "shutdown active task was rejected");
    }
    require(
        activeGate->waitUntilEntered(Executor::WorkerCount),
        "shutdown active tasks did not start");

    constexpr std::size_t QueuedCount = 5U;
    for (std::size_t index{}; index < QueuedCount; ++index) {
        require(
            owner->trySubmit(
                     std::make_unique<ImmediateOperation>(&queuedExecutions),
                     context(310U + index),
                     sink)
                .hasValue(),
            "shutdown queued task was rejected");
    }

    owner->shutdown();
    require(owner->isShuttingDown(), "shutdown state was not retained");
    require(owner->activeCount() == 0U, "shutdown retained active tasks");
    require(owner->pendingCount() == 0U, "shutdown retained pending tasks");
    require(
        queuedExecutions.load(std::memory_order_acquire) == 0U,
        "shutdown executed queued application work");
    require(
        sink->waitFor(Executor::WorkerCount + QueuedCount),
        "shutdown did not deliver every accepted cancellation");
    const auto records = sink->records();
    require(
        records.size() == Executor::WorkerCount + QueuedCount,
        "shutdown completion count was wrong");
    for (const auto& record : records) {
        require(!record.success, "shutdown delivered a successful task");
        require(
            record.errorCode == Domain::ErrorCodes::Cancelled,
            "shutdown completion was not cancelled");
    }

    auto afterShutdown = owner->trySubmit(
        std::make_unique<ImmediateOperation>(),
        context(320U),
        sink);
    require(!afterShutdown, "shutdown executor accepted new work");
    require(
        afterShutdown.error().code == Domain::ErrorCodes::TransportClosed,
        "post-shutdown admission returned wrong error");
}

void enqueueShutdownRaceRetainsEveryAcceptedCompletion()
{
    auto owner = executor();
    const auto sink = std::make_shared<RecordingSink>();
    constexpr std::size_t ProducerCount = 8U;
    constexpr std::size_t AttemptsPerProducer = 100U;
    std::latch start{1};
    std::atomic_size_t accepted{};
    std::atomic_size_t rejected{};
    std::atomic_size_t firstAttempts{};
    std::atomic_bool malformedError{};
    std::vector<std::jthread> producers;
    producers.reserve(ProducerCount);

    for (std::size_t producer{}; producer < ProducerCount; ++producer) {
        producers.emplace_back([&, producer](std::stop_token) {
            start.wait();
            for (std::size_t attempt{};
                 attempt < AttemptsPerProducer;
                 ++attempt) {
                const auto sequence =
                    1'000U + producer * AttemptsPerProducer + attempt;
                auto submitted = owner->trySubmit(
                    std::make_unique<ImmediateOperation>(),
                    context(sequence),
                    sink);
                if (submitted) {
                    accepted.fetch_add(1U, std::memory_order_relaxed);
                } else {
                    const auto& code = submitted.error().code;
                    if (code != Domain::ErrorCodes::LimitExceeded &&
                        code != Domain::ErrorCodes::TransportClosed) {
                        malformedError.store(true, std::memory_order_release);
                    }
                    rejected.fetch_add(1U, std::memory_order_relaxed);
                }
                if (attempt == 0U) {
                    firstAttempts.fetch_add(1U, std::memory_order_release);
                    firstAttempts.notify_all();
                }
            }
        });
    }

    std::jthread shutdownThread{[&](std::stop_token) {
        start.wait();
        auto observed = firstAttempts.load(std::memory_order_acquire);
        while (observed != ProducerCount) {
            firstAttempts.wait(observed, std::memory_order_acquire);
            observed = firstAttempts.load(std::memory_order_acquire);
        }
        owner->shutdown();
    }};
    start.count_down();
    producers.clear();
    shutdownThread.join();

    const auto acceptedCount = accepted.load(std::memory_order_acquire);
    const auto rejectedCount = rejected.load(std::memory_order_acquire);
    require(
        acceptedCount + rejectedCount ==
            ProducerCount * AttemptsPerProducer,
        "enqueue/shutdown race lost an admission result");
    require(acceptedCount > 0U, "enqueue/shutdown race accepted no work");
    require(!malformedError.load(), "enqueue/shutdown race returned wrong error");
    require(
        sink->waitFor(acceptedCount),
        "enqueue/shutdown race lost an accepted completion");
    require(
        sink->records().size() == acceptedCount,
        "accepted/completed race counts differed");
    require(owner->activeCount() == 0U, "race retained active tasks");
    require(owner->pendingCount() == 0U, "race retained queued tasks");
}

void reservationReleaseSubmitShutdownRaceStaysBounded()
{
    auto owner = executor();
    const auto sink = std::make_shared<RecordingSink>();
    auto gate = std::make_shared<WaitGate>();
    for (std::size_t index{}; index < Executor::WorkerCount; ++index) {
        require(
            owner->trySubmit(
                     std::make_unique<GateOperation>(gate),
                     context(3'000U + index),
                     sink)
                .hasValue(),
            "reservation-race blocker was rejected");
    }
    require(
        gate->waitUntilEntered(Executor::WorkerCount),
        "reservation-race blockers did not start");

    constexpr std::size_t ProducerCount = 8U;
    constexpr std::size_t AttemptsPerProducer = 100U;
    std::latch start{1};
    std::atomic_size_t firstAttempts{};
    std::atomic_size_t reservationSuccesses{};
    std::atomic_size_t reservationFailures{};
    std::atomic_size_t submissionSuccesses{};
    std::atomic_size_t submissionFailures{};
    std::atomic_size_t explicitReleases{};
    std::atomic_size_t implicitReleases{};
    std::atomic_size_t taskExecutions{};
    std::atomic_bool malformedResult{};
    std::vector<std::jthread> producers;
    producers.reserve(ProducerCount);

    for (std::size_t producer{}; producer < ProducerCount; ++producer) {
        producers.emplace_back([&, producer](std::stop_token) {
            start.wait();
            for (std::size_t attempt{};
                 attempt < AttemptsPerProducer;
                 ++attempt) {
                auto reserved = owner->tryReservePostDelivery();
                if (!reserved) {
                    const auto& code = reserved.error().code;
                    if (code != Domain::ErrorCodes::LimitExceeded &&
                        code != Domain::ErrorCodes::TransportClosed) {
                        malformedResult.store(true, std::memory_order_release);
                    }
                    reservationFailures.fetch_add(
                        1U, std::memory_order_relaxed);
                } else {
                    reservationSuccesses.fetch_add(
                        1U, std::memory_order_relaxed);
                    auto reservation = std::move(reserved).value();
                    switch ((producer + attempt) % 3U) {
                    case 0U: {
                        const auto sequence =
                            3'100U + producer * AttemptsPerProducer + attempt;
                        auto submitted = std::move(reservation).trySubmit(
                            std::make_unique<ImmediateOperation>(
                                &taskExecutions),
                            context(sequence),
                            sink);
                        if (submitted) {
                            submissionSuccesses.fetch_add(
                                1U, std::memory_order_relaxed);
                        } else {
                            if (submitted.error().code !=
                                Domain::ErrorCodes::TransportClosed) {
                                malformedResult.store(
                                    true, std::memory_order_release);
                            }
                            submissionFailures.fetch_add(
                                1U, std::memory_order_relaxed);
                        }
                        break;
                    }
                    case 1U:
                        reservation.release();
                        explicitReleases.fetch_add(
                            1U, std::memory_order_relaxed);
                        break;
                    default:
                        implicitReleases.fetch_add(
                            1U, std::memory_order_relaxed);
                        break;
                    }
                }
                if (attempt == 0U) {
                    firstAttempts.fetch_add(1U, std::memory_order_release);
                    firstAttempts.notify_all();
                }
            }
        });
    }

    std::jthread shutdownThread{[&](std::stop_token) {
        start.wait();
        auto observed = firstAttempts.load(std::memory_order_acquire);
        while (observed != ProducerCount) {
            firstAttempts.wait(observed, std::memory_order_acquire);
            observed = firstAttempts.load(std::memory_order_acquire);
        }
        owner->shutdown();
    }};

    start.count_down();
    producers.clear();
    shutdownThread.join();

    const auto reservedCount =
        reservationSuccesses.load(std::memory_order_acquire);
    const auto reservationFailureCount =
        reservationFailures.load(std::memory_order_acquire);
    const auto submittedCount =
        submissionSuccesses.load(std::memory_order_acquire);
    const auto submissionFailureCount =
        submissionFailures.load(std::memory_order_acquire);
    const auto explicitlyReleasedCount =
        explicitReleases.load(std::memory_order_acquire);
    const auto implicitlyReleasedCount =
        implicitReleases.load(std::memory_order_acquire);

    require(
        reservedCount + reservationFailureCount ==
            ProducerCount * AttemptsPerProducer,
        "reservation race lost an admission result");
    require(reservedCount > 0U, "reservation race reserved no capacity");
    require(
        reservedCount == submittedCount + submissionFailureCount +
                             explicitlyReleasedCount + implicitlyReleasedCount,
        "reservation race lost a token disposition");
    require(submittedCount > 0U, "reservation race submitted no work");
    require(
        !malformedResult.load(std::memory_order_acquire),
        "reservation race returned an unexpected error");
    require(
        sink->waitFor(Executor::WorkerCount + submittedCount),
        "reservation race lost an accepted completion");
    require(
        sink->records().size() == Executor::WorkerCount + submittedCount,
        "reservation race completion count differed from admission");
    require(
        taskExecutions.load(std::memory_order_acquire) == 0U,
        "shutdown executed queued reserved application work");
    require(owner->pendingCount() == 0U, "reservation race retained pending work");
    require(owner->reservationCount() == 0U, "reservation race leaked capacity");
    require(owner->activeCount() == 0U, "reservation race retained active work");
}

void externalShutdownJoinsEveryWorkerBeforeReentrantDrainEdge()
{
    auto owner = executor();
    const auto sink = std::make_shared<RecordingSink>();
    auto gate = std::make_shared<ManualGate>();
    auto lifecycle = std::make_shared<WorkerLifecycleProbe>();
    auto observer = std::make_shared<RecordingExecutorDrainObserver>(
        owner.get(), true, lifecycle);
    require(
        owner->bindShutdownDrainObserver(observer).hasValue(),
        "worker lifecycle drain observer binding failed");

    for (std::size_t index{}; index < Executor::WorkerCount; ++index) {
        require(
            owner->trySubmit(
                     std::make_unique<WorkerLifecycleOperation>(
                         gate, lifecycle),
                     context(3'900U + index),
                     sink)
                .hasValue(),
            "worker lifecycle task was rejected");
    }
    const auto allEntered =
        lifecycle->waitUntilEntered(Executor::WorkerCount);
    if (!allEntered) {
        gate->release();
        owner->shutdown();
    }
    require(allEntered, "not every handler worker entered lifecycle work");

    const auto shutdownCaller = std::this_thread::get_id();
    gate->release();
    owner->shutdown();

    require(
        observer->waitFor(1U),
        "joined worker shutdown emitted no drain edge");
    require(
        lifecycle->exitedCount() == Executor::WorkerCount,
        "drain edge preceded a handler worker thread exit");
    require(
        observer->callbackFollowedWorkerExits(),
        "drain callback ran before all joined thread-local owners settled");
    require(
        observer->callbackWasOffWorkers(),
        "drain callback ran on a handler worker");
    require(
        observer->callbackThread() == shutdownCaller,
        "external shutdown did not retain drain callback authority");
    require(
        observer->reentrantSnapshotWasDrained(),
        "drain callback could not reenter shutdown after exact joining");
    require(
        observer->notificationCount() == 1U,
        "joined worker shutdown repeated its drain edge");
}

void idleShutdownEmitsOneExactReentrantDrainEdge()
{
    auto owner = executor();
    auto invalidBinding = owner->bindShutdownDrainObserver(
        std::weak_ptr<DrainObserver>{});
    require(!invalidBinding, "expired drain observer was accepted");
    require(
        invalidBinding.error().code == Domain::ErrorCodes::InvalidRequest,
        "expired drain observer returned wrong error");

    auto observer =
        std::make_shared<RecordingExecutorDrainObserver>(owner.get());
    require(
        owner->bindShutdownDrainObserver(observer).hasValue(),
        "live drain observer binding failed");
    auto duplicate = owner->bindShutdownDrainObserver(observer);
    require(!duplicate, "duplicate drain observer binding was accepted");
    require(
        duplicate.error().code == Domain::ErrorCodes::Conflict,
        "duplicate drain observer binding returned wrong error");
    require(!owner->fullyDrained(), "running idle executor reported drained");
    require(
        observer->notificationCount() == 0U,
        "drain observer fired before shutdown");

    owner->beginShutdown();
    require(
        observer->notificationCount() == 0U,
        "beginShutdown published before worker handles were joined");
    require(
        !owner->fullyDrained(),
        "beginShutdown reported drainage before worker handles were joined");
    owner->shutdown();
    require(
        observer->waitFor(1U),
        "idle shutdown did not emit its drain edge");
    require(owner->fullyDrained(), "idle shutdown did not become fully drained");
    require(
        observer->reentrantSnapshotWasDrained(),
        "drain callback could not reenter the exact drained snapshot");
    require(
        observer->notificationCount() == 1U,
        "idle shutdown emitted more than one drain edge");

    owner->beginShutdown();
    owner->shutdown();
    require(
        observer->notificationCount() == 1U,
        "repeated shutdown repeated the drain edge");
    auto lateBinding = owner->bindShutdownDrainObserver(observer);
    require(!lateBinding, "shutdown accepted a late drain observer");
    require(
        lateBinding.error().code == Domain::ErrorCodes::TransportClosed,
        "late drain observer returned wrong error");
}

void liveDrainObserverRemainsProcessOwnedThroughItsExactEdge()
{
    auto owner = executor();
    auto notifications = std::make_shared<std::atomic_size_t>();
    auto observer =
        std::make_shared<ExternallyCountedDrainObserver>(notifications);
    std::weak_ptr<DrainObserver> retained = observer;
    require(
        owner->bindShutdownDrainObserver(observer).hasValue(),
        "process-owned drain observer binding failed");

    require(
        observer.use_count() == 1,
        "executor formed a strong ownership cycle with its drain observer");

    owner->shutdown();
    require(
        notifications->load(std::memory_order_acquire) == 1U,
        "process-owned observer missed its exact drain edge");
    observer.reset();
    require(
        retained.expired(),
        "executor retained the process observer beyond external ownership");
}

void bindAndExternalShutdownRaceHasOneLinearizedOutcome()
{
    constexpr std::size_t Iterations = 16U;
    for (std::size_t iteration{}; iteration < Iterations; ++iteration) {
        auto owner = executor();
        auto observer =
            std::make_shared<RecordingExecutorDrainObserver>(owner.get());
        std::latch ready{2};
        std::latch start{1};
        std::atomic_int bindOutcome{};

        std::jthread binder{[&](std::stop_token) noexcept {
            ready.count_down();
            start.wait();
            auto bound = owner->bindShutdownDrainObserver(observer);
            if (bound) {
                bindOutcome.store(1, std::memory_order_release);
                return;
            }
            if (bound.error().code == Domain::ErrorCodes::TransportClosed) {
                bindOutcome.store(2, std::memory_order_release);
                return;
            }
            bindOutcome.store(3, std::memory_order_release);
        }};
        std::jthread shutdownCaller{[&](std::stop_token) noexcept {
            ready.count_down();
            start.wait();
            owner->shutdown();
        }};

        ready.wait();
        start.count_down();
        binder.join();
        shutdownCaller.join();

        const auto outcome = bindOutcome.load(std::memory_order_acquire);
        require(
            outcome == 1 || outcome == 2,
            "bind/shutdown race produced a non-linearized result");
        require(owner->fullyDrained(), "bind/shutdown race did not join workers");
        if (outcome == 1) {
            require(
                observer->waitFor(1U),
                "winning concurrent binding missed its drain edge");
            require(
                observer->notificationCount() == 1U,
                "winning concurrent binding repeated its drain edge");
        } else {
            require(
                observer->notificationCount() == 0U,
                "rejected concurrent binding received a drain edge");
        }
    }
}

void cancelledQueuedAndActiveWorkSettlesBeforeDrainEdge()
{
    auto owner = executor();
    const auto sink = std::make_shared<RecordingSink>();
    auto gate = std::make_shared<WaitGate>();
    auto observer =
        std::make_shared<RecordingExecutorDrainObserver>(owner.get());
    require(
        owner->bindShutdownDrainObserver(observer).hasValue(),
        "cancellation drain observer binding failed");

    for (std::size_t index{}; index < Executor::WorkerCount; ++index) {
        require(
            owner->trySubmit(
                     std::make_unique<GateOperation>(gate),
                     context(4'000U + index),
                     sink)
                .hasValue(),
            "cancellation active task was rejected");
    }
    const auto allEntered = gate->waitUntilEntered(Executor::WorkerCount);
    if (!allEntered) {
        gate->release();
        owner->shutdown();
    }
    require(allEntered, "cancellation active tasks did not all start");

    constexpr std::size_t QueuedCount = 3U;
    for (std::size_t index{}; index < QueuedCount; ++index) {
        require(
            owner->trySubmit(
                     std::make_unique<ImmediateOperation>(),
                     context(4'100U + index),
                     sink)
                .hasValue(),
            "cancellation queued task was rejected");
    }

    owner->beginShutdown();
    owner->shutdown();
    require(
        sink->waitFor(Executor::WorkerCount + QueuedCount),
        "shutdown drain edge preceded accepted cancellation delivery");
    require(
        observer->waitFor(1U),
        "cancelled accepted work did not emit a drain edge");
    require(owner->fullyDrained(), "cancelled work retained executor ownership");
    require(
        observer->notificationCount() == 1U,
        "cancelled work emitted more than one drain edge");
    for (const auto& record : sink->records()) {
        require(!record.success, "shutdown cancellation succeeded unexpectedly");
        require(
            record.errorCode == Domain::ErrorCodes::Cancelled,
            "shutdown drain retained a non-cancellation completion");
    }
}

void lastWorkerAndLastReservationBothOwnTheExactDrainEdge()
{
    {
        auto owner = executor();
        const auto sink = std::make_shared<RecordingSink>();
        auto gate = std::make_shared<ManualGate>();
        auto observer =
            std::make_shared<RecordingExecutorDrainObserver>(owner.get());
        require(
            owner->bindShutdownDrainObserver(observer).hasValue(),
            "worker-last drain observer binding failed");
        auto reserved = owner->tryReservePostDelivery();
        require(reserved.hasValue(), "worker-last reservation was rejected");
        auto reservation = std::move(reserved).value();
        require(
            owner->trySubmit(
                     std::make_unique<ManualGateOperation>(gate),
                     context(4'200U),
                     sink)
                .hasValue(),
            "worker-last task was rejected");
        const auto entered = gate->waitUntilEntered();
        if (!entered) {
            gate->release();
            reservation.release();
            owner->shutdown();
        }
        require(entered, "worker-last task did not start");

        owner->beginShutdown();
        reservation.release();
        const auto withheldForWorker =
            observer->notificationCount() == 0U && !owner->fullyDrained();
        gate->release();
        owner->shutdown();
        require(
            withheldForWorker,
            "reservation release emitted while an active worker remained");
        require(
            observer->waitFor(1U),
            "joining the last worker did not emit the exact drain edge");
        require(owner->fullyDrained(), "worker-last executor was not drained");
        require(
            observer->notificationCount() == 1U,
            "worker-last shutdown repeated the drain edge");
    }

    {
        auto owner = executor();
        auto observer =
            std::make_shared<RecordingExecutorDrainObserver>(owner.get());
        require(
            owner->bindShutdownDrainObserver(observer).hasValue(),
            "reservation-last drain observer binding failed");
        auto reserved = owner->tryReservePostDelivery();
        require(
            reserved.hasValue(),
            "reservation-last capacity reservation was rejected");
        auto reservation = std::move(reserved).value();

        owner->beginShutdown();
        owner->shutdown();
        require(
            !owner->fullyDrained(),
            "live reservation was omitted from the drained predicate");
        require(
            observer->notificationCount() == 0U,
            "joined workers emitted while a live reservation remained");

        reservation.release();
        require(
            observer->waitFor(1U),
            "last reservation did not emit the exact drain edge");
        require(
            owner->fullyDrained(),
            "reservation-last executor was not fully drained");
        reservation.release();
        require(
            observer->notificationCount() == 1U,
            "repeated reservation release repeated the drain edge");
    }
}

void concurrentLastWorkerAndReservationReleaseEmitOneDrainEdge()
{
    auto owner = executor();
    const auto sink = std::make_shared<RecordingSink>();
    auto gate = std::make_shared<ManualGate>();
    auto observer =
        std::make_shared<RecordingExecutorDrainObserver>(owner.get());
    require(
        owner->bindShutdownDrainObserver(observer).hasValue(),
        "concurrent drain observer binding failed");
    auto reserved = owner->tryReservePostDelivery();
    require(
        reserved.hasValue(),
        "concurrent drain capacity reservation was rejected");
    auto reservation = std::move(reserved).value();
    bool allSubmitted{true};
    for (std::size_t index{}; index < Executor::WorkerCount; ++index) {
        auto submitted = owner->trySubmit(
            std::make_unique<ManualGateOperation>(gate),
            context(4'300U + index),
            sink);
        if (!submitted) {
            allSubmitted = false;
            break;
        }
    }
    if (!allSubmitted) {
        gate->release();
        reservation.release();
        owner->shutdown();
    }
    require(allSubmitted, "concurrent drain worker task was rejected");
    const auto entered = gate->waitUntilEntered(Executor::WorkerCount);
    if (!entered) {
        gate->release();
        reservation.release();
        owner->shutdown();
    }
    require(entered, "concurrent drain worker task did not start");

    owner->beginShutdown();
    const auto withheldBeforeRace =
        observer->notificationCount() == 0U && !owner->fullyDrained();
    std::latch ready{3};
    std::latch start{1};
    std::atomic_bool workerReleaseReturned{};
    std::atomic_bool reservationReleaseReturned{};
    std::atomic_bool shutdownReturned{};
    std::jthread workerRelease;
    std::jthread reservationRelease;
    std::jthread shutdownCaller;
    try {
        workerRelease = std::jthread{[&](std::stop_token) noexcept {
            ready.count_down();
            start.wait();
            gate->release();
            workerReleaseReturned.store(true, std::memory_order_release);
        }};
        reservationRelease = std::jthread{[&](std::stop_token) noexcept {
            ready.count_down();
            start.wait();
            reservation.release();
            reservationReleaseReturned.store(true, std::memory_order_release);
        }};
        shutdownCaller = std::jthread{[&](std::stop_token) noexcept {
            ready.count_down();
            start.wait();
            owner->shutdown();
            shutdownReturned.store(true, std::memory_order_release);
        }};
    } catch (...) {
        start.count_down();
        gate->release();
        if (workerRelease.joinable()) {
            workerRelease.join();
        }
        if (reservationRelease.joinable()) {
            reservationRelease.join();
        }
        if (shutdownCaller.joinable()) {
            shutdownCaller.join();
        }
        reservation.release();
        owner->shutdown();
        throw;
    }

    ready.wait();
    start.count_down();
    workerRelease.join();
    reservationRelease.join();
    shutdownCaller.join();

    const auto notified = observer->waitFor(1U);
    const auto workerReleaseCompleted =
        workerReleaseReturned.load(std::memory_order_acquire);
    const auto reservationReleaseCompleted =
        reservationReleaseReturned.load(std::memory_order_acquire);
    const auto shutdownCompleted =
        shutdownReturned.load(std::memory_order_acquire);
    const auto drainedAfterBoth = owner->fullyDrained();
    const auto exactNotificationCount = observer->notificationCount();
    const auto callbackObservedExactDrain =
        observer->reentrantSnapshotWasDrained();

    require(
        withheldBeforeRace,
        "concurrent drain edge fired before either final owner settled");
    require(
        workerReleaseCompleted && reservationReleaseCompleted &&
            shutdownCompleted,
        "concurrent drain release and join threads did not all complete");
    require(notified, "concurrent final owners emitted no drain edge");
    require(drainedAfterBoth, "concurrent final owners did not fully drain");
    require(
        callbackObservedExactDrain,
        "concurrent drain callback fired before both owners settled");
    require(
        exactNotificationCount == 1U,
        "concurrent final owners emitted more than one drain edge");
}

void reservationOutlivingOuterExecutorRetainsExactDrainOwnership()
{
    Reservation survivor;
    auto observer = std::make_shared<RecordingExecutorDrainObserver>();
    {
        auto owner = executor();
        require(
            owner->bindShutdownDrainObserver(observer).hasValue(),
            "outliving-reservation drain observer binding failed");
        auto reserved = owner->tryReservePostDelivery();
        require(
            reserved.hasValue(),
            "outliving-reservation capacity reservation was rejected");
        survivor = std::move(reserved).value();

        owner.reset();
        require(
            observer->notificationCount() == 0U,
            "outer executor destruction ignored its live reservation");
    }

    survivor.release();
    require(
        observer->waitFor(1U),
        "outliving reservation did not complete exact drainage");
    survivor.release();
    require(
        observer->notificationCount() == 1U,
        "outliving reservation repeated its drain edge");
}

class LifetimeSink final : public Sink {
public:
    LifetimeSink(
        std::atomic_size_t& destructions,
        std::atomic_size_t& posts) noexcept
        : destructions_{destructions}, posts_{posts}
    {
    }

    ~LifetimeSink() noexcept override
    {
        destructions_.fetch_add(1U, std::memory_order_release);
    }

    [[nodiscard]] bool tryPost(Completion) noexcept override
    {
        posts_.fetch_add(1U, std::memory_order_release);
        return true;
    }

private:
    std::atomic_size_t& destructions_;
    std::atomic_size_t& posts_;
};

class ReentrantShutdownSink final : public Sink {
public:
    explicit ReentrantShutdownSink(Executor& owner) noexcept : owner_{owner} {}

    [[nodiscard]] bool tryPost(Completion) noexcept override
    {
        owner_.shutdown();
        try {
            {
                const std::lock_guard lock{mutex_};
                posted_ = true;
            }
            changed_.notify_all();
        } catch (...) {
            std::terminate();
        }
        return true;
    }

    [[nodiscard]] bool wait() const
    {
        std::unique_lock lock{mutex_};
        return changed_.wait_for(lock, 7s, [this] { return posted_; });
    }

private:
    Executor& owner_;
    mutable std::mutex mutex_;
    mutable std::condition_variable changed_;
    bool posted_{};
};

class ReleasingExecutorSink final : public Sink {
public:
    explicit ReleasingExecutorSink(
        std::unique_ptr<Executor>& owner) noexcept
        : owner_{owner}
    {
    }

    [[nodiscard]] bool tryPost(Completion) noexcept override
    {
        owner_.reset();
        try {
            {
                const std::lock_guard lock{mutex_};
                released_ = true;
            }
            changed_.notify_all();
        } catch (...) {
            std::terminate();
        }
        return true;
    }

    [[nodiscard]] bool wait() const
    {
        std::unique_lock lock{mutex_};
        return changed_.wait_for(lock, 7s, [this] { return released_; });
    }

private:
    std::unique_ptr<Executor>& owner_;
    mutable std::mutex mutex_;
    mutable std::condition_variable changed_;
    bool released_{};
};

class OneShotReleasingExecutorSink final : public Sink {
public:
    explicit OneShotReleasingExecutorSink(
        std::unique_ptr<Executor>& owner) noexcept
        : owner_{owner}
    {
    }

    [[nodiscard]] bool tryPost(Completion) noexcept override
    {
        try {
            const bool ownsRelease =
                !releaseClaimed_.exchange(true, std::memory_order_acq_rel);
            if (ownsRelease) {
                owner_.reset();
            }
            {
                const std::lock_guard lock{mutex_};
                ++postCount_;
                releaseReturned_ = releaseReturned_ || ownsRelease;
            }
            changed_.notify_all();
            return true;
        } catch (...) {
            std::terminate();
        }
    }

    [[nodiscard]] bool waitFor(
        const std::size_t count,
        const std::chrono::milliseconds timeout = 7s) const
    {
        std::unique_lock lock{mutex_};
        return changed_.wait_for(lock, timeout, [this, count] {
            return releaseReturned_ && postCount_ >= count;
        });
    }

private:
    std::unique_ptr<Executor>& owner_;
    std::atomic_bool releaseClaimed_{};
    mutable std::mutex mutex_;
    mutable std::condition_variable changed_;
    std::size_t postCount_{};
    bool releaseReturned_{};
};

class DestructionProbe final {
public:
    void mark() noexcept
    {
        try {
            {
                const std::lock_guard lock{mutex_};
                destroyed_ = true;
            }
            changed_.notify_all();
        } catch (...) {
            std::terminate();
        }
    }

    [[nodiscard]] bool wait() const
    {
        std::unique_lock lock{mutex_};
        return changed_.wait_for(lock, 7s, [this] { return destroyed_; });
    }

private:
    mutable std::mutex mutex_;
    mutable std::condition_variable changed_;
    bool destroyed_{};
};

class DestructionProbeOperation final : public Operation {
public:
    explicit DestructionProbeOperation(
        std::shared_ptr<DestructionProbe> probe) noexcept
        : probe_{std::move(probe)}
    {
    }

    ~DestructionProbeOperation() noexcept override { probe_->mark(); }

    [[nodiscard]] CompletionKind completionKind() const noexcept override
    {
        return CompletionKind::PostDelivery;
    }

    [[nodiscard]] Completion execute(
        const Domain::OperationContext&) override
    {
        return postDeliverySuccess();
    }

private:
    std::shared_ptr<DestructionProbe> probe_;
};

void sinksAreWeakAndWorkerShutdownIsReentrantSafe()
{
    auto gate = std::make_shared<WaitGate>();
    auto owner = executor();
    std::atomic_size_t destructions{};
    std::atomic_size_t posts{};
    auto lifetimeSink =
        std::make_shared<LifetimeSink>(destructions, posts);
    std::weak_ptr<Sink> weakLifetime = lifetimeSink;
    require(
        owner->trySubmit(
                 std::make_unique<GateOperation>(gate),
                 context(2'000U),
                 weakLifetime)
            .hasValue(),
        "weak-lifetime task was rejected");
    require(gate->waitUntilEntered(1U), "weak-lifetime task did not start");
    lifetimeSink.reset();
    require(weakLifetime.expired(), "executor retained completion sink strongly");
    require(destructions.load() == 1U, "completion sink was not destroyed");
    gate->release();
    owner->shutdown();
    require(posts.load() == 0U, "expired completion sink was invoked");

    auto reentrantOwner = executor();
    auto reentrantObserver =
        std::make_shared<RecordingExecutorDrainObserver>(
            reentrantOwner.get());
    require(
        reentrantOwner->bindShutdownDrainObserver(reentrantObserver)
            .hasValue(),
        "worker-context process drain observer binding failed");
    const auto reentrant =
        std::make_shared<ReentrantShutdownSink>(*reentrantOwner);
    require(
        reentrantOwner
            ->trySubmit(
                std::make_unique<ImmediateOperation>(),
                context(2'001U),
                reentrant)
            .hasValue(),
        "reentrant shutdown task was rejected");
    require(reentrant->wait(), "worker-context shutdown did not return");
    require(
        reentrantOwner->isShuttingDown(),
        "worker-context shutdown did not begin shutdown");
    require(
        !reentrantOwner->fullyDrained(),
        "worker-context shutdown bypassed the external join owner");
    require(
        reentrantObserver->notificationCount() == 0U,
        "worker-context shutdown published the process drain edge");

    auto rejected = reentrantOwner->trySubmit(
        std::make_unique<ImmediateOperation>(),
        context(2'002U),
        reentrant);
    require(!rejected, "reentrant shutdown accepted another task");
    require(
        rejected.error().code == Domain::ErrorCodes::TransportClosed,
        "reentrant shutdown rejection returned wrong error");
    reentrantOwner->shutdown();
    require(
        reentrantOwner->activeCount() == 0U,
        "external shutdown did not join reentrant worker");
    require(
        reentrantObserver->waitFor(1U),
        "external finalizer did not publish worker-context drainage");
    require(
        reentrantOwner->fullyDrained(),
        "external finalizer did not establish exact joined drainage");
    require(
        reentrantObserver->notificationCount() == 1U,
        "worker-context process shutdown repeated its drain edge");

    std::unique_ptr<Executor> releasingOwner = executor();
    const auto releasing =
        std::make_shared<ReleasingExecutorSink>(releasingOwner);
    const auto destructionProbe = std::make_shared<DestructionProbe>();
    require(
        releasingOwner
            ->trySubmit(
                std::make_unique<DestructionProbeOperation>(destructionProbe),
                context(2'003U),
                releasing)
            .hasValue(),
        "self-releasing executor task was rejected");
    require(releasing->wait(), "self-releasing executor callback did not return");
    require(
        releasingOwner == nullptr,
        "completion sink did not release the outer executor");
    require(
        destructionProbe->wait(),
        "owned task did not survive outer executor release safely");
}

void boundWorkerSelfReleaseSettlesHandlesWithoutFalseDrainEdge()
{
    std::unique_ptr<Executor> owner = executor();
    auto observer = std::make_shared<RecordingExecutorDrainObserver>();
    require(
        owner->bindShutdownDrainObserver(observer).hasValue(),
        "bound self-release drain observer binding failed");

    auto lifecycle = std::make_shared<WorkerLifecycleProbe>();
    auto gate = std::make_shared<ManualGate>();
    const auto releasing =
        std::make_shared<OneShotReleasingExecutorSink>(owner);
    for (std::size_t index{}; index < Executor::WorkerCount; ++index) {
        require(
            owner->trySubmit(
                     std::make_unique<WorkerLifecycleOperation>(
                         gate, lifecycle),
                     context(4'400U + index),
                     releasing)
                .hasValue(),
            "bound self-release lifecycle task was rejected");
    }

    const auto allEntered =
        lifecycle->waitUntilEntered(Executor::WorkerCount);
    if (!allEntered) {
        gate->release();
        owner->shutdown();
    }
    require(
        allEntered,
        "bound self-release did not occupy every handler worker");

    gate->release();
    const auto allCallbacksReturned =
        releasing->waitFor(Executor::WorkerCount);
    if (!allCallbacksReturned && owner != nullptr) {
        owner->shutdown();
    }
    require(
        allCallbacksReturned,
        "bound worker-context executor release did not return");
    require(
        owner == nullptr,
        "bound completion sink retained the outer executor");
    require(
        lifecycle->waitUntilExited(Executor::WorkerCount, 7s),
        "bound self-release did not settle every worker thread handle");
    require(
        observer->notificationCount() == 0U,
        "bound self-release falsely published an exact joined drain edge");
}

} // namespace

int main()
{
    try {
        exactBoundsAndImmediateAdmissionValidation();
        fourWorkersAndEightPendingTasksSaturateImmediately();
        reservationsShareEightPendingPositionsAndConvertOnce();
        shutdownInvalidatesReservationsAndRejectsNewOnes();
        unusedReservationSafelyOutlivesOuterExecutor();
        pendingTasksDequeueFifoWhileOtherWorkersRemainBusy();
        taskExceptionsAndKindMismatchBecomeTypedCompletions();
        callerCancellationAndDeadlineReachRunningOperations();
        shutdownCancelsQueuedAndActiveWorkThenJoins();
        enqueueShutdownRaceRetainsEveryAcceptedCompletion();
        reservationReleaseSubmitShutdownRaceStaysBounded();
        externalShutdownJoinsEveryWorkerBeforeReentrantDrainEdge();
        idleShutdownEmitsOneExactReentrantDrainEdge();
        liveDrainObserverRemainsProcessOwnedThroughItsExactEdge();
        bindAndExternalShutdownRaceHasOneLinearizedOutcome();
        cancelledQueuedAndActiveWorkSettlesBeforeDrainEdge();
        lastWorkerAndLastReservationBothOwnTheExactDrainEdge();
        concurrentLastWorkerAndReservationReleaseEmitOneDrainEdge();
        reservationOutlivingOuterExecutorRetainsExactDrainOwnership();
        sinksAreWeakAndWorkerShutdownIsReentrantSafe();
        boundWorkerSelfReleaseSettlesHandlesWithoutFalseDrainEdge();
        std::cout << "Windows dashboard handler executor tests passed: "
                  << assertionCount.load() << " assertions\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Windows dashboard handler executor tests failed after "
                  << assertionCount.load() << " assertions: " << error.what()
                  << '\n';
        return 1;
    } catch (...) {
        std::cerr << "Windows dashboard handler executor tests failed after "
                  << assertionCount.load()
                  << " assertions: unknown exception\n";
        return 1;
    }
}
