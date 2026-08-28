#include "ForgeConductor/Dashboard/DashboardApplicationErrorMapper.h"

#include <string>
#include <string_view>

namespace ForgeConductor::Dashboard {
namespace {

[[nodiscard]] bool isCode(
    const Domain::Error& error,
    const std::string_view expected) noexcept
{
    return error.code == expected;
}

[[nodiscard]] DashboardHttpRejection rejection(
    const std::uint16_t status,
    const std::string_view code,
    const std::string_view message)
{
    return DashboardHttpRejection{
        status,
        std::string{code},
        std::string{message},
        {}};
}

[[nodiscard]] DashboardHttpRejection internalFailure()
{
    return rejection(
        500U,
        "internal_failure",
        "The dashboard operation failed safely.");
}

} // namespace

DashboardHttpRejection DashboardApplicationErrorMapper::map(
    const Domain::Error& error,
    const DashboardApplicationErrorOrigin origin) noexcept
{
    try {
        if (origin == DashboardApplicationErrorOrigin::ResponseEncoding) {
            return rejection(
                500U,
                "response_too_large",
                "The dashboard response could not fit its bounded wire "
                "representation.");
        }

        if (origin == DashboardApplicationErrorOrigin::RequestBody) {
            if (isCode(error, Domain::ErrorCodes::PayloadTooLarge) ||
                isCode(error, Domain::ErrorCodes::LimitExceeded)) {
                return rejection(
                    413U,
                    "payload_too_large",
                    "The dashboard request body exceeds its bounded limit.");
            }
            if (isCode(error, Domain::ErrorCodes::InvalidRequest) ||
                isCode(error, Domain::ErrorCodes::MalformedMessage)) {
                return rejection(
                    400U,
                    "invalid_request",
                    "The dashboard request body is invalid.");
            }
        }

        if (origin == DashboardApplicationErrorOrigin::SseSubscription &&
            (isCode(error, Domain::ErrorCodes::LimitExceeded) ||
             isCode(error, Domain::ErrorCodes::RateLimited) ||
             isCode(error, Domain::ErrorCodes::TransportClosed))) {
            return rejection(
                503U,
                "stream_unavailable",
                "The dashboard telemetry stream is temporarily unavailable.");
        }

        if (isCode(error, Domain::ErrorCodes::InvalidRequest) ||
            isCode(error, Domain::ErrorCodes::MalformedMessage)) {
            return rejection(
                400U,
                "invalid_request",
                "The dashboard request is invalid.");
        }
        if (isCode(error, Domain::ErrorCodes::Unauthorized) ||
            isCode(error, Domain::ErrorCodes::ProjectScopeMismatch) ||
            isCode(error, Domain::ErrorCodes::PathOutsideAuthority) ||
            isCode(error, Domain::ErrorCodes::RedactionRejected) ||
            isCode(error, Domain::ErrorCodes::ShellDisabled)) {
            return rejection(
                403U,
                "forbidden",
                "The dashboard operation is not permitted.");
        }
        if (isCode(error, Domain::ErrorCodes::ProjectNotFound) ||
            isCode(error, Domain::ErrorCodes::RecordNotFound) ||
            isCode(error, Domain::ErrorCodes::AgentNotFound) ||
            isCode(error, Domain::ErrorCodes::SessionNotFound)) {
            return rejection(
                404U,
                "not_found",
                "The requested dashboard record was not found.");
        }
        if (isCode(error, Domain::ErrorCodes::OwnershipConflict) ||
            isCode(error, Domain::ErrorCodes::Conflict)) {
            return rejection(
                409U,
                "conflict",
                "The dashboard operation conflicts with current state.");
        }
        if (isCode(error, Domain::ErrorCodes::PayloadTooLarge)) {
            return rejection(
                413U,
                "payload_too_large",
                "The dashboard payload exceeds its bounded limit.");
        }
        if (isCode(error, Domain::ErrorCodes::UnsupportedVersion)) {
            return rejection(
                422U,
                "unsupported_version",
                "The dashboard request version is unsupported.");
        }
        if (isCode(error, Domain::ErrorCodes::LimitExceeded) ||
            isCode(error, Domain::ErrorCodes::RateLimited)) {
            return rejection(
                429U,
                "rate_limited",
                "The dashboard request exceeds a bounded service limit.");
        }
        if (isCode(error, Domain::ErrorCodes::DatabaseBusy) ||
            isCode(error, Domain::ErrorCodes::DeadlineExceeded) ||
            isCode(error, Domain::ErrorCodes::Cancelled) ||
            isCode(error, Domain::ErrorCodes::TransportClosed) ||
            isCode(error, Domain::ErrorCodes::HostCapabilityUnavailable) ||
            isCode(error, Domain::ErrorCodes::AcknowledgementTimeout)) {
            return rejection(
                503U,
                "service_unavailable",
                "The dashboard service is temporarily unavailable.");
        }
        return internalFailure();
    } catch (...) {
        return internalFailure();
    }
}

} // namespace ForgeConductor::Dashboard
