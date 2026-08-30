#include "Infrastructure/Windows/Detail/ManagerStartupComWorker.h"
#include "Infrastructure/TestSupport.h"

#include <Windows.h>
#include <objbase.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace ForgeConductor::Tests {
namespace {

namespace Detail = Infrastructure::Windows::Detail;

using namespace std::chrono_literals;

using Handler = Detail::IManagerStartupComHandler;
using AdmissionGate = Detail::IManagerStartupComWorkerAdmissionGate;
using Kind = Detail::ManagerStartupComOperationKind;
using Request = Detail::ManagerStartupComRequest;
using Response = Detail::ManagerStartupComResponse;
using Result = Detail::ManagerStartupComResult;
using Worker = Detail::ManagerStartupComWorker;

static_assert(std::is_abstract_v<Handler>);
static_assert(std::is_final_v<Worker>);
static_assert(!std::is_copy_constructible_v<Worker>);
static_assert(!std::is_move_constructible_v<Worker>);
static_assert(Worker::MaximumActiveOperationCount == 1U);
static_assert(Worker::MaximumQueuedOperationCount == 1U);
static_assert(Worker::WorkerStartupTimeout == 5s);
static_assert(Worker::CancellationDrainTimeout == 5s);
static_assert(Worker::ShutdownDrainTimeout == 5s);

[[nodiscard]] Domain::PathText path(const std::string_view value)
{
    return take(Domain::PathText::create(value));
}

[[nodiscard]] Domain::OperationId operationId(
    const std::string_view value)
{
    return parse<Domain::OperationId>(value);
}

[[nodiscard]] Domain::ManagerStartupDefinition startupDefinition()
{
    return Domain::ManagerStartupDefinition{
        path("C:\\Forge\\ForgeConductor.Manager.exe"),
        path("C:\\ForgeHome")};
}

[[nodiscard]] Request request(
    const Kind kind,
    const std::string_view identifier,
    const Domain::MonotonicTimePoint deadline =
        std::chrono::steady_clock::now() + 5min,
    const std::stop_token cancellation = {})
{
    return Request{
        kind,
        startupDefinition(),
        "worker_test",
        true,
        Domain::OperationContext{
            operationId(identifier),
            deadline,
            cancellation,
            parse<Domain::CorrelationId>("manager-startup-com-worker-test")}};
}

class ControlledHandler final : public Handler {
public:
    struct Observation final {
        Kind kind;
        Domain::OperationId operationId;
        Domain::ManagerStartupDefinition expected;
        std::string purposeSuffix;
        bool enabled{};
        DWORD nativeThreadId{};
        HRESULT apartmentResult{E_UNEXPECTED};
        APTTYPE apartmentType{APTTYPE_CURRENT};
        APTTYPEQUALIFIER apartmentQualifier{APTTYPEQUALIFIER_NONE};
        bool cancellationRequestedAtReturn{};
    };

    void block(const Domain::OperationId& identifier)
    {
        const std::lock_guard lock{mutex_};
        blockedOperation_ = identifier.value();
        released_ = false;
    }

    void release() noexcept
    {
        try {
            const std::lock_guard lock{mutex_};
            released_ = true;
            changed_.notify_all();
        } catch (...) {
            std::terminate();
        }
    }

    [[nodiscard]] bool waitForEntered(
        const std::size_t count,
        const std::chrono::milliseconds timeout)
    {
        std::unique_lock lock{mutex_};
        return changed_.wait_for(
            lock,
            timeout,
            [this, count]() noexcept {
                return observations_.size() >= count;
            });
    }

    [[nodiscard]] std::size_t enteredCount() const noexcept
    {
        try {
            const std::lock_guard lock{mutex_};
            return observations_.size();
        } catch (...) {
            return 0U;
        }
    }

    [[nodiscard]] Observation observation(const std::size_t index) const
    {
        const std::lock_guard lock{mutex_};
        if (index >= observations_.size()) {
            throw TestFailure{"COM worker observation index was out of range"};
        }
        return observations_[index];
    }

    [[nodiscard]] Result handle(
        const Request& value) noexcept override
    {
        try {
            Observation observation{
                value.kind,
                value.context.operationId,
                value.expected,
                value.purposeSuffix,
                value.enabled,
                GetCurrentThreadId()};
            observation.apartmentResult = CoGetApartmentType(
                &observation.apartmentType,
                &observation.apartmentQualifier);

            {
                std::unique_lock lock{mutex_};
                const std::size_t observationIndex = observations_.size();
                observations_.push_back(observation);
                changed_.notify_all();
                if (blockedOperation_.has_value() &&
                    blockedOperation_.value() ==
                        value.context.operationId.value()) {
                    changed_.wait(lock, [this]() noexcept {
                        return released_;
                    });
                }
                observations_[observationIndex]
                    .cancellationRequestedAtReturn =
                    value.context.isCancellationRequested();
            }

            if (value.kind == Kind::Inspect) {
                Domain::ManagerStartupStatus status;
                status.registrationIdentity =
                    value.context.operationId.value();
                return Result::success(Response{std::move(status)});
            }

            Domain::ManagerStartupOutcome outcome;
            outcome.status.registrationIdentity =
                value.context.operationId.value();
            outcome.changed = true;
            return Result::success(Response{std::move(outcome)});
        } catch (...) {
            std::terminate();
        }
    }

private:
    mutable std::mutex mutex_;
    std::condition_variable changed_;
    std::vector<Observation> observations_;
    std::optional<std::string> blockedOperation_;
    bool released_{};
};

class BlockingAdmissionGate final : public AdmissionGate {
public:
    void afterAdmissionBeforeDispatch(
        const Domain::OperationId&) noexcept override
    {
        try {
            std::unique_lock lock{mutex_};
            entered_ = true;
            changed_.notify_all();
            changed_.wait(lock, [this]() noexcept { return released_; });
        } catch (...) {
            std::terminate();
        }
    }

    [[nodiscard]] bool waitForEntered(
        const std::chrono::milliseconds timeout)
    {
        std::unique_lock lock{mutex_};
        return changed_.wait_for(
            lock, timeout, [this]() noexcept { return entered_; });
    }

    void release() noexcept
    {
        try {
            const std::lock_guard lock{mutex_};
            released_ = true;
            changed_.notify_all();
        } catch (...) {
            std::terminate();
        }
    }

private:
    std::mutex mutex_;
    std::condition_variable changed_;
    bool entered_{};
    bool released_{};
};

[[nodiscard]] std::unique_ptr<Worker> createWorker(
    const std::shared_ptr<Handler>& handler)
{
    return take(Worker::create(handler));
}

[[nodiscard]] std::future<Result> executeAsync(
    Worker& worker,
    Request value)
{
    return std::async(
        std::launch::async,
        [&worker, value = std::move(value)]() mutable {
            return worker.execute(std::move(value));
        });
}

[[nodiscard]] bool waitForQueue(
    const Worker& worker,
    const std::size_t queuedCount,
    const std::chrono::milliseconds timeout)
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    do {
        if (worker.snapshot().queuedOperationCount == queuedCount) {
            return true;
        }
        std::this_thread::sleep_for(1ms);
    } while (std::chrono::steady_clock::now() < deadline);
    return worker.snapshot().queuedOperationCount == queuedCount;
}

void requireSucceeded(const Result& result, const std::string_view message)
{
    require(result.hasValue(), message);
}

void testExecutesOnDedicatedMtaAndPreservesPayload()
{
    auto handler = std::make_shared<ControlledHandler>();
    auto worker = createWorker(handler);
    const auto value = request(
        Kind::Inspect,
        "10000000-0000-4000-8000-000000000001");
    const auto expected = value.expected;

    const auto result = worker->execute(value);
    const auto observation = handler->observation(0U);

    requireSucceeded(result, "the MTA worker did not return its handler response");
    require(
        std::holds_alternative<Domain::ManagerStartupStatus>(result.value()),
        "inspect did not retain the status response alternative");
    require(
        SUCCEEDED(observation.apartmentResult) &&
            observation.apartmentType == APTTYPE_MTA,
        "the handler did not execute in a COM MTA");
    require(
        observation.nativeThreadId != GetCurrentThreadId(),
        "the handler executed on the caller instead of the dedicated worker");
    require(
        observation.expected == expected &&
            observation.purposeSuffix == "worker_test" &&
            observation.enabled,
        "the copied operation payload changed before MTA execution");
}

void testFifoAndCapacity()
{
    const auto firstId = operationId(
        "10000000-0000-4000-8000-000000000002");
    auto handler = std::make_shared<ControlledHandler>();
    handler->block(firstId);
    auto worker = createWorker(handler);

    auto first = executeAsync(
        *worker,
        request(Kind::Register, firstId.value()));
    const bool firstEntered = handler->waitForEntered(1U, 5s);
    auto second = executeAsync(
        *worker,
        request(
            Kind::Repair,
            "10000000-0000-4000-8000-000000000003"));
    const bool secondQueued = waitForQueue(*worker, 1U, 5s);

    const auto third = worker->execute(request(
        Kind::StartNow,
        "10000000-0000-4000-8000-000000000004"));
    handler->release();
    const auto firstResult = first.get();
    const auto secondResult = second.get();

    require(firstEntered, "the first FIFO operation did not enter the handler");
    require(secondQueued, "the FIFO successor did not occupy the one queued slot");
    requireError(
        third,
        Domain::ErrorCodes::LimitExceeded,
        "a third operation was admitted beyond the active-plus-queued bound");
    requireSucceeded(firstResult, "the first FIFO operation failed");
    requireSucceeded(secondResult, "the queued FIFO operation failed");
    require(
        handler->observation(0U).operationId == firstId &&
            handler->observation(1U).operationId.value() ==
                "10000000-0000-4000-8000-000000000003",
        "the worker did not execute admitted operations in FIFO order");
}

void testRejectsDuplicateActiveAndQueuedIds()
{
    const auto firstId = operationId(
        "10000000-0000-4000-8000-000000000005");
    const auto secondId = operationId(
        "10000000-0000-4000-8000-000000000006");
    auto handler = std::make_shared<ControlledHandler>();
    handler->block(firstId);
    auto worker = createWorker(handler);

    auto first = executeAsync(
        *worker,
        request(Kind::Register, firstId.value()));
    const bool firstEntered = handler->waitForEntered(1U, 5s);
    const auto duplicateActive = worker->execute(
        request(Kind::Inspect, firstId.value()));
    auto second = executeAsync(
        *worker,
        request(Kind::Repair, secondId.value()));
    const bool secondQueued = waitForQueue(*worker, 1U, 5s);
    const auto duplicateQueued = worker->execute(
        request(Kind::Remove, secondId.value()));

    handler->release();
    const auto firstResult = first.get();
    const auto secondResult = second.get();

    require(firstEntered && secondQueued, "duplicate-ID setup did not stabilize");
    requireError(
        duplicateActive,
        Domain::ErrorCodes::Conflict,
        "a duplicate active operation identifier was accepted");
    requireError(
        duplicateQueued,
        Domain::ErrorCodes::Conflict,
        "a duplicate queued operation identifier was accepted");
    requireSucceeded(firstResult, "the active duplicate-ID owner failed");
    requireSucceeded(secondResult, "the queued duplicate-ID owner failed");
}

void testQueuedCancellationSkipsHandler()
{
    const auto firstId = operationId(
        "10000000-0000-4000-8000-000000000007");
    auto handler = std::make_shared<ControlledHandler>();
    handler->block(firstId);
    auto worker = createWorker(handler);
    std::stop_source cancellation;

    auto first = executeAsync(
        *worker,
        request(Kind::Register, firstId.value()));
    const bool firstEntered = handler->waitForEntered(1U, 5s);
    auto queued = executeAsync(
        *worker,
        request(
            Kind::SetEnabled,
            "10000000-0000-4000-8000-000000000008",
            std::chrono::steady_clock::now() + 5min,
            cancellation.get_token()));
    const bool wasQueued = waitForQueue(*worker, 1U, 5s);
    const bool requested = cancellation.request_stop();
    const auto queuedResult = queued.get();
    const auto enteredBeforeRelease = handler->enteredCount();

    handler->release();
    const auto firstResult = first.get();

    require(firstEntered && wasQueued && requested, "queued-cancellation setup failed");
    requireError(
        queuedResult,
        Domain::ErrorCodes::Cancelled,
        "a cancelled queued operation did not complete as cancelled");
    require(
        enteredBeforeRelease == 1U,
        "a cancelled queued operation reached the COM handler");
    requireSucceeded(firstResult, "the active cancellation-test owner failed");
}

void testCancellationRelayPrecedesDispatchVisibility()
{
    auto handler = std::make_shared<ControlledHandler>();
    auto gate = std::make_shared<BlockingAdmissionGate>();
    auto worker = take(Worker::createForTesting(handler, gate));
    std::stop_source cancellation;

    auto operation = executeAsync(
        *worker,
        request(
            Kind::Inspect,
            "10000000-0000-4000-8000-000000000014",
            std::chrono::steady_clock::now() + 5min,
            cancellation.get_token()));
    const bool admissionVisible = gate->waitForEntered(5s);
    auto cancellationRequest = std::async(
        std::launch::async,
        [&cancellation]() { return cancellation.request_stop(); });
    const auto cancellationVisibleDeadline =
        std::chrono::steady_clock::now() + 5s;
    while (!cancellation.stop_requested() &&
           std::chrono::steady_clock::now() <
               cancellationVisibleDeadline) {
        std::this_thread::sleep_for(1ms);
    }
    const bool cancellationVisible = cancellation.stop_requested();
    const bool relayBlockedOnAdmissionLock =
        cancellationVisible && cancellationRequest.wait_for(100ms) ==
        std::future_status::timeout;

    gate->release();
    const bool requested = cancellationRequest.get();
    const auto result = operation.get();

    require(admissionVisible, "the deterministic admission gate was not reached");
    require(
        cancellationVisible,
        "the pre-dispatch cancellation request did not become visible");
    require(
        relayBlockedOnAdmissionLock,
        "caller cancellation was not linked before the work became visible");
    require(requested, "the pre-dispatch cancellation request was not accepted");
    requireError(
        result,
        Domain::ErrorCodes::Cancelled,
        "pre-dispatch caller cancellation lost terminal ownership");
    require(
        handler->enteredCount() == 0U,
        "pre-dispatch caller cancellation reached the COM handler");
}

void testAlreadyCancelledRequestIsNeverAdmitted()
{
    auto handler = std::make_shared<ControlledHandler>();
    auto worker = createWorker(handler);
    std::stop_source cancellation;
    static_cast<void>(cancellation.request_stop());

    const auto result = worker->execute(request(
        Kind::Inspect,
        "10000000-0000-4000-8000-000000000015",
        std::chrono::steady_clock::now() + 5min,
        cancellation.get_token()));
    const auto snapshot = worker->snapshot();

    requireError(
        result,
        Domain::ErrorCodes::Cancelled,
        "an already-cancelled request was admitted");
    require(
        handler->enteredCount() == 0U &&
            snapshot.activeOperationCount == 0U &&
            snapshot.queuedOperationCount == 0U,
        "an already-cancelled request became visible to the worker");
}

void testQueuedDeadlineSkipsHandler()
{
    const auto firstId = operationId(
        "10000000-0000-4000-8000-000000000009");
    auto handler = std::make_shared<ControlledHandler>();
    handler->block(firstId);
    auto worker = createWorker(handler);

    auto first = executeAsync(
        *worker,
        request(Kind::Register, firstId.value()));
    const bool firstEntered = handler->waitForEntered(1U, 5s);
    auto queued = executeAsync(
        *worker,
        request(
            Kind::Remove,
            "10000000-0000-4000-8000-000000000010",
            std::chrono::steady_clock::now() + 1s));
    const bool wasQueued = waitForQueue(*worker, 1U, 5s);
    const auto queuedResult = queued.get();
    const auto enteredBeforeRelease = handler->enteredCount();

    handler->release();
    const auto firstResult = first.get();

    require(firstEntered && wasQueued, "queued-deadline setup failed");
    requireError(
        queuedResult,
        Domain::ErrorCodes::DeadlineExceeded,
        "an expired queued operation did not complete with its deadline error");
    require(
        enteredBeforeRelease == 1U,
        "an expired queued operation reached the COM handler");
    requireSucceeded(firstResult, "the active deadline-test owner failed");
}

void testActiveDeadlineWaitsForTerminalHandlerCompletion()
{
    const auto identifier = operationId(
        "10000000-0000-4000-8000-000000000016");
    auto handler = std::make_shared<ControlledHandler>();
    handler->block(identifier);
    auto worker = createWorker(handler);

    auto active = executeAsync(
        *worker,
        request(
            Kind::Repair,
            identifier.value(),
            std::chrono::steady_clock::now() + 200ms));
    const bool entered = handler->waitForEntered(1U, 5s);
    const bool callerStillWaiting =
        active.wait_for(500ms) == std::future_status::timeout;

    handler->release();
    const auto activeResult = active.get();
    const auto observation = handler->observation(0U);

    require(entered, "the active deadline operation did not enter the handler");
    require(
        callerStillWaiting,
        "an active deadline released caller ownership before handler completion");
    require(
        observation.cancellationRequestedAtReturn,
        "an active deadline was not relayed into the handler context");
    requireError(
        activeResult,
        Domain::ErrorCodes::DeadlineExceeded,
        "an active operation finishing after its deadline returned success");
}

void testActiveCancellationWaitsForTerminalHandlerCompletion()
{
    const auto identifier = operationId(
        "10000000-0000-4000-8000-000000000011");
    auto handler = std::make_shared<ControlledHandler>();
    handler->block(identifier);
    auto worker = createWorker(handler);

    auto active = executeAsync(
        *worker,
        request(Kind::Repair, identifier.value()));
    const bool entered = handler->waitForEntered(1U, 5s);
    worker->cancel(identifier);
    const bool callerStillWaiting =
        active.wait_for(100ms) == std::future_status::timeout;

    handler->release();
    const auto activeResult = active.get();
    const auto observation = handler->observation(0U);

    require(entered, "the active cancellation operation did not enter the handler");
    require(
        callerStillWaiting,
        "active cancellation completed before the handler released operation ownership");
    require(
        observation.cancellationRequestedAtReturn,
        "active cancellation was not relayed into the handler operation context");
    requireError(
        activeResult,
        Domain::ErrorCodes::Cancelled,
        "the terminal active response did not retain cancellation");
}

void testActiveCallerTokenCancellationDrainsAndPromotesSuccessor()
{
    const auto activeId = operationId(
        "10000000-0000-4000-8000-000000000019");
    auto handler = std::make_shared<ControlledHandler>();
    handler->block(activeId);
    auto worker = createWorker(handler);
    std::stop_source cancellation;

    auto active = executeAsync(
        *worker,
        request(
            Kind::Repair,
            activeId.value(),
            std::chrono::steady_clock::now() + 5min,
            cancellation.get_token()));
    const bool activeEntered = handler->waitForEntered(1U, 5s);
    auto successor = executeAsync(
        *worker,
        request(
            Kind::Inspect,
            "10000000-0000-4000-8000-000000000020"));
    const bool successorQueued = waitForQueue(*worker, 1U, 5s);
    const bool requested = cancellation.request_stop();
    const bool callerStillWaiting =
        active.wait_for(100ms) == std::future_status::timeout;

    handler->release();
    const auto activeResult = active.get();
    const auto successorResult = successor.get();

    require(
        activeEntered && successorQueued && requested,
        "active caller-token cancellation setup did not stabilize");
    require(
        callerStillWaiting,
        "caller-token cancellation released ownership before the handler drained");
    requireError(
        activeResult,
        Domain::ErrorCodes::Cancelled,
        "active caller-token cancellation did not own the terminal result");
    requireSucceeded(
        successorResult,
        "the FIFO successor did not run after cancelled active work drained");
    require(
        handler->enteredCount() == 2U &&
            handler->observation(0U).cancellationRequestedAtReturn &&
            handler->observation(1U).operationId.value() ==
                "10000000-0000-4000-8000-000000000020",
        "caller cancellation did not drain before FIFO successor promotion");
}

void testShutdownCancelsAndDrainsActiveOwnership()
{
    const auto identifier = operationId(
        "10000000-0000-4000-8000-000000000012");
    auto handler = std::make_shared<ControlledHandler>();
    handler->block(identifier);
    auto worker = createWorker(handler);

    auto active = executeAsync(
        *worker,
        request(Kind::StartNow, identifier.value()));
    const bool entered = handler->waitForEntered(1U, 5s);
    auto shutdown = std::async(
        std::launch::async,
        [&worker]() noexcept { worker->shutdown(); });
    const bool shutdownWaited =
        shutdown.wait_for(100ms) == std::future_status::timeout;
    const bool callerWaited =
        active.wait_for(0ms) == std::future_status::timeout;

    handler->release();
    shutdown.get();
    const auto activeResult = active.get();
    worker->shutdown();

    require(entered, "the shutdown-drain operation did not enter the handler");
    require(
        shutdownWaited && callerWaited,
        "shutdown did not retain both worker and caller ownership during the drain");
    requireError(
        activeResult,
        Domain::ErrorCodes::Cancelled,
        "shutdown did not cancel the active operation");
    require(
        !worker->snapshot().workerRunning,
        "the MTA worker remained running after its bounded shutdown drain");
}

void testShutdownCancelsQueuedWorkWithoutDispatch()
{
    const auto activeId = operationId(
        "10000000-0000-4000-8000-000000000017");
    auto handler = std::make_shared<ControlledHandler>();
    handler->block(activeId);
    auto worker = createWorker(handler);

    auto active = executeAsync(
        *worker,
        request(Kind::Register, activeId.value()));
    const bool activeEntered = handler->waitForEntered(1U, 5s);
    auto queued = executeAsync(
        *worker,
        request(
            Kind::Remove,
            "10000000-0000-4000-8000-000000000018"));
    const bool queuedVisible = waitForQueue(*worker, 1U, 5s);
    auto shutdown = std::async(
        std::launch::async,
        [&worker]() noexcept { worker->shutdown(); });
    const auto queuedResult = queued.get();
    const bool shutdownWaited =
        shutdown.wait_for(100ms) == std::future_status::timeout;
    const auto enteredBeforeRelease = handler->enteredCount();

    handler->release();
    shutdown.get();
    const auto activeResult = active.get();

    require(
        activeEntered && queuedVisible,
        "active-plus-queued shutdown setup did not stabilize");
    requireError(
        queuedResult,
        Domain::ErrorCodes::Cancelled,
        "shutdown did not cancel queued work");
    requireError(
        activeResult,
        Domain::ErrorCodes::Cancelled,
        "shutdown did not cancel active work");
    require(
        shutdownWaited,
        "shutdown did not drain the active handler before returning");
    require(
        enteredBeforeRelease == 1U && handler->enteredCount() == 1U,
        "shutdown dispatched queued work into the COM handler");
}

void testClosedAdmissionAndIdempotentShutdown()
{
    auto handler = std::make_shared<ControlledHandler>();
    auto worker = createWorker(handler);

    worker->shutdown();
    worker->shutdown();
    const auto snapshot = worker->snapshot();
    const auto result = worker->execute(request(
        Kind::Inspect,
        "10000000-0000-4000-8000-000000000013"));

    require(
        !snapshot.accepting && !snapshot.workerRunning &&
            snapshot.activeOperationCount == 0U &&
            snapshot.queuedOperationCount == 0U,
        "idempotent shutdown did not leave a closed and empty worker");
    requireError(
        result,
        Domain::ErrorCodes::TransportClosed,
        "an operation was admitted after the worker closed admission");
}

} // namespace

void registerManagerStartupComWorkerTests(TestRegistry& tests)
{
    addTest(
        tests,
        "manager_startup_com_worker_mta_payload",
        testExecutesOnDedicatedMtaAndPreservesPayload);
    addTest(
        tests,
        "manager_startup_com_worker_fifo_capacity",
        testFifoAndCapacity);
    addTest(
        tests,
        "manager_startup_com_worker_duplicate_ids",
        testRejectsDuplicateActiveAndQueuedIds);
    addTest(
        tests,
        "manager_startup_com_worker_queued_cancellation",
        testQueuedCancellationSkipsHandler);
    addTest(
        tests,
        "manager_startup_com_worker_predispatch_cancellation",
        testCancellationRelayPrecedesDispatchVisibility);
    addTest(
        tests,
        "manager_startup_com_worker_precancelled_admission",
        testAlreadyCancelledRequestIsNeverAdmitted);
    addTest(
        tests,
        "manager_startup_com_worker_queued_deadline",
        testQueuedDeadlineSkipsHandler);
    addTest(
        tests,
        "manager_startup_com_worker_active_deadline_drain",
        testActiveDeadlineWaitsForTerminalHandlerCompletion);
    addTest(
        tests,
        "manager_startup_com_worker_active_cancel_drain",
        testActiveCancellationWaitsForTerminalHandlerCompletion);
    addTest(
        tests,
        "manager_startup_com_worker_active_token_cancel_promotion",
        testActiveCallerTokenCancellationDrainsAndPromotesSuccessor);
    addTest(
        tests,
        "manager_startup_com_worker_shutdown_drain",
        testShutdownCancelsAndDrainsActiveOwnership);
    addTest(
        tests,
        "manager_startup_com_worker_shutdown_active_queued",
        testShutdownCancelsQueuedWorkWithoutDispatch);
    addTest(
        tests,
        "manager_startup_com_worker_closed_idempotent_shutdown",
        testClosedAdmissionAndIdempotentShutdown);
}

} // namespace ForgeConductor::Tests
