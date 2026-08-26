#pragma once

#include "ForgeConductor/Domain/OperationContext.h"
#include "ForgeConductor/Domain/Result.h"

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <mutex>
#include <string_view>

namespace ForgeConductor::Infrastructure::Windows::Detail {

// Serializes synchronous boundary work without owning a background thread.
// At most MaximumPendingOperationCount callers may be active or waiting.
class BoundedSerialExecutor final {
public:
    static constexpr std::size_t MaximumPendingOperationCount = 16U;

    class Lease final {
    public:
        Lease(const Lease&) = delete;
        Lease& operator=(const Lease&) = delete;
        Lease(Lease&& other) noexcept;
        Lease& operator=(Lease&& other) = delete;
        ~Lease();

    private:
        friend class BoundedSerialExecutor;
        explicit Lease(BoundedSerialExecutor& owner) noexcept;
        void release() noexcept;

        BoundedSerialExecutor* owner_{};
    };

    BoundedSerialExecutor() = default;
    ~BoundedSerialExecutor();

    BoundedSerialExecutor(const BoundedSerialExecutor&) = delete;
    BoundedSerialExecutor& operator=(const BoundedSerialExecutor&) = delete;

    [[nodiscard]] Domain::Result<Lease> acquire(const Domain::OperationContext& context,
                                                std::string_view action) noexcept;

    void beginShutdown() noexcept;
    [[nodiscard]] bool waitUntilIdle(std::chrono::milliseconds timeout) noexcept;
    void shutdown() noexcept;

private:
    void release() noexcept;

    std::mutex mutex_;
    std::condition_variable condition_;
    bool active_{};
    bool shutdownRequested_{};
    std::size_t waiting_{};
};

} // namespace ForgeConductor::Infrastructure::Windows::Detail
