#pragma once

#include "ForgeConductor/Dashboard/DashboardHttpModels.h"
#include "ForgeConductor/Dashboard/DashboardPreparedExchange.h"
#include "ForgeConductor/Domain/Error.h"
#include "ForgeConductor/Domain/Result.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace ForgeConductor::Dashboard {

// The application-side owner of complete response composition. Reserving the
// entire header ceiling before accepting a body makes every JSON and static
// provider use one stable body budget regardless of actual header length.
class DashboardResponseComposer final {
public:
    static constexpr std::size_t MaximumResponseBodyBytes =
        DashboardHttpResponseEncoder::MaximumEncodedResponseBytes -
        DashboardHttpResponseEncoder::MaximumEncodedHeaderBytes;
    static constexpr std::size_t MaximumErrorCodeBytes = 128U;
    static constexpr std::size_t MaximumErrorMessageBytes = 4U * 1024U;

    [[nodiscard]] static Domain::Result<DashboardPreparedExchange> complete(
        std::uint16_t status,
        std::string contentType,
        std::vector<std::byte> body,
        std::vector<DashboardHttpHeader> extraHeaders = {},
        DashboardPostDeliveryAction postDeliveryAction =
            DashboardPostDeliveryAction::None) noexcept;

    [[nodiscard]] static Domain::Result<DashboardPreparedExchange>
    completeText(
        std::uint16_t status,
        std::string contentType,
        std::string body,
        std::vector<DashboardHttpHeader> extraHeaders = {},
        DashboardPostDeliveryAction postDeliveryAction =
            DashboardPostDeliveryAction::None) noexcept;

    [[nodiscard]] static Domain::Result<DashboardPreparedExchange> head(
        std::uint16_t status,
        std::string contentType,
        std::size_t representationLength,
        std::vector<DashboardHttpHeader> extraHeaders = {}) noexcept;

    // Produces a closed JSON error shape. Invalid or oversized internal error
    // metadata is replaced by a fixed 500 response; it is never reflected as
    // malformed JSON or an unbounded allocation.
    [[nodiscard]] static Domain::Result<DashboardPreparedExchange>
    errorResponse(
        std::uint16_t status,
        std::string_view code,
        std::string_view message,
        std::vector<DashboardHttpHeader> extraHeaders = {}) noexcept;

    [[nodiscard]] static Domain::Result<DashboardPreparedExchange>
    headErrorResponse(
        std::uint16_t status,
        std::string_view code,
        std::string_view message,
        std::vector<DashboardHttpHeader> extraHeaders = {}) noexcept;

    [[nodiscard]] static Domain::Result<DashboardPreparedExchange> rejection(
        const DashboardHttpRejection& rejection) noexcept;

    [[nodiscard]] static Domain::Result<DashboardPreparedExchange>
    headRejection(const DashboardHttpRejection& rejection) noexcept;

    [[nodiscard]] static Domain::Result<DashboardPreparedExchange>
    serverSentEvents(
        std::unique_ptr<IDashboardSseSubscription> subscription) noexcept;
};

} // namespace ForgeConductor::Dashboard
