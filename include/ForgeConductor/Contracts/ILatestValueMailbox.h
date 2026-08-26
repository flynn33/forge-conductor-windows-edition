#pragma once

#include "ForgeConductor/Domain/Result.h"

#include <cstddef>
#include <functional>
#include <memory>

namespace ForgeConductor::Contracts {

template <typename T>
class ILatestValueMailbox {
public:
    using Snapshot = std::shared_ptr<const T>;
    using Consumer = std::function<void(Snapshot)>;

    virtual ~ILatestValueMailbox() = default;

    // Replaces the pending value. Implementations may drop intermediate values,
    // and may never queue more than one pending delivery callback.
    virtual void publish(Snapshot value) noexcept = 0;

    [[nodiscard]] virtual Domain::Result<void> setConsumer(
        Consumer consumer) noexcept = 0;

    [[nodiscard]] virtual Snapshot latest() const noexcept = 0;
    [[nodiscard]] virtual std::size_t pendingCount() const noexcept = 0;

    virtual void shutdown() noexcept = 0;
};

} // namespace ForgeConductor::Contracts
