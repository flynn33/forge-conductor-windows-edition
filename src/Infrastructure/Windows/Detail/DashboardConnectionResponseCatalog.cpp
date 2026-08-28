#include "DashboardConnectionResponseCatalog.h"

#include "ForgeConductor/Dashboard/DashboardResponseComposer.h"
#include "ForgeConductor/Domain/Error.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>

namespace ForgeConductor::Infrastructure::Windows::Detail {
namespace {

using ImmutableBytes = DashboardConnectionResponseCatalog::ImmutableBytes;

[[nodiscard]] Domain::Error catalogError(
    const std::string_view code,
    std::string message)
{
    return Domain::makeError(code, std::move(message));
}

[[nodiscard]] Domain::Result<ImmutableBytes> fixedErrorResponse(
    const std::uint16_t status,
    const std::string_view code,
    const std::string_view message) noexcept
{
    try {
        auto prepared = Dashboard::DashboardResponseComposer::errorResponse(
            status, code, message);
        if (!prepared) {
            return Domain::Result<ImmutableBytes>::failure(
                std::move(prepared).error());
        }

        auto& exchange = prepared.value();
        auto* complete = exchange.completeExchange();
        if (exchange.kind() !=
                Dashboard::DashboardPreparedExchange::Kind::Complete ||
            complete == nullptr ||
            complete->encodedResponse().kind() !=
                Dashboard::DashboardHttpEncodingResult::Kind::CompleteResponse ||
            complete->encodedResponse().bytes().empty() ||
            complete->takePostDeliveryAction() !=
                Dashboard::DashboardPostDeliveryAction::None) {
            return Domain::Result<ImmutableBytes>::failure(catalogError(
                Domain::ErrorCodes::IntegrityFailure,
                "A fixed dashboard transport response violated its complete "
                "exchange contract."));
        }

        return Domain::Result<ImmutableBytes>::success(
            std::make_shared<const std::vector<std::byte>>(
                complete->encodedResponse().bytes()));
    } catch (...) {
        return Domain::Result<ImmutableBytes>::failure(catalogError(
            Domain::ErrorCodes::InternalFailure,
            "A fixed dashboard transport response could not be composed."));
    }
}

} // namespace

Domain::Result<std::unique_ptr<DashboardConnectionResponseCatalog>>
DashboardConnectionResponseCatalog::create() noexcept
{
    using CreationResult = Domain::Result<
        std::unique_ptr<DashboardConnectionResponseCatalog>>;
    try {
        auto generic = fixedErrorResponse(
            503U,
            "service_unavailable",
            "The dashboard service is temporarily unavailable.");
        if (!generic) {
            return CreationResult::failure(std::move(generic).error());
        }

        auto stream = fixedErrorResponse(
            503U,
            "stream_unavailable",
            "The dashboard telemetry stream is temporarily unavailable.");
        if (!stream) {
            return CreationResult::failure(std::move(stream).error());
        }

        auto internal = fixedErrorResponse(
            500U,
            "internal_failure",
            "The dashboard operation failed safely.");
        if (!internal) {
            return CreationResult::failure(std::move(internal).error());
        }

        return CreationResult::success(
            std::unique_ptr<DashboardConnectionResponseCatalog>{
                new DashboardConnectionResponseCatalog{
                    std::move(generic).value(),
                    std::move(stream).value(),
                    std::move(internal).value()}});
    } catch (...) {
        return CreationResult::failure(catalogError(
            Domain::ErrorCodes::InternalFailure,
            "The dashboard connection response catalog could not be "
            "created."));
    }
}

DashboardConnectionResponseCatalog::DashboardConnectionResponseCatalog(
    ImmutableBytes genericServiceUnavailable,
    ImmutableBytes streamUnavailable,
    ImmutableBytes internalFailure) noexcept
    : genericServiceUnavailable_{std::move(genericServiceUnavailable)},
      streamUnavailable_{std::move(streamUnavailable)},
      internalFailure_{std::move(internalFailure)}
{
}

const DashboardConnectionResponseCatalog::ImmutableBytes&
DashboardConnectionResponseCatalog::genericServiceUnavailable()
    const noexcept
{
    return genericServiceUnavailable_;
}

const DashboardConnectionResponseCatalog::ImmutableBytes&
DashboardConnectionResponseCatalog::streamUnavailable() const noexcept
{
    return streamUnavailable_;
}

const DashboardConnectionResponseCatalog::ImmutableBytes&
DashboardConnectionResponseCatalog::internalFailure() const noexcept
{
    return internalFailure_;
}

} // namespace ForgeConductor::Infrastructure::Windows::Detail
