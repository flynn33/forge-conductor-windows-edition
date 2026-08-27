#include "ForgeConductor/Dashboard/DashboardResponseComposer.h"

#include "ForgeConductor/Domain/Utf8.h"

#include <nlohmann/json.hpp>

#include <cstring>
#include <string>
#include <utility>

namespace ForgeConductor::Dashboard {
namespace {

constexpr std::string_view JsonContentType =
    "application/json; charset=utf-8";
constexpr std::string_view FallbackCode = "internal_failure";
constexpr std::string_view FallbackMessage =
    "The dashboard request failed safely.";

[[nodiscard]] Domain::Error compositionError(
    const std::string_view code,
    std::string message)
{
    return Domain::makeError(code, std::move(message));
}

[[nodiscard]] Domain::Error encodingError(
    const DashboardHttpEncodingError error)
{
    if (error == DashboardHttpEncodingError::ResponseTooLarge ||
        error == DashboardHttpEncodingError::HeaderTooLarge) {
        return compositionError(
            Domain::ErrorCodes::PayloadTooLarge,
            "The dashboard response exceeds its encoded byte limit.");
    }
    if (error == DashboardHttpEncodingError::InternalFailure) {
        return compositionError(
            Domain::ErrorCodes::InternalFailure,
            "The dashboard response encoder failed safely.");
    }
    return compositionError(
        Domain::ErrorCodes::IntegrityFailure,
        "The dashboard response metadata violates the encoding contract.");
}

[[nodiscard]] bool validErrorText(
    const std::string_view value,
    const std::size_t maximumBytes) noexcept
{
    return !value.empty() && value.size() <= maximumBytes &&
        value.find('\0') == std::string_view::npos &&
        Domain::isValidUtf8(value);
}

[[nodiscard]] std::vector<std::byte> bytesFromString(std::string text)
{
    std::vector<std::byte> bytes(text.size());
    if (!text.empty()) {
        std::memcpy(bytes.data(), text.data(), text.size());
    }
    return bytes;
}

[[nodiscard]] std::vector<std::byte> errorBody(
    const std::string_view code,
    const std::string_view message)
{
    const nlohmann::json document{
        {"ok", false},
        {"code", code},
        {"message", message}};
    return bytesFromString(document.dump());
}

} // namespace

Domain::Result<DashboardPreparedExchange> DashboardResponseComposer::complete(
    const std::uint16_t status,
    std::string contentType,
    std::vector<std::byte> body,
    std::vector<DashboardHttpHeader> extraHeaders,
    const DashboardPostDeliveryAction postDeliveryAction) noexcept
{
    try {
        if (body.size() > MaximumResponseBodyBytes) {
            return Domain::Result<DashboardPreparedExchange>::failure(
                compositionError(
                    Domain::ErrorCodes::PayloadTooLarge,
                    "The dashboard response body exceeds its reserved byte limit."));
        }

        auto encoded = DashboardHttpResponseEncoder::encode(
            DashboardHttpResponse{
                status,
                std::move(contentType),
                std::move(body),
                std::move(extraHeaders)});
        if (!encoded) {
            return Domain::Result<DashboardPreparedExchange>::failure(
                encodingError(encoded.error()));
        }
        return DashboardPreparedExchange::createComplete(
            std::move(encoded), postDeliveryAction);
    } catch (...) {
        return Domain::Result<DashboardPreparedExchange>::failure(
            compositionError(
                Domain::ErrorCodes::InternalFailure,
                "The dashboard response could not be composed."));
    }
}

Domain::Result<DashboardPreparedExchange>
DashboardResponseComposer::completeText(
    const std::uint16_t status,
    std::string contentType,
    std::string body,
    std::vector<DashboardHttpHeader> extraHeaders,
    const DashboardPostDeliveryAction postDeliveryAction) noexcept
{
    try {
        if (body.size() > MaximumResponseBodyBytes) {
            return Domain::Result<DashboardPreparedExchange>::failure(
                compositionError(
                    Domain::ErrorCodes::PayloadTooLarge,
                    "The dashboard response body exceeds its reserved byte limit."));
        }
        return complete(
            status,
            std::move(contentType),
            bytesFromString(std::move(body)),
            std::move(extraHeaders),
            postDeliveryAction);
    } catch (...) {
        return Domain::Result<DashboardPreparedExchange>::failure(
            compositionError(
                Domain::ErrorCodes::InternalFailure,
                "The dashboard text response could not be composed."));
    }
}

Domain::Result<DashboardPreparedExchange> DashboardResponseComposer::head(
    const std::uint16_t status,
    std::string contentType,
    const std::size_t representationLength,
    std::vector<DashboardHttpHeader> extraHeaders) noexcept
{
    try {
        if (representationLength > MaximumResponseBodyBytes) {
            return Domain::Result<DashboardPreparedExchange>::failure(
                compositionError(
                    Domain::ErrorCodes::PayloadTooLarge,
                    "The dashboard HEAD representation exceeds its reserved byte limit."));
        }
        auto encoded = DashboardHttpResponseEncoder::encodeHead(
            DashboardHttpHeadResponse{
                status,
                std::move(contentType),
                representationLength,
                std::move(extraHeaders)});
        if (!encoded) {
            return Domain::Result<DashboardPreparedExchange>::failure(
                encodingError(encoded.error()));
        }
        return DashboardPreparedExchange::createComplete(
            std::move(encoded), DashboardPostDeliveryAction::None);
    } catch (...) {
        return Domain::Result<DashboardPreparedExchange>::failure(
            compositionError(
                Domain::ErrorCodes::InternalFailure,
                "The dashboard HEAD response could not be composed."));
    }
}

Domain::Result<DashboardPreparedExchange>
DashboardResponseComposer::errorResponse(
    const std::uint16_t status,
    const std::string_view code,
    const std::string_view message,
    std::vector<DashboardHttpHeader> extraHeaders) noexcept
{
    try {
        const bool metadataValid =
            validErrorText(code, MaximumErrorCodeBytes) &&
            validErrorText(message, MaximumErrorMessageBytes);
        if (metadataValid) {
            auto response = complete(
                status,
                std::string{JsonContentType},
                errorBody(code, message),
                std::move(extraHeaders));
            if (response) {
                return response;
            }
        }

        return complete(
            500U,
            std::string{JsonContentType},
            errorBody(FallbackCode, FallbackMessage));
    } catch (...) {
        return Domain::Result<DashboardPreparedExchange>::failure(
            compositionError(
                Domain::ErrorCodes::InternalFailure,
                "The dashboard error response could not be composed."));
    }
}

Domain::Result<DashboardPreparedExchange>
DashboardResponseComposer::headErrorResponse(
    const std::uint16_t status,
    const std::string_view code,
    const std::string_view message,
    std::vector<DashboardHttpHeader> extraHeaders) noexcept
{
    try {
        const bool metadataValid =
            validErrorText(code, MaximumErrorCodeBytes) &&
            validErrorText(message, MaximumErrorMessageBytes);
        if (metadataValid) {
            const auto body = errorBody(code, message);
            auto response = head(
                status,
                std::string{JsonContentType},
                body.size(),
                std::move(extraHeaders));
            if (response) {
                return response;
            }
        }

        const auto fallbackBody = errorBody(FallbackCode, FallbackMessage);
        return head(
            500U,
            std::string{JsonContentType},
            fallbackBody.size());
    } catch (...) {
        return Domain::Result<DashboardPreparedExchange>::failure(
            compositionError(
                Domain::ErrorCodes::InternalFailure,
                "The dashboard HEAD error response could not be composed."));
    }
}

Domain::Result<DashboardPreparedExchange> DashboardResponseComposer::rejection(
    const DashboardHttpRejection& rejection) noexcept
{
    try {
        return errorResponse(
            rejection.status,
            rejection.code,
            rejection.message,
            rejection.responseHeaders);
    } catch (...) {
        return Domain::Result<DashboardPreparedExchange>::failure(
            compositionError(
                Domain::ErrorCodes::InternalFailure,
                "The dashboard rejection could not be composed."));
    }
}

Domain::Result<DashboardPreparedExchange>
DashboardResponseComposer::headRejection(
    const DashboardHttpRejection& rejection) noexcept
{
    try {
        return headErrorResponse(
            rejection.status,
            rejection.code,
            rejection.message,
            rejection.responseHeaders);
    } catch (...) {
        return Domain::Result<DashboardPreparedExchange>::failure(
            compositionError(
                Domain::ErrorCodes::InternalFailure,
                "The dashboard HEAD rejection could not be composed."));
    }
}

Domain::Result<DashboardPreparedExchange>
DashboardResponseComposer::serverSentEvents(
    std::unique_ptr<IDashboardSseSubscription> subscription) noexcept
{
    try {
        auto encoded = DashboardHttpResponseEncoder::encodeSseBootstrap();
        if (!encoded) {
            if (subscription != nullptr) {
                subscription->close();
            }
            return Domain::Result<DashboardPreparedExchange>::failure(
                encodingError(encoded.error()));
        }
        return DashboardPreparedExchange::createSse(
            std::move(encoded), std::move(subscription));
    } catch (...) {
        if (subscription != nullptr) {
            subscription->close();
        }
        return Domain::Result<DashboardPreparedExchange>::failure(
            compositionError(
                Domain::ErrorCodes::InternalFailure,
                "The dashboard SSE response could not be composed."));
    }
}

} // namespace ForgeConductor::Dashboard
