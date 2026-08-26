#pragma once

#include "ForgeConductor/Domain/Identifiers.h"

#include <chrono>
#include <stop_token>

namespace ForgeConductor::Domain {

using UtcTimePoint = std::chrono::system_clock::time_point;
using MonotonicTimePoint = std::chrono::steady_clock::time_point;

struct OperationContext final {
    OperationId operationId;
    MonotonicTimePoint deadline;
    std::stop_token cancellation;
    CorrelationId correlationId;

    [[nodiscard]] bool isCancellationRequested() const noexcept
    {
        return cancellation.stop_requested();
    }

    [[nodiscard]] bool isExpired(const MonotonicTimePoint now) const noexcept
    {
        return now >= deadline;
    }
};

} // namespace ForgeConductor::Domain
