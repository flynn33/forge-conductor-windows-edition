#include "ForgeConductor/SessionHost/BoundedLogicalContinuationQueue.h"

#include "ForgeConductor/Domain/Utf8.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <deque>
#include <mutex>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>

namespace ForgeConductor::SessionHost {
namespace {

constexpr std::size_t MaximumContinuationFieldBytes = 4U * 1024U;

template <typename T>
[[nodiscard]] Domain::Result<T> failure(
    const std::string_view code,
    std::string message)
{
    return Domain::Result<T>::failure(
        Domain::makeError(code, std::move(message)));
}

[[nodiscard]] Domain::Result<void> validateContext(
    const Domain::OperationContext& context,
    const std::atomic_bool& shutdownRequested) noexcept
{
    if (shutdownRequested.load(std::memory_order_acquire) ||
        context.isCancellationRequested()) {
        return Domain::Result<void>::failure(Domain::makeError(
            Domain::ErrorCodes::Cancelled,
            "The logical continuation queue operation was cancelled."));
    }
    if (context.isExpired(std::chrono::steady_clock::now())) {
        return Domain::Result<void>::failure(Domain::makeError(
            Domain::ErrorCodes::DeadlineExceeded,
            "The logical continuation queue operation exceeded its deadline."));
    }
    return Domain::Result<void>::success();
}

[[nodiscard]] bool exactMatch(
    const Domain::NativeLogicalContinuation& left,
    const Domain::NativeLogicalContinuation& right) noexcept
{
    return left.providerSessionId == right.providerSessionId &&
           left.handoffId == right.handoffId &&
           left.sequence == right.sequence &&
           left.action == right.action && left.command == right.command &&
           left.successCondition == right.successCondition;
}

[[nodiscard]] Domain::Result<void> validateContinuation(
    const Domain::NativeLogicalContinuation& continuation) noexcept
{
    try {
        if (continuation.sequence == 0U || continuation.action.empty() ||
            continuation.successCondition.empty()) {
            return Domain::Result<void>::failure(Domain::makeError(
                Domain::ErrorCodes::InvalidRequest,
                "A logical continuation requires a positive sequence, action, "
                "and success condition."));
        }
        if (continuation.action.size() > MaximumContinuationFieldBytes ||
            continuation.command.size() > MaximumContinuationFieldBytes ||
            continuation.successCondition.size() >
                MaximumContinuationFieldBytes) {
            return Domain::Result<void>::failure(Domain::makeError(
                Domain::ErrorCodes::PayloadTooLarge,
                "A logical continuation field exceeds its UTF-8 byte limit."));
        }
        if (continuation.action.find('\0') != std::string::npos ||
            continuation.command.find('\0') != std::string::npos ||
            continuation.successCondition.find('\0') != std::string::npos ||
            !Domain::isValidUtf8(continuation.action) ||
            !Domain::isValidUtf8(continuation.command) ||
            !Domain::isValidUtf8(continuation.successCondition)) {
            return Domain::Result<void>::failure(Domain::makeError(
                Domain::ErrorCodes::InvalidRequest,
                "A logical continuation field contains U+0000 or invalid UTF-8."));
        }
        return Domain::Result<void>::success();
    } catch (...) {
        return Domain::Result<void>::failure(Domain::makeError(
            Domain::ErrorCodes::InternalFailure,
            "The logical continuation could not be validated safely."));
    }
}

} // namespace

class BoundedLogicalContinuationQueue::Impl final {
public:
    explicit Impl(const std::size_t requestedCapacity)
        : capacity{requestedCapacity}
    {
        if (capacity == 0U ||
            capacity > Domain::MaximumNativeSessionRecords) {
            throw std::invalid_argument(
                "Logical continuation queue capacity is outside its bounds.");
        }
    }

    [[nodiscard]] Domain::Result<void> schedule(
        const Domain::NativeLogicalContinuation& continuation,
        const Domain::OperationContext& context) noexcept
    {
        auto valid = validateContext(context, shutdownRequested);
        if (!valid) {
            return valid;
        }
        valid = validateContinuation(continuation);
        if (!valid) {
            return valid;
        }
        try {
            std::lock_guard lock{mutex};
            valid = validateContext(context, shutdownRequested);
            if (!valid) {
                return valid;
            }
            const auto key = continuation.providerSessionId.value();
            const auto existing = bindings.find(key);
            if (existing != bindings.end()) {
                if (exactMatch(existing->second, continuation)) {
                    return Domain::Result<void>::success();
                }
                return Domain::Result<void>::failure(Domain::makeError(
                    Domain::ErrorCodes::Conflict,
                    "The logical session is already bound to another continuation."));
            }
            if (pending.size() >= capacity ||
                bindings.size() >= Domain::MaximumNativeSessionRecords) {
                return Domain::Result<void>::failure(Domain::makeError(
                    Domain::ErrorCodes::StorageFull,
                    "The logical continuation queue reached its configured limit."));
            }
            const auto [binding, inserted] = bindings.emplace(key, continuation);
            if (!inserted) {
                return Domain::Result<void>::failure(Domain::makeError(
                    Domain::ErrorCodes::IntegrityFailure,
                    "The logical continuation queue could not reserve its binding."));
            }
            try {
                pending.push_back(key);
            } catch (...) {
                bindings.erase(binding);
                throw;
            }
            return Domain::Result<void>::success();
        } catch (...) {
            return Domain::Result<void>::failure(Domain::makeError(
                Domain::ErrorCodes::InternalFailure,
                "The logical continuation could not be scheduled safely."));
        }
    }

    [[nodiscard]] Domain::Result<
        std::optional<Domain::NativeLogicalContinuation>> takeNext(
        const Domain::OperationContext& context) noexcept
    {
        auto valid = validateContext(context, shutdownRequested);
        if (!valid) {
            return failure<std::optional<Domain::NativeLogicalContinuation>>(
                valid.error().code, valid.error().message);
        }
        try {
            std::lock_guard lock{mutex};
            valid = validateContext(context, shutdownRequested);
            if (!valid) {
                return failure<
                    std::optional<Domain::NativeLogicalContinuation>>(
                    valid.error().code, valid.error().message);
            }
            if (pending.empty()) {
                return Domain::Result<
                    std::optional<Domain::NativeLogicalContinuation>>::success(
                    std::nullopt);
            }
            const auto accepted = bindings.find(pending.front());
            if (accepted == bindings.end()) {
                return failure<
                    std::optional<Domain::NativeLogicalContinuation>>(
                    Domain::ErrorCodes::IntegrityFailure,
                    "The logical continuation queue lost an accepted binding.");
            }
            auto result = Domain::Result<
                std::optional<Domain::NativeLogicalContinuation>>::success(
                accepted->second);
            pending.pop_front();
            return result;
        } catch (...) {
            return failure<std::optional<Domain::NativeLogicalContinuation>>(
                Domain::ErrorCodes::InternalFailure,
                "The logical continuation could not be dequeued safely.");
        }
    }

    [[nodiscard]] std::size_t pendingCount() const noexcept
    {
        try {
            std::lock_guard lock{mutex};
            return pending.size();
        } catch (...) {
            return 0U;
        }
    }

    void cancel(const Domain::ProviderSessionId& sessionId) noexcept
    {
        try {
            std::lock_guard lock{mutex};
            const auto& key = sessionId.value();
            pending.erase(
                std::remove(pending.begin(), pending.end(), key),
                pending.end());
            bindings.erase(key);
        } catch (...) {
        }
    }

    void shutdown() noexcept
    {
        shutdownRequested.store(true, std::memory_order_release);
        try {
            std::lock_guard lock{mutex};
            pending.clear();
            bindings.clear();
        } catch (...) {
        }
    }

    const std::size_t capacity;
    mutable std::mutex mutex;
    std::unordered_map<std::string, Domain::NativeLogicalContinuation> bindings;
    std::deque<std::string> pending;
    std::atomic_bool shutdownRequested{};
};

BoundedLogicalContinuationQueue::BoundedLogicalContinuationQueue(
    const std::size_t capacity)
    : implementation_{std::make_unique<Impl>(capacity)}
{
}

BoundedLogicalContinuationQueue::~BoundedLogicalContinuationQueue() noexcept
{
    shutdown();
}

Domain::Result<void> BoundedLogicalContinuationQueue::schedule(
    const Domain::NativeLogicalContinuation& continuation,
    const Domain::OperationContext& context) noexcept
{
    return implementation_->schedule(continuation, context);
}

Domain::Result<std::optional<Domain::NativeLogicalContinuation>>
BoundedLogicalContinuationQueue::takeNext(
    const Domain::OperationContext& context) noexcept
{
    return implementation_->takeNext(context);
}

std::size_t BoundedLogicalContinuationQueue::pendingCount() const noexcept
{
    return implementation_->pendingCount();
}

void BoundedLogicalContinuationQueue::cancel(
    const Domain::ProviderSessionId& sessionId) noexcept
{
    implementation_->cancel(sessionId);
}

void BoundedLogicalContinuationQueue::shutdown() noexcept
{
    if (implementation_) {
        implementation_->shutdown();
    }
}

} // namespace ForgeConductor::SessionHost
