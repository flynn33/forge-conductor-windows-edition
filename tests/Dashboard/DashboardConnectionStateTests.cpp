#include "Infrastructure/Windows/Detail/DashboardConnectionState.h"
#include "Infrastructure/Windows/Detail/DashboardIoCompletionPort.h"

#include "ForgeConductor/Dashboard/DashboardHttpResponse.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <iomanip>
#include <iostream>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

namespace {

namespace Detail = ForgeConductor::Infrastructure::Windows::Detail;
namespace Dashboard = ForgeConductor::Dashboard;
namespace Contracts = ForgeConductor::Contracts;
namespace Domain = ForgeConductor::Domain;
namespace Windows = ForgeConductor::Infrastructure::Windows;

using namespace std::chrono_literals;

std::size_t assertions{};

void require(const bool condition, const std::string_view message)
{
    ++assertions;
    if (!condition) {
        throw std::runtime_error{std::string{message}};
    }
}

template <typename Value>
[[nodiscard]] Value take(Domain::Result<Value> result)
{
    if (!result) {
        throw std::runtime_error{
            result.error().code + ": " + result.error().message};
    }
    return std::move(result).value();
}

[[nodiscard]] std::vector<std::byte> bytes(const std::string_view text)
{
    std::vector<std::byte> result;
    result.reserve(text.size());
    for (const unsigned char value : text) {
        result.push_back(static_cast<std::byte>(value));
    }
    return result;
}

[[nodiscard]] std::string text(const std::span<const std::byte> value)
{
    std::string result;
    result.reserve(value.size());
    for (const auto entry : value) {
        result.push_back(static_cast<char>(std::to_integer<unsigned char>(entry)));
    }
    return result;
}

template <typename Predicate>
void waitUntil(Predicate predicate, const std::string_view message)
{
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (!predicate()) {
        if (std::chrono::steady_clock::now() >= deadline) {
            throw std::runtime_error{std::string{message}};
        }
        std::this_thread::sleep_for(1ms);
    }
}

class MutableClock final : public Contracts::IClock {
public:
    MutableClock() noexcept = default;

    [[nodiscard]] Domain::UtcTimePoint utcNow() const noexcept override
    {
        return {};
    }

    [[nodiscard]] Domain::MonotonicTimePoint monotonicNow()
        const noexcept override
    {
        const std::scoped_lock lock{mutex_};
        return std::chrono::steady_clock::now() + offset_;
    }

    void advance(const Domain::MonotonicTimePoint::duration amount) noexcept
    {
        const std::scoped_lock lock{mutex_};
        offset_ += amount;
    }

private:
    mutable std::mutex mutex_;
    Domain::MonotonicTimePoint::duration offset_{};
};

class SequenceUuidGenerator final : public Contracts::IUuidGenerator {
public:
    [[nodiscard]] Domain::Result<Domain::Uuid> next() noexcept override
    {
        try {
            const auto value = sequence_.fetch_add(1U) + 1U;
            std::ostringstream stream;
            stream << "10000000-0000-4000-8000-" << std::setfill('0')
                   << std::setw(12) << value;
            return Domain::Uuid::parse(stream.str());
        } catch (...) {
            return Domain::Result<Domain::Uuid>::failure(Domain::makeError(
                Domain::ErrorCodes::InternalFailure,
                "The test UUID generator failed."));
        }
    }

private:
    std::atomic_size_t sequence_{};
};

class ActiveOperationalState final
    : public Detail::IDashboardOperationalStateSource {
public:
    [[nodiscard]] bool operationalServiceActive() const noexcept override
    {
        return true;
    }
};

class FakeConnectionIo final : public Detail::IDashboardConnectionIo {
public:
    explicit FakeConnectionIo(const Detail::DashboardIoCompletionKey key)
        noexcept
        : key_{key}
    {
    }

    [[nodiscard]] Detail::DashboardIoCompletionKey completionKey()
        const noexcept override
    {
        return key_;
    }

    [[nodiscard]] OVERLAPPED* borrowedOperation() noexcept override
    {
        return &operation_;
    }

    [[nodiscard]] Detail::DashboardConnectionSocketState state()
        const noexcept override
    {
        std::unique_lock lock{mutex_};
        if (blockNextStateObservation_) {
            stateObservationBlocked_ = true;
            stateGateChanged_.notify_all();
            stateGateChanged_.wait(lock, [this] {
                return releaseStateObservation_;
            });
            blockNextStateObservation_ = false;
            stateObservationBlocked_ = false;
            releaseStateObservation_ = false;
        }
        return state_;
    }

    [[nodiscard]] Detail::DashboardConnectionSocketSnapshot snapshot()
        const noexcept override
    {
        const std::scoped_lock lock{mutex_};
        return Detail::DashboardConnectionSocketSnapshot{
            state_,
            shutdownRequested_,
            received_.size(),
            activeLength_};
    }

    [[nodiscard]] Domain::Result<
        Detail::DashboardConnectionSocketIssueDisposition>
    issueReceive() noexcept override
    {
        const std::scoped_lock lock{mutex_};
        if (state_ != Detail::DashboardConnectionSocketState::Idle ||
            shutdownRequested_) {
            return issueFailure("receive conflict");
        }
        state_ = Detail::DashboardConnectionSocketState::ReceiveIssued;
        activeKind_ = Detail::DashboardConnectionSocketOperationKind::Receive;
        activeLength_ = Detail::DashboardConnectionSocket::ReceiveBufferLength;
        received_.clear();
        return issueSuccess();
    }

    [[nodiscard]] Domain::Result<
        Detail::DashboardConnectionSocketIssueDisposition>
    issueSend(const std::span<const std::byte> value) noexcept override
    {
        const std::scoped_lock lock{mutex_};
        if (state_ != Detail::DashboardConnectionSocketState::Idle ||
            shutdownRequested_ || value.empty()) {
            return issueFailure("send conflict");
        }
        state_ = Detail::DashboardConnectionSocketState::SendIssued;
        activeKind_ = Detail::DashboardConnectionSocketOperationKind::Send;
        activeLength_ = value.size();
        try {
            issuedSends_.emplace_back(value.begin(), value.end());
        } catch (...) {
            state_ = Detail::DashboardConnectionSocketState::Idle;
            activeLength_ = 0U;
            return issueFailure("send recording failed");
        }
        return issueSuccess();
    }

    [[nodiscard]] Domain::Result<
        Detail::DashboardConnectionSocketCancellationDisposition>
    requestCancellation() noexcept override
    {
        const std::scoped_lock lock{mutex_};
        if (state_ == Detail::DashboardConnectionSocketState::Idle) {
            return Domain::Result<Detail::
                DashboardConnectionSocketCancellationDisposition>::failure(
                Domain::makeError(
                    Domain::ErrorCodes::Conflict,
                    "The fake socket is idle."));
        }
        state_ = activeKind_ ==
                Detail::DashboardConnectionSocketOperationKind::Receive
            ? Detail::DashboardConnectionSocketState::
                  ReceiveCancellationRequested
            : Detail::DashboardConnectionSocketState::
                  SendCancellationRequested;
        return Domain::Result<Detail::
            DashboardConnectionSocketCancellationDisposition>::success(
            Detail::DashboardConnectionSocketCancellationDisposition::
                Requested);
    }

    [[nodiscard]] Domain::Result<
        Detail::DashboardConnectionSocketShutdownDisposition>
    shutdownBoth() noexcept override
    {
        const std::scoped_lock lock{mutex_};
        const bool already = shutdownRequested_;
        shutdownRequested_ = true;
        return Domain::Result<Detail::
            DashboardConnectionSocketShutdownDisposition>::success(
            already
                ? Detail::DashboardConnectionSocketShutdownDisposition::
                      AlreadyRequested
                : Detail::DashboardConnectionSocketShutdownDisposition::
                      Requested);
    }

    [[nodiscard]] Domain::Result<
        Detail::DashboardConnectionSocketReapResult>
    reap(
        const Detail::DashboardIoCompletionKey key,
        const DWORD transferredBytes,
        OVERLAPPED* const operation,
        const DWORD nativeError) noexcept override
    {
        const std::scoped_lock lock{mutex_};
        if (!(key == key_) || operation != &operation_ ||
            state_ == Detail::DashboardConnectionSocketState::Idle ||
            static_cast<std::size_t>(transferredBytes) > activeLength_) {
            return reapFailure(
                Domain::ErrorCodes::IntegrityFailure,
                "The fake socket completion shape was invalid.");
        }
        const auto kind = activeKind_;
        state_ = Detail::DashboardConnectionSocketState::Idle;
        activeLength_ = 0U;
        if (nativeError != ERROR_SUCCESS) {
            return reapFailure(
                nativeError == ERROR_OPERATION_ABORTED
                    ? Domain::ErrorCodes::Cancelled
                    : Domain::ErrorCodes::TransportClosed,
                "The fake socket injected a native failure.");
        }
        if (transferredBytes == 0U) {
            return reapFailure(
                Domain::ErrorCodes::TransportClosed,
                "The fake socket injected EOF.");
        }
        if (kind ==
            Detail::DashboardConnectionSocketOperationKind::Receive) {
            if (nextReceive_.size() !=
                static_cast<std::size_t>(transferredBytes)) {
                return reapFailure(
                    Domain::ErrorCodes::IntegrityFailure,
                    "The fake receive count did not match its staged bytes.");
            }
            received_ = nextReceive_;
            nextReceive_.clear();
        }
        return Domain::Result<Detail::
            DashboardConnectionSocketReapResult>::success(
            Detail::DashboardConnectionSocketReapResult{
                kind, transferredBytes});
    }

    [[nodiscard]] std::span<const std::byte> receivedBytes()
        const noexcept override
    {
        const std::scoped_lock lock{mutex_};
        return {received_};
    }

    void stageReceive(std::vector<std::byte> value)
    {
        const std::scoped_lock lock{mutex_};
        nextReceive_ = std::move(value);
    }

    [[nodiscard]] std::size_t activeLength() const noexcept
    {
        const std::scoped_lock lock{mutex_};
        return activeLength_;
    }

    [[nodiscard]] std::size_t sendIssueCount() const noexcept
    {
        const std::scoped_lock lock{mutex_};
        return issuedSends_.size();
    }

    [[nodiscard]] std::vector<std::byte> sendIssue(
        const std::size_t index) const
    {
        const std::scoped_lock lock{mutex_};
        return issuedSends_.at(index);
    }

    void blockNextStateObservation() noexcept
    {
        const std::scoped_lock lock{mutex_};
        blockNextStateObservation_ = true;
        stateObservationBlocked_ = false;
        releaseStateObservation_ = false;
    }

    void waitUntilStateObservationBlocked()
    {
        std::unique_lock lock{mutex_};
        if (!stateGateChanged_.wait_for(lock, 5s, [this] {
                return stateObservationBlocked_;
            })) {
            throw std::runtime_error{
                "snapshot did not enter the fake socket state gate"};
        }
    }

    void releaseStateObservation() noexcept
    {
        const std::scoped_lock lock{mutex_};
        releaseStateObservation_ = true;
        stateGateChanged_.notify_all();
    }

private:
    [[nodiscard]] static Domain::Result<
        Detail::DashboardConnectionSocketIssueDisposition>
    issueSuccess()
    {
        return Domain::Result<Detail::
            DashboardConnectionSocketIssueDisposition>::success(
            Detail::DashboardConnectionSocketIssueDisposition::Pending);
    }

    [[nodiscard]] static Domain::Result<
        Detail::DashboardConnectionSocketIssueDisposition>
    issueFailure(const std::string_view message)
    {
        return Domain::Result<Detail::
            DashboardConnectionSocketIssueDisposition>::failure(
            Domain::makeError(Domain::ErrorCodes::Conflict,
                              std::string{message}));
    }

    [[nodiscard]] static Domain::Result<
        Detail::DashboardConnectionSocketReapResult>
    reapFailure(
        const std::string_view code,
        const std::string_view message)
    {
        return Domain::Result<Detail::
            DashboardConnectionSocketReapResult>::failure(
            Domain::makeError(code, std::string{message}));
    }

    const Detail::DashboardIoCompletionKey key_;
    mutable std::mutex mutex_;
    mutable std::condition_variable stateGateChanged_;
    OVERLAPPED operation_{};
    std::vector<std::byte> nextReceive_;
    std::vector<std::byte> received_;
    std::vector<std::vector<std::byte>> issuedSends_;
    Detail::DashboardConnectionSocketOperationKind activeKind_{
        Detail::DashboardConnectionSocketOperationKind::Receive};
    Detail::DashboardConnectionSocketState state_{
        Detail::DashboardConnectionSocketState::Idle};
    std::size_t activeLength_{};
    bool shutdownRequested_{};
    mutable bool blockNextStateObservation_{};
    mutable bool stateObservationBlocked_{};
    mutable bool releaseStateObservation_{};
};

class CompleteApplication final
    : public Dashboard::IDashboardConnectionApplication {
public:
    explicit CompleteApplication(
        const Dashboard::DashboardPostDeliveryAction action) noexcept
        : action_{action}
    {
    }

    [[nodiscard]] Domain::Result<Dashboard::DashboardPreparedExchange>
    prepare(
        Dashboard::DashboardHttpRequest request,
        const bool operationalServiceActive,
        Domain::OperationContext) noexcept override
    {
        try {
            preparedTarget_ = request.target();
            operationalState_ = operationalServiceActive;
            ++prepareCalls_;
            auto encoded = Dashboard::DashboardHttpResponseEncoder::encode(
                Dashboard::DashboardHttpResponse{
                    200U,
                    "text/plain; charset=utf-8",
                    bytes("state-machine-response")});
            return Dashboard::DashboardPreparedExchange::createComplete(
                std::move(encoded), action_);
        } catch (...) {
            return Domain::Result<
                Dashboard::DashboardPreparedExchange>::failure(
                Domain::makeError(
                    Domain::ErrorCodes::InternalFailure,
                    "The test connection application failed."));
        }
    }

    [[nodiscard]] Domain::Result<void> executePostDelivery(
        const Dashboard::DashboardPostDeliveryAction action,
        Domain::OperationContext) noexcept override
    {
        postAction_.store(action, std::memory_order_release);
        postCalls_.fetch_add(1U, std::memory_order_acq_rel);
        return Domain::Result<void>::success();
    }

    [[nodiscard]] std::size_t prepareCalls() const noexcept
    {
        return prepareCalls_.load(std::memory_order_acquire);
    }

    [[nodiscard]] std::size_t postCalls() const noexcept
    {
        return postCalls_.load(std::memory_order_acquire);
    }

    [[nodiscard]] const std::string& preparedTarget() const noexcept
    {
        return preparedTarget_;
    }

    [[nodiscard]] bool operationalState() const noexcept
    {
        return operationalState_;
    }

private:
    const Dashboard::DashboardPostDeliveryAction action_;
    std::atomic_size_t prepareCalls_{};
    std::atomic_size_t postCalls_{};
    std::atomic<Dashboard::DashboardPostDeliveryAction> postAction_{
        Dashboard::DashboardPostDeliveryAction::None};
    std::string preparedTarget_;
    bool operationalState_{};
};

class SseSubscriptionControl final {
public:
    explicit SseSubscriptionControl(const double deliveryHz) noexcept
        : deliveryHz_{deliveryHz}
    {
    }

    void attach(
        std::weak_ptr<Dashboard::IDashboardSseReadySink> sink) noexcept
    {
        std::shared_ptr<Dashboard::IDashboardSseReadySink> ready;
        {
            const std::scoped_lock lock{mutex_};
            if (closed_) {
                return;
            }
            sink_ = std::move(sink);
            if (latest_ != nullptr) {
                ready = sink_.lock();
            }
        }
        if (ready != nullptr) {
            ready->signal();
        }
    }

    void publish(Dashboard::DashboardSseFramePair::ImmutableFrame frame)
        noexcept
    {
        std::shared_ptr<Dashboard::IDashboardSseReadySink> ready;
        {
            const std::scoped_lock lock{mutex_};
            if (closed_ || frame == nullptr) {
                return;
            }
            const bool wasEmpty = latest_ == nullptr;
            latest_ = std::move(frame);
            if (wasEmpty) {
                ready = sink_.lock();
            }
        }
        if (ready != nullptr) {
            ready->signal();
        }
    }

    [[nodiscard]] Dashboard::DashboardSseFramePair::ImmutableFrame take()
        noexcept
    {
        const std::scoped_lock lock{mutex_};
        if (closed_) {
            return {};
        }
        return std::exchange(latest_, {});
    }

    [[nodiscard]] std::size_t pendingCount() const noexcept
    {
        const std::scoped_lock lock{mutex_};
        return latest_ == nullptr ? 0U : 1U;
    }

    void close() noexcept
    {
        const std::scoped_lock lock{mutex_};
        closed_ = true;
        latest_.reset();
        sink_.reset();
    }

    [[nodiscard]] double deliveryHz() const noexcept { return deliveryHz_; }

    [[nodiscard]] bool isClosed() const noexcept
    {
        const std::scoped_lock lock{mutex_};
        return closed_;
    }

private:
    const double deliveryHz_{};
    mutable std::mutex mutex_;
    std::weak_ptr<Dashboard::IDashboardSseReadySink> sink_;
    Dashboard::DashboardSseFramePair::ImmutableFrame latest_;
    bool closed_{};
};

class TestSseSubscription final
    : public Dashboard::IDashboardSseSubscription {
public:
    explicit TestSseSubscription(
        std::shared_ptr<SseSubscriptionControl> control) noexcept
        : control_{std::move(control)}
    {
    }

    [[nodiscard]] double deliveryHz() const noexcept override
    {
        return control_->deliveryHz();
    }

    void attachReadySink(
        std::weak_ptr<Dashboard::IDashboardSseReadySink> sink)
        noexcept override
    {
        control_->attach(std::move(sink));
    }

    [[nodiscard]] Dashboard::DashboardSseFramePair::ImmutableFrame
    takeLatest() noexcept override
    {
        return control_->take();
    }

    [[nodiscard]] std::size_t pendingCount() const noexcept override
    {
        return control_->pendingCount();
    }

    void close() noexcept override { control_->close(); }

private:
    std::shared_ptr<SseSubscriptionControl> control_;
};

class SseApplication final
    : public Dashboard::IDashboardConnectionApplication {
public:
    explicit SseApplication(
        std::shared_ptr<SseSubscriptionControl> control) noexcept
        : control_{std::move(control)}
    {
    }

    [[nodiscard]] Domain::Result<Dashboard::DashboardPreparedExchange>
    prepare(
        Dashboard::DashboardHttpRequest,
        bool,
        Domain::OperationContext) noexcept override
    {
        ++prepareCalls_;
        return Dashboard::DashboardPreparedExchange::createSse(
            Dashboard::DashboardHttpResponseEncoder::encodeSseBootstrap(),
            std::make_unique<TestSseSubscription>(control_));
    }

    [[nodiscard]] Domain::Result<void> executePostDelivery(
        Dashboard::DashboardPostDeliveryAction,
        Domain::OperationContext) noexcept override
    {
        return Domain::Result<void>::failure(Domain::makeError(
            Domain::ErrorCodes::IntegrityFailure,
            "An SSE test must not execute post-delivery work."));
    }

    [[nodiscard]] std::size_t prepareCalls() const noexcept
    {
        return prepareCalls_.load(std::memory_order_acquire);
    }

private:
    std::shared_ptr<SseSubscriptionControl> control_;
    std::atomic_size_t prepareCalls_{};
};

class BlockingApplication final
    : public Dashboard::IDashboardConnectionApplication {
public:
    [[nodiscard]] Domain::Result<Dashboard::DashboardPreparedExchange>
    prepare(
        Dashboard::DashboardHttpRequest,
        bool,
        Domain::OperationContext context) noexcept override
    {
        entered_.store(true, std::memory_order_release);
        while (!context.cancellation.stop_requested()) {
            std::this_thread::yield();
        }
        cancelled_.store(true, std::memory_order_release);
        return Domain::Result<
            Dashboard::DashboardPreparedExchange>::failure(
            Domain::makeError(
                Domain::ErrorCodes::Cancelled,
                "The blocking test application observed cancellation."));
    }

    [[nodiscard]] Domain::Result<void> executePostDelivery(
        Dashboard::DashboardPostDeliveryAction,
        Domain::OperationContext) noexcept override
    {
        return Domain::Result<void>::failure(Domain::makeError(
            Domain::ErrorCodes::IntegrityFailure,
            "The blocking prepare test reached post-delivery."));
    }

    [[nodiscard]] bool entered() const noexcept
    {
        return entered_.load(std::memory_order_acquire);
    }

    [[nodiscard]] bool cancelled() const noexcept
    {
        return cancelled_.load(std::memory_order_acquire);
    }

private:
    std::atomic_bool entered_{};
    std::atomic_bool cancelled_{};
};

class GatedActionApplication final
    : public Dashboard::IDashboardConnectionApplication {
public:
    [[nodiscard]] Domain::Result<Dashboard::DashboardPreparedExchange>
    prepare(
        Dashboard::DashboardHttpRequest,
        bool,
        Domain::OperationContext context) noexcept override
    {
        {
            const std::scoped_lock lock{mutex_};
            entered_ = true;
            changed_.notify_all();
        }
        std::unique_lock lock{mutex_};
        changed_.wait(lock, [this, &context] {
            return released_ || context.cancellation.stop_requested();
        });
        if (context.cancellation.stop_requested()) {
            return Domain::Result<
                Dashboard::DashboardPreparedExchange>::failure(
                Domain::makeError(
                    Domain::ErrorCodes::Cancelled,
                    "The gated action application was cancelled."));
        }
        return Dashboard::DashboardPreparedExchange::createComplete(
            Dashboard::DashboardHttpResponseEncoder::encode(
                Dashboard::DashboardHttpResponse{
                    200U, "text/plain", bytes("must-not-be-sent")}),
            Dashboard::DashboardPostDeliveryAction::
                RequestManagerShutdown);
    }

    [[nodiscard]] Domain::Result<void> executePostDelivery(
        Dashboard::DashboardPostDeliveryAction,
        Domain::OperationContext) noexcept override
    {
        ++postCalls_;
        return Domain::Result<void>::success();
    }

    void waitUntilEntered()
    {
        std::unique_lock lock{mutex_};
        if (!changed_.wait_for(lock, 5s, [this] { return entered_; })) {
            throw std::runtime_error{
                "gated action application did not enter"};
        }
    }

    void release() noexcept
    {
        const std::scoped_lock lock{mutex_};
        released_ = true;
        changed_.notify_all();
    }

    [[nodiscard]] std::size_t postCalls() const noexcept
    {
        return postCalls_.load(std::memory_order_acquire);
    }

private:
    mutable std::mutex mutex_;
    std::condition_variable changed_;
    std::atomic_size_t postCalls_{};
    bool entered_{};
    bool released_{};
};

class ForwardingKernelSink final : public Detail::IDashboardIocpCompletionSink {
public:
    void setTarget(
        std::weak_ptr<Detail::IDashboardConnectionDispatchTarget> target)
        noexcept
    {
        const std::scoped_lock lock{mutex_};
        target_ = std::move(target);
    }

    void consume(
        const Detail::DashboardIoCompletionPacket packet,
        const DWORD nativeError) noexcept override
    {
        std::shared_ptr<Detail::IDashboardConnectionDispatchTarget> target;
        {
            const std::scoped_lock lock{mutex_};
            target = target_.lock();
        }
        if (target != nullptr) {
            target->dispatchIocp(
                packet.transferredBytes,
                packet.operation,
                nativeError);
        }
    }

    void fatal(const DWORD nativeError) noexcept override
    {
        fatalError_.store(nativeError, std::memory_order_release);
    }

    [[nodiscard]] DWORD fatalError() const noexcept
    {
        return fatalError_.load(std::memory_order_acquire);
    }

private:
    mutable std::mutex mutex_;
    std::weak_ptr<Detail::IDashboardConnectionDispatchTarget> target_;
    std::atomic<DWORD> fatalError_{};
};

class ForwardingDeadlineSink final
    : public Windows::IWindowsDashboardDeadlineSink {
public:
    void setTarget(
        std::weak_ptr<Detail::IDashboardConnectionDispatchTarget> target)
        noexcept
    {
        const std::scoped_lock lock{mutex_};
        target_ = std::move(target);
    }

    void signal(const Windows::WindowsDashboardDeadline deadline)
        noexcept override
    {
        std::shared_ptr<Detail::IDashboardConnectionDispatchTarget> target;
        {
            const std::scoped_lock lock{mutex_};
            target = target_.lock();
        }
        if (target != nullptr) {
            target->dispatchDeadline(deadline);
        }
    }

private:
    mutable std::mutex mutex_;
    std::weak_ptr<Detail::IDashboardConnectionDispatchTarget> target_;
};

struct RealStateFixture final {
    explicit RealStateFixture(
        const Dashboard::DashboardPostDeliveryAction action =
            Dashboard::DashboardPostDeliveryAction::None)
        : RealStateFixture{
              std::make_shared<CompleteApplication>(action)}
    {
        completeApplication =
            std::static_pointer_cast<CompleteApplication>(application);
    }

    explicit RealStateFixture(
        std::shared_ptr<Dashboard::IDashboardConnectionApplication>
            connectionApplication)
        : clock{std::make_shared<MutableClock>()},
          uuidGenerator{std::make_shared<SequenceUuidGenerator>()},
          operationalState{std::make_shared<ActiveOperationalState>()},
          application{std::move(connectionApplication)},
          kernelSink{std::make_shared<ForwardingKernelSink>()},
          deadlineSink{std::make_shared<ForwardingDeadlineSink>()}
    {
        runtimeServices = take(
            Detail::DashboardConnectionRuntimeServices::create(
                clock,
                uuidGenerator,
                operationalState));
        responseCatalog =
            take(Detail::DashboardConnectionResponseCatalog::create());
        handlerExecutor =
            take(Windows::WindowsDashboardHandlerExecutor::create());
        deadlineScheduler = take(
            Windows::WindowsDashboardDeadlineScheduler::create(
                clock, deadlineSink));
        auto port = take(Detail::DashboardIoCompletionPort::create());
        kernel = take(Detail::DashboardIocpWorkerKernel::create(
            std::move(port), kernelSink));
        identity = take(runtimeServices->allocateConnectionIdentity());
        auto io = std::make_unique<FakeConnectionIo>(
            identity.completionKey);
        socket = io.get();
        state = take(Detail::DashboardConnectionState::create(
            3U,
            identity,
            std::move(io),
            *kernel,
            *deadlineScheduler,
            *handlerExecutor,
            *runtimeServices,
            application,
            *responseCatalog));
        kernelSink->setTarget(state);
        deadlineSink->setTarget(state);
    }

    ~RealStateFixture() noexcept
    {
        if (state != nullptr && !state->isDrained()) {
            state->beginShutdown();
            if (socket->state() !=
                Detail::DashboardConnectionSocketState::Idle) {
                state->dispatchIocp(
                    0U,
                    socket->borrowedOperation(),
                    ERROR_OPERATION_ABORTED);
            }
            try {
                waitUntil(
                    [this] { return state->isDrained(); },
                    "fixture state did not drain");
            } catch (...) {
                std::terminate();
            }
        }
        deadlineSink->setTarget({});
        kernelSink->setTarget({});
        state.reset();
        deadlineScheduler->shutdown();
        handlerExecutor->shutdown();
        kernel->shutdown();
    }

    void start()
    {
        auto started = state->start();
        require(started.hasValue(), "real state failed to start");
        require(
            state->snapshot().state() ==
                Detail::DashboardConnectionLifecycleState::Receiving,
            "real state did not enter Receiving");
    }

    void receive(const std::string_view request)
    {
        auto requestBytes = bytes(request);
        const auto size = static_cast<DWORD>(requestBytes.size());
        socket->stageReceive(std::move(requestBytes));
        state->dispatchIocp(
            size, socket->borrowedOperation(), ERROR_SUCCESS);
    }

    void completeSend(
        const DWORD transferredBytes,
        const DWORD nativeError = ERROR_SUCCESS)
    {
        state->dispatchIocp(
            transferredBytes, socket->borrowedOperation(), nativeError);
    }

    std::shared_ptr<MutableClock> clock;
    std::shared_ptr<SequenceUuidGenerator> uuidGenerator;
    std::shared_ptr<ActiveOperationalState> operationalState;
    std::shared_ptr<Dashboard::IDashboardConnectionApplication> application;
    std::shared_ptr<CompleteApplication> completeApplication;
    std::shared_ptr<ForwardingKernelSink> kernelSink;
    std::shared_ptr<ForwardingDeadlineSink> deadlineSink;
    std::unique_ptr<Detail::DashboardConnectionRuntimeServices>
        runtimeServices;
    std::unique_ptr<Detail::DashboardConnectionResponseCatalog>
        responseCatalog;
    std::unique_ptr<Windows::WindowsDashboardHandlerExecutor>
        handlerExecutor;
    std::unique_ptr<Windows::WindowsDashboardDeadlineScheduler>
        deadlineScheduler;
    std::unique_ptr<Detail::DashboardIocpWorkerKernel> kernel;
    Detail::DashboardConnectionRuntimeIdentity identity{};
    FakeConnectionIo* socket{};
    std::shared_ptr<Detail::DashboardConnectionState> state;
};

class MockDispatchTarget final
    : public Detail::IDashboardConnectionDispatchTarget {
public:
    [[nodiscard]] Detail::DashboardIoCompletionKey completionKey()
        const noexcept override
    {
        return Detail::DashboardIoCompletionKey{17U};
    }

    [[nodiscard]] std::uint64_t registrationId() const noexcept override
    {
        return 31U;
    }

    [[nodiscard]] std::uint64_t generationId() const noexcept override
    {
        return 7U;
    }

    [[nodiscard]] Domain::Result<void> start() noexcept override
    {
        started_ = true;
        return Domain::Result<void>::success();
    }

    void dispatchIocp(
        const DWORD transferredBytes,
        OVERLAPPED* const operation,
        const DWORD nativeError) noexcept override
    {
        bytes_ = transferredBytes;
        operation_ = operation;
        nativeError_ = nativeError;
    }

    void dispatchDeadline(
        const Windows::WindowsDashboardDeadline deadline) noexcept override
    {
        deadline_ = deadline;
    }

    void beginShutdown() noexcept override { shutdown_ = true; }

    [[nodiscard]] bool isDrained() const noexcept override
    {
        return shutdown_;
    }

    [[nodiscard]] Detail::DashboardConnectionStateSnapshot snapshot()
        const noexcept override
    {
        return Detail::DashboardConnectionStateSnapshot{
            started_
                ? Detail::DashboardConnectionLifecycleState::Receiving
                : Detail::DashboardConnectionLifecycleState::Created,
            registrationId(),
            generationId(),
            completionKey(),
            operation_ != nullptr,
            false,
            deadline_.armSequence != 0U,
            shutdown_,
            nativeError_ != ERROR_SUCCESS};
    }

    DWORD bytes_{};
    OVERLAPPED* operation_{};
    DWORD nativeError_{};
    Windows::WindowsDashboardDeadline deadline_{};
    bool started_{};
    bool shutdown_{};
};

static_assert(std::is_final_v<Detail::DashboardConnectionState>);
static_assert(std::is_base_of_v<
              Detail::IDashboardConnectionDispatchTarget,
              Detail::DashboardConnectionState>);
static_assert(std::is_base_of_v<
              Detail::IDashboardConnectionEventFatalSink,
              Detail::DashboardConnectionState>);
static_assert(!std::is_copy_constructible_v<Detail::DashboardConnectionState>);
static_assert(!std::is_move_constructible_v<Detail::DashboardConnectionState>);
static_assert(std::has_virtual_destructor_v<
              Detail::IDashboardConnectionDispatchTarget>);
static_assert(std::has_virtual_destructor_v<Detail::IDashboardConnectionIo>);
static_assert(noexcept(std::declval<
                       Detail::IDashboardConnectionDispatchTarget&>()
                           .dispatchIocp(0U, nullptr, ERROR_SUCCESS)));
static_assert(noexcept(std::declval<
                       Detail::IDashboardConnectionDispatchTarget&>()
                           .beginShutdown()));

void exposesEveryExactLifecycleState()
{
    using State = Detail::DashboardConnectionLifecycleState;
    require(State::Created != State::Receiving,
            "Created and Receiving collapsed");
    require(State::Receiving != State::Preparing,
            "Receiving and Preparing collapsed");
    require(State::Preparing != State::SendingComplete,
            "Preparing and SendingComplete collapsed");
    require(State::SendingComplete != State::SendingSseBootstrap,
            "complete and SSE bootstrap collapsed");
    require(State::SendingSseBootstrap != State::SseIdle,
            "SSE bootstrap and idle collapsed");
    require(State::SseIdle != State::SendingSseFrame,
            "SSE idle and frame send collapsed");
    require(State::SendingSseFrame != State::AwaitingPostDelivery,
            "SSE send and post-delivery collapsed");
    require(State::AwaitingPostDelivery != State::Closing,
            "post-delivery and Closing collapsed");
    require(State::Closing != State::Drained,
            "Closing and Drained collapsed");
}

void exposesMockableFixedDispatchBoundary()
{
    MockDispatchTarget target;
    Detail::IDashboardConnectionDispatchTarget& dispatch = target;
    require(dispatch.completionKey().value() == 17U,
            "completion key changed");
    require(dispatch.registrationId() == 31U,
            "registration id changed");
    require(dispatch.generationId() == 7U,
            "generation id changed");
    require(dispatch.start().hasValue(), "start failed");

    OVERLAPPED operation{};
    dispatch.dispatchIocp(23U, &operation, ERROR_OPERATION_ABORTED);
    dispatch.dispatchDeadline(Windows::WindowsDashboardDeadline{
        31U,
        9U,
        Windows::WindowsDashboardDeadlineKind::HeaderIngress,
        Domain::MonotonicTimePoint{}});
    const auto active = dispatch.snapshot();
    require(active.state() ==
                Detail::DashboardConnectionLifecycleState::Receiving,
            "snapshot lost active state");
    require(active.socketOperationOutstanding(),
            "snapshot lost socket obligation");
    require(active.deadlineArmed(), "snapshot lost deadline obligation");
    require(active.hasFailure(), "snapshot lost native failure");

    dispatch.beginShutdown();
    const auto closed = dispatch.snapshot();
    require(dispatch.isDrained(), "mock target did not drain");
    require(closed.shutdownRequested(), "shutdown was not observable");
}

void publishesBoundedTimingContracts()
{
    require(
        Detail::DashboardConnectionState::HeaderIngressLifetime ==
            std::chrono::seconds{2},
        "header ingress lifetime changed");
    require(
        Detail::DashboardConnectionState::SocketLifetime ==
            std::chrono::seconds{15},
        "socket lifetime changed");
    require(
        Detail::DashboardConnectionState::ServerSentEventsLifetime ==
            std::chrono::hours{1},
        "SSE lifetime changed");
    require(
        ForgeConductor::Dashboard::IDashboardSseSubscription::
                MinimumDeliveryHz ==
            1.0,
        "minimum SSE rate changed");
    require(
        ForgeConductor::Dashboard::IDashboardSseSubscription::
                MaximumDeliveryHz ==
            2.0,
        "maximum SSE rate changed");
}

constexpr std::string_view CompleteRequest =
    "GET /dashboard HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n";

void runsRealIngressPreparePartialSendAndDrain()
{
    RealStateFixture fixture;
    fixture.start();
    fixture.receive(CompleteRequest);
    waitUntil(
        [&fixture] {
            return fixture.state->snapshot().state() ==
                Detail::DashboardConnectionLifecycleState::SendingComplete;
        },
        "real state did not prepare a complete response");
    require(fixture.completeApplication->prepareCalls() == 1U,
            "application prepare was not called exactly once");
    require(fixture.completeApplication->preparedTarget() == "/dashboard",
            "parsed target changed before application dispatch");
    require(fixture.completeApplication->operationalState(),
            "operational snapshot was not captured");
    require(fixture.socket->sendIssueCount() == 1U,
            "complete response did not issue one initial send");

    const auto initial = fixture.socket->sendIssue(0U);
    require(initial.size() > 9U, "encoded response was unexpectedly short");
    fixture.completeSend(9U);
    require(
        fixture.state->snapshot().state() ==
            Detail::DashboardConnectionLifecycleState::SendingComplete,
        "partial send left complete-delivery state");
    require(fixture.socket->sendIssueCount() == 2U,
            "partial send did not issue the exact remaining suffix");
    const auto suffix = fixture.socket->sendIssue(1U);
    require(suffix.size() == initial.size() - 9U,
            "partial send suffix length changed");
    require(std::equal(
                suffix.begin(), suffix.end(), initial.begin() + 9),
            "partial send suffix bytes changed");

    fixture.completeSend(static_cast<DWORD>(suffix.size()));
    waitUntil(
        [&fixture] { return fixture.state->isDrained(); },
        "complete response did not drain");
    const auto responseText = text(initial);
    require(responseText.starts_with("HTTP/1.1 200 OK\r\n"),
            "complete response lost its HTTP status");
    require(responseText.ends_with("state-machine-response"),
            "complete response lost its body");
    require(fixture.completeApplication->postCalls() == 0U,
            "ordinary response invented a post-delivery action");
}

void reservesBeforeByteOneAndRunsPostDeliveryAfterSuccess()
{
    RealStateFixture fixture{
        Dashboard::DashboardPostDeliveryAction::RequestManagerShutdown};
    fixture.start();
    fixture.receive(CompleteRequest);
    waitUntil(
        [&fixture] {
            return fixture.state->snapshot().state() ==
                Detail::DashboardConnectionLifecycleState::SendingComplete;
        },
        "action response was not prepared");
    require(fixture.socket->sendIssueCount() == 1U,
            "action response did not begin delivery");
    require(fixture.handlerExecutor->reservationCount() == 1U,
            "post-delivery capacity was not reserved before byte one");
    require(fixture.completeApplication->postCalls() == 0U,
            "post-delivery action ran before acknowledgement");

    fixture.completeSend(
        static_cast<DWORD>(fixture.socket->activeLength()));
    waitUntil(
        [&fixture] {
            return fixture.completeApplication->postCalls() == 1U;
        },
        "acknowledged action did not run");
    waitUntil(
        [&fixture] { return fixture.state->isDrained(); },
        "post-delivery connection did not drain");
    require(fixture.handlerExecutor->reservationCount() == 0U,
            "post-delivery reservation was not consumed");
}

void suppressesReservedActionWhenSendFails()
{
    RealStateFixture fixture{
        Dashboard::DashboardPostDeliveryAction::RequestManagerShutdown};
    fixture.start();
    fixture.receive(CompleteRequest);
    waitUntil(
        [&fixture] {
            return fixture.state->snapshot().state() ==
                Detail::DashboardConnectionLifecycleState::SendingComplete;
        },
        "failing action response was not prepared");
    require(fixture.handlerExecutor->reservationCount() == 1U,
            "failing send lacked its pre-delivery reservation");

    fixture.completeSend(0U, ERROR_NETNAME_DELETED);
    waitUntil(
        [&fixture] { return fixture.state->isDrained(); },
        "failed action send did not drain");
    require(fixture.completeApplication->postCalls() == 0U,
            "failed send executed its post-delivery action");
    require(fixture.handlerExecutor->reservationCount() == 0U,
            "failed send leaked its reservation");
    require(fixture.state->snapshot().hasFailure(),
            "failed send was not retained in status");
}

void ignoresStaleDeadlineAndCancellationDrainsOutstandingReceive()
{
    RealStateFixture fixture;
    fixture.start();
    fixture.state->dispatchDeadline(Windows::WindowsDashboardDeadline{
        fixture.identity.registrationId,
        (std::numeric_limits<std::uint64_t>::max)(),
        Windows::WindowsDashboardDeadlineKind::HeaderIngress,
        fixture.clock->monotonicNow()});
    require(
        fixture.state->snapshot().state() ==
            Detail::DashboardConnectionLifecycleState::Receiving,
        "stale deadline changed a live connection");

    fixture.state->beginShutdown();
    require(
        fixture.state->snapshot().state() ==
            Detail::DashboardConnectionLifecycleState::Closing,
        "shutdown did not retain outstanding receive storage");
    fixture.state->dispatchIocp(
        0U,
        fixture.socket->borrowedOperation(),
        ERROR_OPERATION_ABORTED);
    waitUntil(
        [&fixture] { return fixture.state->isDrained(); },
        "cancelled receive did not reach Drained");
}

void malformedForeignCompletionClosesButRetainsExactSocketObligation()
{
    RealStateFixture fixture;
    fixture.start();
    OVERLAPPED foreign{};
    fixture.state->dispatchIocp(0U, &foreign, ERROR_SUCCESS);
    require(
        fixture.state->snapshot().state() ==
            Detail::DashboardConnectionLifecycleState::Closing,
        "foreign operation did not close the connection");
    require(fixture.state->snapshot().socketOperationOutstanding(),
            "foreign operation discarded the exact socket obligation");
    fixture.state->dispatchIocp(
        0U,
        fixture.socket->borrowedOperation(),
        ERROR_OPERATION_ABORTED);
    waitUntil(
        [&fixture] { return fixture.state->isDrained(); },
        "malformed completion path did not drain exact storage");
    require(fixture.state->snapshot().hasFailure(),
            "malformed completion was not retained");
}

[[nodiscard]] Dashboard::DashboardSseFramePair::ImmutableFrame sseFrame(
    const std::uint64_t sequence,
    const std::string_view compact,
    const std::string_view full)
{
    return take(Dashboard::DashboardSseFramePair::create(
        sequence, bytes(compact), bytes(full)));
}

void runsSseBootstrapLatestValuePacingCadenceAndLifetime()
{
    auto control = std::make_shared<SseSubscriptionControl>(2.0);
    auto application = std::make_shared<SseApplication>(control);
    RealStateFixture fixture{application};
    fixture.start();
    fixture.receive(CompleteRequest);
    waitUntil(
        [&fixture] {
            return fixture.state->snapshot().state() ==
                Detail::DashboardConnectionLifecycleState::
                    SendingSseBootstrap;
        },
        "SSE exchange did not enter bootstrap delivery");
    require(application->prepareCalls() == 1U,
            "SSE application prepare was not called once");
    require(fixture.socket->sendIssueCount() == 1U,
            "SSE head did not issue first");
    require(text(fixture.socket->sendIssue(0U)).starts_with(
                "HTTP/1.1 200 OK\r\n"),
            "SSE bootstrap lost its status head");

    const auto headLength = fixture.socket->activeLength();
    require(headLength > 4U, "SSE head was unexpectedly short");
    fixture.completeSend(4U);
    require(fixture.socket->sendIssueCount() == 2U,
            "partial SSE head did not issue its suffix");
    fixture.completeSend(
        static_cast<DWORD>(fixture.socket->activeLength()));
    require(fixture.socket->sendIssueCount() == 3U,
            "SSE connected comment did not follow the head");
    require(
        text(fixture.socket->sendIssue(2U)) ==
            Dashboard::DashboardSseExchange::ConnectedCommentText,
        "SSE connected comment bytes changed");
    fixture.completeSend(
        static_cast<DWORD>(fixture.socket->activeLength()));
    waitUntil(
        [&fixture] {
            return fixture.state->snapshot().state() ==
                Detail::DashboardConnectionLifecycleState::SseIdle;
        },
        "SSE bootstrap did not reach idle");

    control->publish(sseFrame(
        1U, "data: compact-1\n\n", "data: full-1\n\n"));
    waitUntil(
        [&fixture] {
            return fixture.state->snapshot().state() ==
                Detail::DashboardConnectionLifecycleState::SendingSseFrame;
        },
        "first SSE frame did not begin delivery");
    require(text(fixture.socket->sendIssue(3U)) ==
                "data: compact-1\n\n",
            "first SSE delivery was not compact");
    fixture.completeSend(
        static_cast<DWORD>(fixture.socket->activeLength()));
    waitUntil(
        [&fixture] {
            return fixture.state->snapshot().state() ==
                Detail::DashboardConnectionLifecycleState::SseIdle;
        },
        "first SSE frame did not return idle");

    control->publish(sseFrame(
        2U, "data: compact-old\n\n", "data: full-old\n\n"));
    control->publish(sseFrame(
        3U, "data: compact-latest\n\n", "data: full-latest\n\n"));
    waitUntil(
        [&fixture] {
            return fixture.state->snapshot().state() ==
                Detail::DashboardConnectionLifecycleState::SendingSseFrame;
        },
        "paced latest SSE frame did not begin");
    require(text(fixture.socket->sendIssue(4U)) ==
                "data: compact-latest\n\n",
            "capacity-one SSE mailbox did not replace the stale frame");
    fixture.completeSend(
        static_cast<DWORD>(fixture.socket->activeLength()));

    for (std::uint64_t delivery = 3U; delivery <= 10U; ++delivery) {
        waitUntil(
            [&fixture] {
                return fixture.state->snapshot().state() ==
                    Detail::DashboardConnectionLifecycleState::SseIdle;
            },
            "SSE delivery did not return idle");
        const auto compact =
            "data: compact-" + std::to_string(delivery) + "\n\n";
        const auto full =
            "data: full-" + std::to_string(delivery) + "\n\n";
        control->publish(sseFrame(delivery + 1U, compact, full));
        waitUntil(
            [&fixture] {
                return fixture.state->snapshot().state() ==
                    Detail::DashboardConnectionLifecycleState::
                        SendingSseFrame;
            },
            "paced SSE cadence frame did not begin");
        const auto issueIndex = static_cast<std::size_t>(delivery + 2U);
        require(
            text(fixture.socket->sendIssue(issueIndex)) ==
                (delivery == 10U ? full : compact),
            "SSE compact/full delivery cadence changed");
        fixture.completeSend(
            static_cast<DWORD>(fixture.socket->activeLength()));
    }

    waitUntil(
        [&fixture] {
            return fixture.state->snapshot().state() ==
                Detail::DashboardConnectionLifecycleState::SseIdle;
        },
        "tenth SSE frame did not return idle");
    fixture.clock->advance(2h);
    control->publish(sseFrame(
        20U, "data: compact-expired\n\n", "data: full-expired\n\n"));
    waitUntil(
        [&fixture] { return fixture.state->isDrained(); },
        "expired SSE lifetime did not close and drain");
    require(control->isClosed(), "expired SSE subscription was not closed");
    require(fixture.state->snapshot().hasFailure(),
            "SSE lifetime expiry was not retained");
}

void consumesExactHeaderIngressDeadlineAndDrainsCancellation()
{
    RealStateFixture fixture;
    fixture.start();
    require(fixture.deadlineScheduler->cancel(
                fixture.identity.registrationId, 1U),
            "live header arm was not present");
    fixture.state->dispatchDeadline(Windows::WindowsDashboardDeadline{
        fixture.identity.registrationId,
        1U,
        Windows::WindowsDashboardDeadlineKind::HeaderIngress,
        fixture.clock->monotonicNow()});
    require(
        fixture.state->snapshot().state() ==
            Detail::DashboardConnectionLifecycleState::Closing,
        "exact header deadline did not enter Closing");
    require(fixture.state->snapshot().socketOperationOutstanding(),
            "header timeout released receive storage before cancellation");
    fixture.state->dispatchIocp(
        0U,
        fixture.socket->borrowedOperation(),
        ERROR_OPERATION_ABORTED);
    waitUntil(
        [&fixture] { return fixture.state->isDrained(); },
        "header deadline cancellation did not drain");
    const auto failure = fixture.state->fullFailure();
    require(failure.has_value() &&
                failure->code == Domain::ErrorCodes::DeadlineExceeded,
            "header deadline retained the wrong failure");
}

void consumesExactHandlerDeadlineAndCancelsBlockingApplication()
{
    auto application = std::make_shared<BlockingApplication>();
    RealStateFixture fixture{application};
    fixture.start();
    fixture.receive(CompleteRequest);
    waitUntil(
        [&fixture, &application] {
            return application->entered() &&
                fixture.state->snapshot().state() ==
                    Detail::DashboardConnectionLifecycleState::Preparing;
        },
        "blocking handler did not enter Preparing");
    require(fixture.deadlineScheduler->cancel(
                fixture.identity.registrationId, 2U),
            "live handler arm was not present");
    fixture.state->dispatchDeadline(Windows::WindowsDashboardDeadline{
        fixture.identity.registrationId,
        2U,
        Windows::WindowsDashboardDeadlineKind::HandlerExecution,
        fixture.clock->monotonicNow()});
    waitUntil(
        [&fixture] { return fixture.state->isDrained(); },
        "handler deadline did not drain the connection owner");
    waitUntil(
        [&application] { return application->cancelled(); },
        "handler deadline did not cancel blocking application work");
    const auto failure = fixture.state->fullFailure();
    require(failure.has_value() &&
                failure->code == Domain::ErrorCodes::DeadlineExceeded,
            "handler deadline retained the wrong failure");
}

void fatalLatchContentionClosesAfterSnapshotWithoutBlockingOrScheduling()
{
    auto control = std::make_shared<SseSubscriptionControl>(2.0);
    auto application = std::make_shared<SseApplication>(control);
    RealStateFixture fixture{application};
    fixture.start();
    fixture.receive(CompleteRequest);
    waitUntil(
        [&fixture] {
            return fixture.state->snapshot().state() ==
                Detail::DashboardConnectionLifecycleState::
                    SendingSseBootstrap;
        },
        "fatal contention SSE bootstrap did not begin");
    fixture.completeSend(
        static_cast<DWORD>(fixture.socket->activeLength()));
    fixture.completeSend(
        static_cast<DWORD>(fixture.socket->activeLength()));
    waitUntil(
        [&fixture] {
            return fixture.state->snapshot().state() ==
                Detail::DashboardConnectionLifecycleState::SseIdle;
        },
        "fatal contention stream did not reach idle");

    fixture.socket->blockNextStateObservation();
    std::atomic_bool snapshotFinished{};
    std::jthread snapshotThread{[&fixture, &snapshotFinished] {
        static_cast<void>(fixture.state->snapshot());
        snapshotFinished.store(true, std::memory_order_release);
    }};
    fixture.socket->waitUntilStateObservationBlocked();
    require(!snapshotFinished.load(std::memory_order_acquire),
            "snapshot gate did not retain the state mutex");

    const auto before = std::chrono::steady_clock::now();
    fixture.state->fatal(Detail::DashboardConnectionEventFatalNotification{
        fixture.identity.registrationId,
        Detail::DashboardConnectionEventFailure{
            Detail::DashboardConnectionEventFailureKind::InternalFailure,
            false}});
    const auto elapsed = std::chrono::steady_clock::now() - before;
    require(elapsed < 250ms,
            "fatal callback blocked behind the state mutex");
    require(!snapshotFinished.load(std::memory_order_acquire),
            "fatal callback bypassed the deliberate snapshot gate");

    fixture.socket->releaseStateObservation();
    snapshotThread.join();
    waitUntil(
        [&fixture] { return fixture.state->isDrained(); },
        "post-snapshot fatal latch did not close idle SSE");
    require(control->isClosed(),
            "fatal latch did not close the SSE subscription");
    require(fixture.deadlineScheduler->snapshot().scheduledCount() == 0U,
            "fatal latch created or retained a scheduler wake");
}

void sendsFixed503WithoutActionWhenReservationCapacityIsExhausted()
{
    auto application = std::make_shared<GatedActionApplication>();
    RealStateFixture fixture{application};
    fixture.start();
    fixture.receive(CompleteRequest);
    application->waitUntilEntered();
    std::vector<Windows::WindowsDashboardHandlerExecutor::Reservation>
        reservations;
    reservations.reserve(
        Windows::WindowsDashboardHandlerExecutor::QueueCapacity);
    for (std::size_t index{};
         index < Windows::WindowsDashboardHandlerExecutor::QueueCapacity;
         ++index) {
        reservations.push_back(take(
            fixture.handlerExecutor->tryReservePostDelivery()));
    }
    require(
        fixture.handlerExecutor->reservationCount() ==
            Windows::WindowsDashboardHandlerExecutor::QueueCapacity,
        "test did not exhaust post-delivery reservation capacity");
    application->release();
    waitUntil(
        [&fixture] {
            return fixture.state->snapshot().state() ==
                Detail::DashboardConnectionLifecycleState::SendingComplete;
        },
        "reservation exhaustion did not produce a fixed response");
    const auto response = text(fixture.socket->sendIssue(0U));
    require(response.starts_with("HTTP/1.1 503 Service Unavailable\r\n"),
            "reservation exhaustion did not send the fixed 503");
    require(response.find("must-not-be-sent") == std::string::npos,
            "reservation exhaustion began the action-bearing response");
    require(application->postCalls() == 0U,
            "reservation exhaustion executed post-delivery work");
    fixture.completeSend(
        static_cast<DWORD>(fixture.socket->activeLength()));
    waitUntil(
        [&fixture] { return fixture.state->isDrained(); },
        "fixed 503 connection did not drain");
    reservations.clear();
    require(fixture.handlerExecutor->reservationCount() == 0U,
            "test reservations did not release executor capacity");
}

} // namespace

int main()
{
    try {
        exposesEveryExactLifecycleState();
        exposesMockableFixedDispatchBoundary();
        publishesBoundedTimingContracts();
        runsRealIngressPreparePartialSendAndDrain();
        reservesBeforeByteOneAndRunsPostDeliveryAfterSuccess();
        suppressesReservedActionWhenSendFails();
        sendsFixed503WithoutActionWhenReservationCapacityIsExhausted();
        ignoresStaleDeadlineAndCancellationDrainsOutstandingReceive();
        malformedForeignCompletionClosesButRetainsExactSocketObligation();
        runsSseBootstrapLatestValuePacingCadenceAndLifetime();
        consumesExactHeaderIngressDeadlineAndDrainsCancellation();
        consumesExactHandlerDeadlineAndCancelsBlockingApplication();
        fatalLatchContentionClosesAfterSnapshotWithoutBlockingOrScheduling();
        std::cout << "Dashboard connection state tests passed ("
                  << assertions << " assertions).\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Dashboard connection state tests failed: "
                  << error.what() << '\n';
        return 1;
    } catch (...) {
        std::cerr << "Dashboard connection state tests failed with an "
                     "unknown error.\n";
        return 1;
    }
}
