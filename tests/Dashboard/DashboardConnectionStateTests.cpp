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
        std::unique_lock lock{mutex_};
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
        if (blockNextReapAfterIdle_) {
            reapAfterIdleBlocked_ = true;
            stateGateChanged_.notify_all();
            stateGateChanged_.wait(lock, [this] {
                return releaseReapAfterIdle_;
            });
            blockNextReapAfterIdle_ = false;
            reapAfterIdleBlocked_ = false;
            releaseReapAfterIdle_ = false;
        }
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

    void blockNextReapAfterIdle() noexcept
    {
        const std::scoped_lock lock{mutex_};
        blockNextReapAfterIdle_ = true;
        reapAfterIdleBlocked_ = false;
        releaseReapAfterIdle_ = false;
    }

    void waitUntilReapAfterIdleBlocked()
    {
        std::unique_lock lock{mutex_};
        if (!stateGateChanged_.wait_for(lock, 5s, [this] {
                return reapAfterIdleBlocked_;
            })) {
            throw std::runtime_error{
                "socket reap did not enter its post-idle gate"};
        }
    }

    void releaseReapAfterIdle() noexcept
    {
        const std::scoped_lock lock{mutex_};
        releaseReapAfterIdle_ = true;
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
    bool blockNextReapAfterIdle_{};
    bool reapAfterIdleBlocked_{};
    bool releaseReapAfterIdle_{};
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
            std::unique_lock lock{mutex_};
            if (blockNextConsume_) {
                consumeBlocked_ = true;
                changed_.notify_all();
                changed_.wait(lock, [this] { return releaseConsume_; });
                blockNextConsume_ = false;
                consumeBlocked_ = false;
                releaseConsume_ = false;
            }
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

    void blockNextConsume() noexcept
    {
        const std::scoped_lock lock{mutex_};
        blockNextConsume_ = true;
        consumeBlocked_ = false;
        releaseConsume_ = false;
    }

    void waitUntilConsumeBlocked()
    {
        std::unique_lock lock{mutex_};
        if (!changed_.wait_for(lock, 5s, [this] { return consumeBlocked_; })) {
            throw std::runtime_error{
                "kernel sink did not retain the prepared completion"};
        }
    }

    void releaseConsume() noexcept
    {
        const std::scoped_lock lock{mutex_};
        releaseConsume_ = true;
        changed_.notify_all();
    }

private:
    mutable std::mutex mutex_;
    std::condition_variable changed_;
    std::weak_ptr<Detail::IDashboardConnectionDispatchTarget> target_;
    std::atomic<DWORD> fatalError_{};
    bool blockNextConsume_{};
    bool consumeBlocked_{};
    bool releaseConsume_{};
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

class RecordingConnectionDrainObserver final
    : public Detail::IDashboardConnectionDrainObserver {
public:
    void attachTarget(
        std::weak_ptr<Detail::IDashboardConnectionDispatchTarget> target)
        noexcept
    {
        const std::scoped_lock lock{mutex_};
        target_ = std::move(target);
    }

    void connectionMayHaveDrained(
        const Detail::DashboardIoCompletionKey completionKey,
        const std::uint64_t registrationId,
        const std::uint64_t generationId) noexcept override
    {
        std::shared_ptr<Detail::IDashboardConnectionDispatchTarget> target;
        {
            const std::scoped_lock lock{mutex_};
            completionKey_ = completionKey;
            registrationId_ = registrationId;
            generationId_ = generationId;
            ++callbackCount_;
            target = target_.lock();
        }
        if (target != nullptr) {
            static_cast<void>(target->snapshot());
            reentered_.store(true, std::memory_order_release);
        }
    }

    [[nodiscard]] std::size_t callbackCount() const noexcept
    {
        const std::scoped_lock lock{mutex_};
        return callbackCount_;
    }

    [[nodiscard]] Detail::DashboardIoCompletionKey completionKey()
        const noexcept
    {
        const std::scoped_lock lock{mutex_};
        return completionKey_;
    }

    [[nodiscard]] std::uint64_t registrationId() const noexcept
    {
        const std::scoped_lock lock{mutex_};
        return registrationId_;
    }

    [[nodiscard]] std::uint64_t generationId() const noexcept
    {
        const std::scoped_lock lock{mutex_};
        return generationId_;
    }

    [[nodiscard]] bool reentered() const noexcept
    {
        return reentered_.load(std::memory_order_acquire);
    }

private:
    mutable std::mutex mutex_;
    std::weak_ptr<Detail::IDashboardConnectionDispatchTarget> target_;
    Detail::DashboardIoCompletionKey completionKey_{0U};
    std::uint64_t registrationId_{};
    std::uint64_t generationId_{};
    std::size_t callbackCount_{};
    std::atomic_bool reentered_{};
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
          deadlineSink{std::make_shared<ForwardingDeadlineSink>()},
          drainObserver{
              std::make_shared<RecordingConnectionDrainObserver>()}
    {
        runtimeServices = take(
            Detail::DashboardConnectionRuntimeServices::create(
                clock,
                uuidGenerator,
                operationalState));
        admissionController = take(
            Detail::DashboardAdmissionController::create());
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
        auto admissionLease = take(admissionController->tryAccept());
        admittedAt = clock->monotonicNow();
        state = take(Detail::DashboardConnectionState::create(
            3U,
            identity,
            admittedAt,
            std::move(admissionLease),
            std::move(io),
            *kernel,
            *deadlineScheduler,
            *handlerExecutor,
            *runtimeServices,
            application,
            *responseCatalog));
        auto drainBound = state->bindDrainObserver(drainObserver);
        require(static_cast<bool>(drainBound),
                "real state drain observer failed to bind");
        drainObserver->attachTarget(state);
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
    std::shared_ptr<RecordingConnectionDrainObserver> drainObserver;
    std::unique_ptr<Detail::DashboardConnectionRuntimeServices>
        runtimeServices;
    std::unique_ptr<Detail::DashboardAdmissionController>
        admissionController;
    std::unique_ptr<Detail::DashboardConnectionResponseCatalog>
        responseCatalog;
    std::unique_ptr<Windows::WindowsDashboardHandlerExecutor>
        handlerExecutor;
    std::unique_ptr<Windows::WindowsDashboardDeadlineScheduler>
        deadlineScheduler;
    std::unique_ptr<Detail::DashboardIocpWorkerKernel> kernel;
    Detail::DashboardConnectionRuntimeIdentity identity{};
    Domain::MonotonicTimePoint admittedAt{};
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

    [[nodiscard]] Domain::Result<void> bindDrainObserver(
        std::weak_ptr<Detail::IDashboardConnectionDrainObserver>)
        noexcept override
    {
        return Domain::Result<void>::success();
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
                           .bindDrainObserver({})));
static_assert(noexcept(std::declval<
                       Detail::IDashboardConnectionDispatchTarget&>()
                           .dispatchIocp(0U, nullptr, ERROR_SUCCESS)));
static_assert(noexcept(std::declval<
                       Detail::IDashboardConnectionDispatchTarget&>()
                           .beginGracefulShutdown()));
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

void gracefulShutdownKeepsOnlyPreparedPartialFinalSend()
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
        "graceful test response was not prepared");
    const auto initial = fixture.socket->sendIssue(0U);
    require(initial.size() > 7U,
            "graceful final response was unexpectedly short");
    require(fixture.handlerExecutor->reservationCount() == 1U,
            "graceful action response did not reserve post-delivery work");

    fixture.state->beginGracefulShutdown();
    const auto graceful = fixture.state->snapshot();
    require(
        graceful.state() ==
                Detail::DashboardConnectionLifecycleState::SendingComplete &&
            graceful.shutdownRequested(),
        "graceful shutdown did not retain only the prepared send");
    require(fixture.handlerExecutor->reservationCount() == 0U,
            "graceful shutdown retained post-delivery capacity");
    require(!fixture.socket->snapshot().shutdownRequested(),
            "graceful shutdown half-closed the retained final send");

    fixture.completeSend(7U);
    require(fixture.socket->sendIssueCount() == 2U,
            "graceful partial completion did not reissue its suffix");
    const auto suffix = fixture.socket->sendIssue(1U);
    require(suffix.size() == initial.size() - 7U &&
                std::equal(
                    suffix.begin(), suffix.end(), initial.begin() + 7),
            "graceful partial send changed its immutable suffix");
    fixture.completeSend(static_cast<DWORD>(suffix.size()));
    waitUntil(
        [&fixture] { return fixture.state->isDrained(); },
        "graceful final send did not drain exactly");
    require(fixture.completeApplication->postCalls() == 0U,
            "graceful final send admitted post-delivery work");
}

void gracefulShutdownRacingPostNativePartialSendReissuesExactSuffix()
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
        "racing graceful response was not prepared");

    constexpr DWORD PartialDelivery = 7U;
    const auto initial = fixture.socket->sendIssue(0U);
    require(initial.size() > PartialDelivery,
            "racing graceful response was unexpectedly short");
    require(fixture.handlerExecutor->reservationCount() == 1U,
            "racing graceful response did not reserve its action");

    fixture.socket->blockNextReapAfterIdle();
    std::jthread completion{[&fixture] {
        fixture.completeSend(PartialDelivery);
    }};
    try {
        fixture.socket->waitUntilReapAfterIdleBlocked();
        require(
            fixture.socket->state() ==
                Detail::DashboardConnectionSocketState::Idle,
            "partial native reap did not expose the idle cutover");

        fixture.state->beginGracefulShutdown();
        const auto graceful = fixture.state->snapshot();
        require(
            graceful.state() ==
                    Detail::DashboardConnectionLifecycleState::
                        SendingComplete &&
                graceful.shutdownRequested() &&
                fixture.socket->sendIssueCount() == 1U,
            "graceful cutoff did not retain only the reaped final send");
        require(fixture.handlerExecutor->reservationCount() == 0U &&
                    fixture.completeApplication->postCalls() == 0U,
                "graceful cutoff retained post-delivery work");
        require(!fixture.socket->snapshot().shutdownRequested(),
                "graceful cutoff closed the socket before suffix reissue");
    } catch (...) {
        fixture.socket->releaseReapAfterIdle();
        completion.join();
        throw;
    }

    fixture.socket->releaseReapAfterIdle();
    completion.join();
    require(fixture.socket->sendIssueCount() == 2U,
            "late partial dispatch did not reissue one exact suffix");
    const auto suffix = fixture.socket->sendIssue(1U);
    require(
        suffix.size() == initial.size() - PartialDelivery &&
            std::equal(
                suffix.begin(),
                suffix.end(),
                initial.begin() + PartialDelivery),
        "late partial dispatch changed its immutable suffix");
    require(fixture.completeApplication->postCalls() == 0U,
            "late partial dispatch admitted post-delivery work");

    fixture.completeSend(static_cast<DWORD>(suffix.size()));
    waitUntil(
        [&fixture] { return fixture.state->isDrained(); },
        "late partial suffix did not drain");
    require(fixture.drainObserver->callbackCount() == 1U &&
                fixture.completeApplication->postCalls() == 0U,
            "late partial suffix did not publish one action-free drain");
}

void gracefulShutdownRejectsEveryNonFinalSendCutoff()
{
    {
        RealStateFixture fixture;
        fixture.state->beginGracefulShutdown();
        require(fixture.state->isDrained() &&
                    fixture.state->snapshot().shutdownRequested(),
                "Created survived graceful shutdown");
    }
    {
        RealStateFixture fixture;
        fixture.start();
        fixture.state->beginGracefulShutdown();
        require(
            fixture.state->snapshot().state() ==
                Detail::DashboardConnectionLifecycleState::Closing,
            "Receiving survived graceful shutdown");
        fixture.state->dispatchIocp(
            0U,
            fixture.socket->borrowedOperation(),
            ERROR_OPERATION_ABORTED);
        require(fixture.state->isDrained(),
                "graceful receive cancellation did not drain");
    }
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
            "graceful cutoff did not enter Preparing");
        fixture.state->beginGracefulShutdown();
        waitUntil(
            [&application] { return application->cancelled(); },
            "graceful cutoff did not cancel preparation");
        require(fixture.state->isDrained() &&
                    fixture.socket->sendIssueCount() == 0U,
                "Preparing produced a response after graceful cutoff");
    }
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
            "graceful cutoff did not enter SSE bootstrap");
        fixture.state->beginGracefulShutdown();
        require(control->isClosed(),
                "graceful cutoff retained its SSE subscription");
        require(
            fixture.state->snapshot().state() ==
                Detail::DashboardConnectionLifecycleState::Closing,
            "SSE bootstrap survived graceful shutdown");
        fixture.state->dispatchIocp(
            0U,
            fixture.socket->borrowedOperation(),
            ERROR_OPERATION_ABORTED);
        require(fixture.state->isDrained(),
                "graceful SSE cancellation did not drain");
    }
}

void queuedPreparedCompletionCannotCrossGracefulCutoff()
{
    RealStateFixture fixture;
    fixture.kernelSink->blockNextConsume();
    fixture.start();
    fixture.receive(CompleteRequest);
    fixture.kernelSink->waitUntilConsumeBlocked();
    require(fixture.completeApplication->prepareCalls() == 1U &&
                fixture.state->snapshot().state() ==
                    Detail::DashboardConnectionLifecycleState::Preparing,
            "prepared completion did not remain queued at the cutoff");

    fixture.state->beginGracefulShutdown();
    const auto closing = fixture.state->snapshot();
    require(
        closing.state() ==
                Detail::DashboardConnectionLifecycleState::Closing &&
            closing.eventOperationOutstanding() &&
            fixture.socket->sendIssueCount() == 0U,
        "queued prepared completion crossed graceful cutoff");
    fixture.kernelSink->releaseConsume();
    waitUntil(
        [&fixture] { return fixture.state->isDrained(); },
        "queued prepared tombstone did not drain");
    require(fixture.socket->sendIssueCount() == 0U,
            "retired prepared completion issued a late response");
}

void runtimeShutdownRejectsReleasedPrepareResultBeforeDelivery()
{
    auto application = std::make_shared<GatedActionApplication>();
    RealStateFixture fixture{application};
    fixture.start();
    fixture.receive(CompleteRequest);
    application->waitUntilEntered();
    require(
        fixture.state->snapshot().state() ==
                Detail::DashboardConnectionLifecycleState::Preparing &&
            fixture.socket->sendIssueCount() == 0U,
        "runtime cutoff test did not retain a blocked prepare");

    fixture.runtimeServices->beginShutdown();
    require(fixture.runtimeServices->isShuttingDown(),
            "runtime admission did not close at shutdown");
    application->release();
    waitUntil(
        [&fixture] {
            return fixture.state->isDrained() &&
                fixture.drainObserver->callbackCount() == 1U;
        },
        "released prepare result did not drain after runtime cutoff");
    require(fixture.socket->sendIssueCount() == 0U,
            "released prepare result sent complete or fallback bytes");
    require(fixture.handlerExecutor->reservationCount() == 0U &&
                application->postCalls() == 0U &&
                fixture.drainObserver->callbackCount() == 1U,
            "released prepare result retained work past its exact drain");
}

void runtimeShutdownRejectsFallbackFromNewlyParsedRequest()
{
    RealStateFixture fixture;
    fixture.start();
    fixture.runtimeServices->beginShutdown();
    fixture.receive(CompleteRequest);

    waitUntil(
        [&fixture] {
            return fixture.state->isDrained() &&
                fixture.drainObserver->callbackCount() == 1U;
        },
        "post-cutoff request did not drain after context rejection");
    require(fixture.socket->sendIssueCount() == 0U &&
                fixture.completeApplication->prepareCalls() == 0U,
            "post-cutoff context rejection started fallback or handler work");
}

void hardShutdownEscalatesAGracefulFinalSend()
{
    RealStateFixture fixture;
    fixture.start();
    fixture.receive(CompleteRequest);
    waitUntil(
        [&fixture] {
            return fixture.state->snapshot().state() ==
                Detail::DashboardConnectionLifecycleState::SendingComplete;
        },
        "hard-escalation response was not prepared");
    fixture.state->beginGracefulShutdown();
    require(
        fixture.state->snapshot().state() ==
            Detail::DashboardConnectionLifecycleState::SendingComplete,
        "graceful phase did not retain the final send");

    fixture.state->beginShutdown();
    require(
        fixture.state->snapshot().state() ==
                Detail::DashboardConnectionLifecycleState::Closing &&
            fixture.socket->state() ==
                Detail::DashboardConnectionSocketState::
                    SendCancellationRequested,
        "hard escalation did not cancel the retained send");
    fixture.state->dispatchIocp(
        0U,
        fixture.socket->borrowedOperation(),
        ERROR_OPERATION_ABORTED);
    require(fixture.state->isDrained(),
            "hard-escalated final send did not drain exact storage");
}

void hardEscalationRacingPostNativeFullSendDrainsLateDispatch()
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
        "racing hard-escalation response was not prepared");

    const auto initial = fixture.socket->sendIssue(0U);
    require(!initial.empty() &&
                fixture.handlerExecutor->reservationCount() == 1U,
            "racing hard-escalation response was not fully owned");
    fixture.socket->blockNextReapAfterIdle();
    std::jthread completion{[&fixture, size = initial.size()] {
        fixture.completeSend(static_cast<DWORD>(size));
    }};
    try {
        fixture.socket->waitUntilReapAfterIdleBlocked();
        require(
            fixture.socket->state() ==
                Detail::DashboardConnectionSocketState::Idle,
            "full native reap did not expose the idle cutover");

        fixture.state->beginGracefulShutdown();
        require(
            fixture.state->snapshot().state() ==
                    Detail::DashboardConnectionLifecycleState::
                        SendingComplete &&
                fixture.handlerExecutor->reservationCount() == 0U &&
                fixture.completeApplication->postCalls() == 0U,
            "graceful phase retained action work during full reap");

        fixture.state->beginShutdown();
        require(fixture.state->isDrained() &&
                    fixture.socket->snapshot().shutdownRequested() &&
                    fixture.socket->sendIssueCount() == 1U &&
                    fixture.drainObserver->callbackCount() == 1U,
                "hard escalation did not publish the exact idle drain");
    } catch (...) {
        fixture.socket->releaseReapAfterIdle();
        completion.join();
        throw;
    }

    fixture.socket->releaseReapAfterIdle();
    completion.join();
    require(fixture.state->isDrained() &&
                fixture.socket->sendIssueCount() == 1U &&
                fixture.drainObserver->callbackCount() == 1U &&
                fixture.completeApplication->postCalls() == 0U,
            "late full-send dispatch changed the drained state");
    require(!fixture.state->fullFailure().has_value(),
            "late full-send dispatch retained a false failure");
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
    const auto streamAdmission = take(
        fixture.admissionController->snapshot());
    require(streamAdmission.shortConnectionCount() == 0U &&
                streamAdmission.sseConnectionCount() == 1U,
            "SSE preparation did not convert the owned admission lease");
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
    const auto drainedAdmission = take(
        fixture.admissionController->snapshot());
    require(drainedAdmission.totalConnectionCount() == 0U,
            "drained SSE state retained its admission lease");
}

void cleanShutdownRacingPostNativeReapDoesNotRecordFailure()
{
    RealStateFixture fixture;
    fixture.start();
    fixture.socket->stageReceive(bytes("x"));
    fixture.socket->blockNextReapAfterIdle();
    std::jthread completion{[&fixture] {
        fixture.state->dispatchIocp(
            1U,
            fixture.socket->borrowedOperation(),
            ERROR_SUCCESS);
    }};
    fixture.socket->waitUntilReapAfterIdleBlocked();

    fixture.state->beginShutdown();
    require(fixture.state->isDrained(),
            "shutdown did not drain after native reap released the socket");
    fixture.socket->releaseReapAfterIdle();
    completion.join();
    require(!fixture.state->fullFailure().has_value(),
            "post-drain socket delivery retained a false integrity failure");
}

void admissionDelayCannotRestartTheHeaderIngressCeiling()
{
    RealStateFixture fixture;
    fixture.clock->advance(
        Detail::DashboardConnectionState::HeaderIngressLifetime);
    const auto started = fixture.state->start();
    require(!started &&
                started.error().code ==
                    Domain::ErrorCodes::DeadlineExceeded,
            "expired admission received a fresh header-ingress window");
    require(fixture.state->isDrained() &&
                fixture.socket->state() ==
                    Detail::DashboardConnectionSocketState::Idle,
            "pre-start ingress expiry issued native socket work");
    const auto failure = fixture.state->fullFailure();
    require(failure.has_value() &&
                failure->code == Domain::ErrorCodes::DeadlineExceeded,
            "pre-start ingress expiry did not retain its exact cause");
}

void sendsFixed503WhenSseAdmissionConversionIsExhausted()
{
    auto control = std::make_shared<SseSubscriptionControl>(2.0);
    auto application = std::make_shared<SseApplication>(control);
    RealStateFixture fixture{application};

    std::vector<Detail::DashboardAdmissionController::Lease> blockers;
    blockers.reserve(
        fixture.admissionController->limits().maximumSseConnections);
    for (std::size_t index{};
         index < fixture.admissionController->limits().maximumSseConnections;
        ++index) {
        auto lease = take(fixture.admissionController->tryAccept());
        const auto converted = lease.convertToSse();
        require(converted.hasValue(),
                "test could not saturate SSE admission");
        blockers.push_back(std::move(lease));
    }

    fixture.start();
    fixture.receive(CompleteRequest);
    waitUntil(
        [&fixture] {
            return fixture.state->snapshot().state() ==
                Detail::DashboardConnectionLifecycleState::SendingComplete;
        },
        "SSE admission exhaustion did not begin a fixed response");
    const auto response = text(fixture.socket->sendIssue(0U));
    require(response.starts_with(
                "HTTP/1.1 503 Service Unavailable\r\n"),
            "SSE admission exhaustion did not send the fixed 503");
    const auto saturated = take(
        fixture.admissionController->snapshot());
    require(saturated.shortConnectionCount() == 1U &&
                saturated.sseConnectionCount() ==
                    fixture.admissionController->limits().
                        maximumSseConnections,
            "failed SSE conversion corrupted admission accounting");

    fixture.completeSend(
        static_cast<DWORD>(fixture.socket->activeLength()));
    waitUntil(
        [&fixture] { return fixture.state->isDrained(); },
        "SSE admission exhaustion response did not drain");
    const auto afterDrain = take(
        fixture.admissionController->snapshot());
    require(afterDrain.shortConnectionCount() == 0U &&
                afterDrain.sseConnectionCount() == blockers.size(),
            "drained fallback state retained its short admission");
    blockers.clear();
    require(take(fixture.admissionController->snapshot()).
                totalConnectionCount() == 0U,
            "SSE saturation fixtures did not release admission");
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

void offRegistryFatalPublishesOneExactDrainEdgeAfterUnlock()
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
        "drain-edge SSE bootstrap did not begin");
    fixture.completeSend(
        static_cast<DWORD>(fixture.socket->activeLength()));
    fixture.completeSend(
        static_cast<DWORD>(fixture.socket->activeLength()));
    waitUntil(
        [&fixture] {
            return fixture.state->snapshot().state() ==
                Detail::DashboardConnectionLifecycleState::SseIdle;
        },
        "drain-edge stream did not reach idle");

    fixture.state->fatal(
        Detail::DashboardConnectionEventFatalNotification{
            fixture.identity.registrationId,
            Detail::DashboardConnectionEventFailure{
                Detail::DashboardConnectionEventFailureKind::
                    LimitExceeded,
                true}});
    waitUntil(
        [&fixture] { return fixture.state->isDrained(); },
        "off-registry fatal did not drain the idle connection");
    require(fixture.drainObserver->callbackCount() == 1U &&
                fixture.drainObserver->completionKey() ==
                    fixture.identity.completionKey &&
                fixture.drainObserver->registrationId() ==
                    fixture.identity.registrationId &&
                fixture.drainObserver->generationId() == 3U,
            "off-registry fatal lost or duplicated exact drain identity");
    require(fixture.drainObserver->reentered(),
            "connection drain observer ran while the state lock was held");

    fixture.state->beginShutdown();
    fixture.state->fatal(
        Detail::DashboardConnectionEventFatalNotification{
            fixture.identity.registrationId,
            Detail::DashboardConnectionEventFailure{
                Detail::DashboardConnectionEventFailureKind::
                    InternalFailure,
                false}});
    require(fixture.drainObserver->callbackCount() == 1U,
            "terminal connection redelivered its one-shot drain edge");
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
        admissionDelayCannotRestartTheHeaderIngressCeiling();
        runsRealIngressPreparePartialSendAndDrain();
        gracefulShutdownKeepsOnlyPreparedPartialFinalSend();
        gracefulShutdownRacingPostNativePartialSendReissuesExactSuffix();
        gracefulShutdownRejectsEveryNonFinalSendCutoff();
        queuedPreparedCompletionCannotCrossGracefulCutoff();
        runtimeShutdownRejectsReleasedPrepareResultBeforeDelivery();
        runtimeShutdownRejectsFallbackFromNewlyParsedRequest();
        hardShutdownEscalatesAGracefulFinalSend();
        hardEscalationRacingPostNativeFullSendDrainsLateDispatch();
        reservesBeforeByteOneAndRunsPostDeliveryAfterSuccess();
        suppressesReservedActionWhenSendFails();
        sendsFixed503WithoutActionWhenReservationCapacityIsExhausted();
        ignoresStaleDeadlineAndCancellationDrainsOutstandingReceive();
        cleanShutdownRacingPostNativeReapDoesNotRecordFailure();
        malformedForeignCompletionClosesButRetainsExactSocketObligation();
        runsSseBootstrapLatestValuePacingCadenceAndLifetime();
        sendsFixed503WhenSseAdmissionConversionIsExhausted();
        consumesExactHeaderIngressDeadlineAndDrainsCancellation();
        consumesExactHandlerDeadlineAndCancelsBlockingApplication();
        fatalLatchContentionClosesAfterSnapshotWithoutBlockingOrScheduling();
        offRegistryFatalPublishesOneExactDrainEdgeAfterUnlock();
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
