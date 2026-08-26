#pragma once

#include "ForgeConductor/Contracts/ILatestValueMailbox.h"
#include "ForgeConductor/Domain/Error.h"

#include <mutex>
#include <utility>

namespace ForgeConductor::Tests::Fakes {

template <typename T>
class BoundedLatestValueMailboxFake final
    : public Contracts::ILatestValueMailbox<T> {
public:
    using typename Contracts::ILatestValueMailbox<T>::Consumer;
    using typename Contracts::ILatestValueMailbox<T>::Snapshot;

    void publish(Snapshot value) noexcept override
    {
        std::lock_guard lock{mutex_};
        if (shutdown_) {
            return;
        }
        latest_ = value;
        pending_ = std::move(value);
    }

    [[nodiscard]] Domain::Result<void> setConsumer(
        Consumer consumer) noexcept override
    {
        try {
            std::lock_guard lock{mutex_};
            if (shutdown_) {
                return Domain::Result<void>::failure(Domain::makeError(
                    Domain::ErrorCodes::Cancelled,
                    "Mailbox is shut down."));
            }
            consumer_ = std::move(consumer);
            return Domain::Result<void>::success();
        } catch (...) {
            return Domain::Result<void>::failure(Domain::makeError(
                Domain::ErrorCodes::InternalFailure,
                "Mailbox consumer could not be retained."));
        }
    }

    [[nodiscard]] Snapshot latest() const noexcept override
    {
        std::lock_guard lock{mutex_};
        return latest_;
    }

    [[nodiscard]] std::size_t pendingCount() const noexcept override
    {
        std::lock_guard lock{mutex_};
        return pending_ ? 1U : 0U;
    }

    // Test-owned deterministic dispatcher. It invokes no callback while locked.
    [[nodiscard]] bool drainOne() noexcept
    {
        Snapshot value;
        Consumer consumer;
        {
            std::lock_guard lock{mutex_};
            if (shutdown_ || !pending_ || !consumer_) {
                return false;
            }
            value = std::move(pending_);
            pending_.reset();
            consumer = consumer_;
        }
        try {
            consumer(std::move(value));
            return true;
        } catch (...) {
            return false;
        }
    }

    void shutdown() noexcept override
    {
        std::lock_guard lock{mutex_};
        shutdown_ = true;
        pending_.reset();
        latest_.reset();
        consumer_ = {};
    }

private:
    mutable std::mutex mutex_;
    Snapshot latest_;
    Snapshot pending_;
    Consumer consumer_;
    bool shutdown_{};
};

} // namespace ForgeConductor::Tests::Fakes
