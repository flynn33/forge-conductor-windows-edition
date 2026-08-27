#pragma once

#include "ForgeConductor/Domain/Error.h"
#include "ForgeConductor/Domain/Result.h"

#include <cstddef>

namespace ForgeConductor::Dashboard {

// Product-wide dashboard connection ceilings. Production uses the exact
// defaults. Tests may reduce every ceiling while preserving the same closed
// short-plus-SSE accounting model.
struct DashboardTransportLimits final {
    static constexpr std::size_t DefaultMaximumShortConnections = 8U;
    static constexpr std::size_t DefaultMaximumSseConnections = 32U;
    static constexpr std::size_t DefaultMaximumTotalConnections = 40U;

    std::size_t maximumShortConnections{
        DefaultMaximumShortConnections};
    std::size_t maximumSseConnections{
        DefaultMaximumSseConnections};
    std::size_t maximumTotalConnections{
        DefaultMaximumTotalConnections};

    [[nodiscard]] Domain::Result<void> validate() const noexcept
    {
        try {
            if (maximumShortConnections == 0U ||
                maximumSseConnections == 0U ||
                maximumTotalConnections == 0U) {
                return Domain::Result<void>::failure(Domain::makeError(
                    Domain::ErrorCodes::InvalidRequest,
                    "Dashboard connection limits must all be positive."));
            }
            if (maximumShortConnections >
                    DefaultMaximumShortConnections ||
                maximumSseConnections > DefaultMaximumSseConnections ||
                maximumTotalConnections >
                    DefaultMaximumTotalConnections) {
                return Domain::Result<void>::failure(Domain::makeError(
                    Domain::ErrorCodes::InvalidRequest,
                    "Dashboard connection limits may not exceed the product ceilings."));
            }
            if (maximumShortConnections + maximumSseConnections !=
                maximumTotalConnections) {
                return Domain::Result<void>::failure(Domain::makeError(
                    Domain::ErrorCodes::InvalidRequest,
                    "Dashboard total connections must equal the short and SSE ceilings."));
            }
            return Domain::Result<void>::success();
        } catch (...) {
            return Domain::Result<void>::failure(Domain::makeError(
                Domain::ErrorCodes::InternalFailure,
                "Dashboard connection limits could not be validated."));
        }
    }

    bool operator==(const DashboardTransportLimits&) const = default;
};

} // namespace ForgeConductor::Dashboard
