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
using Executor = Windows::WindowsDashboardHandlerExecutor;
using Operation = Windows::IDashboardHandlerOperation;
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
static_assert(!std::is_copy_constructible_v<Completion>);
static_assert(!std::is_copy_assignable_v<Completion>);
static_assert(std::is_nothrow_move_constructible_v<Completion>);
static_assert(std::is_nothrow_move_assignable_v<Completion>);
static_assert(!std::is_copy_constructible_v<Executor>);
static_assert(!std::is_move_constructible_v<Executor>);
static_assert(noexcept(std::declval<Sink&>().tryPost(
    std::declval<Completion>())));
static_assert(noexcept(std::declval<Executor&>().trySubmit(
    std::declval<std::unique_ptr<Operation>>(),
    std::declval<Domain::OperationContext>(),
    std::declval<std::weak_ptr<Sink>>())));

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

} // namespace

int main()
{
    try {
        exactBoundsAndImmediateAdmissionValidation();
        fourWorkersAndEightPendingTasksSaturateImmediately();
        pendingTasksDequeueFifoWhileOtherWorkersRemainBusy();
        taskExceptionsAndKindMismatchBecomeTypedCompletions();
        callerCancellationAndDeadlineReachRunningOperations();
        shutdownCancelsQueuedAndActiveWorkThenJoins();
        enqueueShutdownRaceRetainsEveryAcceptedCompletion();
        sinksAreWeakAndWorkerShutdownIsReentrantSafe();
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
