#pragma once

#include <functional>
#include <memory>

namespace forge
{
    template <typename T>
    class ILatestValueMailbox
    {
    public:
        virtual ~ILatestValueMailbox() = default;

        // Replaces any pending value. At most one delivery callback may be queued.
        virtual void publish(std::shared_ptr<T const> value) = 0;

        // The consumer receives the newest available value. Intermediate values may be dropped.
        virtual void setConsumer(std::function<void(std::shared_ptr<T const>)> consumer) = 0;

        virtual std::size_t pendingCount() const noexcept = 0;
        virtual void shutdown() noexcept = 0;
    };
}
