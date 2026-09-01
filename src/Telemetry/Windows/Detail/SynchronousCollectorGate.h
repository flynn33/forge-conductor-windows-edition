#pragma once

#include "ForgeConductor/Contracts/IFoundationServices.h"
#include "ForgeConductor/Domain/OperationContext.h"
#include "ForgeConductor/Domain/Result.h"

#include <condition_variable>
#include <functional>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>

namespace ForgeConductor::Telemetry::Windows::Detail {

// Owns only synchronous collector admission and shutdown state. The collector
// owns this gate and must outlive every outstanding Lease.
class SynchronousCollectorGate final {
public:
    class Lease final {
    public:
        ~Lease() noexcept
        {
            release();
        }

        Lease(const Lease&) = delete;
        Lease& operator=(const Lease&) = delete;

        Lease(Lease&& other) noexcept
            : owner_{std::exchange(other.owner_, nullptr)}
        {
        }

        Lease& operator=(Lease&& other) noexcept
        {
            if (this != &other) {
                release();
                owner_ = std::exchange(other.owner_, nullptr);
            }
            return *this;
        }

    private:
        friend class SynchronousCollectorGate;

        explicit Lease(SynchronousCollectorGate& owner) noexcept
            : owner_{&owner}
        {
        }

        void release() noexcept
        {
            if (owner_ != nullptr) {
                auto* const owner = std::exchange(owner_, nullptr);
                owner->releaseAdmission();
            }
        }

        SynchronousCollectorGate* owner_{};
    };

    SynchronousCollectorGate(
        const Contracts::IClock& clock,
        std::string collectorName)
        : clock_{clock}, collectorName_{std::move(collectorName)}
    {
    }

    SynchronousCollectorGate(const SynchronousCollectorGate&) = delete;
    SynchronousCollectorGate& operator=(const SynchronousCollectorGate&) = delete;
    SynchronousCollectorGate(SynchronousCollectorGate&&) = delete;
    SynchronousCollectorGate& operator=(SynchronousCollectorGate&&) = delete;

    [[nodiscard]] Domain::Result<Lease> tryAcquire(
        const Domain::OperationContext& context)
    {
        const auto current = validateContext(context, "collector admission");
        if (!current) {
            return Domain::Result<Lease>::failure(current.error());
        }

        const std::lock_guard lock{lifecycleMutex_};
        if (closed_) {
            return Domain::Result<Lease>::failure(closedError("is shut down"));
        }
        if (active_) {
            return Domain::Result<Lease>::failure(Domain::makeError(
                Domain::ErrorCodes::LimitExceeded,
                "The synchronous " + collectorName_ +
                    " already has an active operation."));
        }

        active_ = true;
        return Domain::Result<Lease>::success(Lease{*this});
    }

    [[nodiscard]] Domain::Result<void> checkpoint(
        const Domain::OperationContext& context,
        const std::string_view action)
    {
        const std::lock_guard lock{lifecycleMutex_};
        const auto current = validateContext(context, action);
        if (!current) {
            return current;
        }
        if (closed_) {
            return Domain::Result<void>::failure(
                closedError("was shut down before " + std::string{action}));
        }
        return Domain::Result<void>::success();
    }

    template <typename Publisher>
    [[nodiscard]] Domain::Result<void> publish(
        const Domain::OperationContext& context,
        const std::string_view action,
        Publisher&& publisher)
    {
        const std::lock_guard lock{lifecycleMutex_};
        const auto current = validateContext(context, action);
        if (!current) {
            return current;
        }
        if (closed_) {
            return Domain::Result<void>::failure(
                closedError("was shut down before " + std::string{action}));
        }

        std::invoke(std::forward<Publisher>(publisher));
        return Domain::Result<void>::success();
    }

    void shutdownAndDrain()
    {
        std::unique_lock lock{lifecycleMutex_};
        closed_ = true;
        lifecycleChanged_.wait(lock, [this]() noexcept { return !active_; });
    }

private:
    [[nodiscard]] Domain::Result<void> validateContext(
        const Domain::OperationContext& context,
        const std::string_view action) const
    {
        if (context.isCancellationRequested()) {
            return Domain::Result<void>::failure(Domain::makeError(
                Domain::ErrorCodes::Cancelled,
                "The " + collectorName_ + " was cancelled before " +
                    std::string{action} + "."));
        }
        if (context.isExpired(clock_.monotonicNow())) {
            return Domain::Result<void>::failure(Domain::makeError(
                Domain::ErrorCodes::DeadlineExceeded,
                "The " + collectorName_ + " deadline expired before " +
                    std::string{action} + "."));
        }
        return Domain::Result<void>::success();
    }

    [[nodiscard]] Domain::Error closedError(const std::string& state) const
    {
        return Domain::makeError(
            Domain::ErrorCodes::TransportClosed,
            "The " + collectorName_ + " " + state + ".");
    }

    void releaseAdmission() noexcept
    {
        {
            const std::lock_guard lock{lifecycleMutex_};
            active_ = false;
        }
        lifecycleChanged_.notify_all();
    }

    const Contracts::IClock& clock_;
    const std::string collectorName_;
    std::mutex lifecycleMutex_;
    std::condition_variable lifecycleChanged_;
    bool closed_{};
    bool active_{};
};

} // namespace ForgeConductor::Telemetry::Windows::Detail
