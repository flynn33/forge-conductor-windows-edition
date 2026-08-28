#include "ForgeConductor/Dashboard/DashboardApplicationErrorMapper.h"

#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace Dashboard = ForgeConductor::Dashboard;
namespace Domain = ForgeConductor::Domain;

std::size_t assertions{};

#define REQUIRE(condition)                                                       \
    do {                                                                         \
        ++assertions;                                                            \
        if (!(condition)) {                                                      \
            throw std::runtime_error{std::string{"Requirement failed: "} +      \
                                     #condition};                                \
        }                                                                        \
    } while (false)

void requireMapping(
    const std::string_view sourceCode,
    const Dashboard::DashboardApplicationErrorOrigin origin,
    const std::uint16_t status,
    const std::string_view publicCode)
{
    const auto mapped = Dashboard::DashboardApplicationErrorMapper::map(
        Domain::makeError(
            sourceCode,
            "hostile dependency detail with token=secret",
            true,
            "private-evidence"),
        origin);
    REQUIRE(mapped.status == status);
    REQUIRE(mapped.code == publicCode);
    REQUIRE(!mapped.message.empty());
    REQUIRE(mapped.message.find("hostile") == std::string::npos);
    REQUIRE(mapped.message.find("secret") == std::string::npos);
    REQUIRE(mapped.message.find("private-evidence") == std::string::npos);
    REQUIRE(mapped.responseHeaders.empty());
}

void mapsRequestBodyFailures()
{
    using Origin = Dashboard::DashboardApplicationErrorOrigin;
    requireMapping(
        Domain::ErrorCodes::InvalidRequest,
        Origin::RequestBody,
        400U,
        "invalid_request");
    requireMapping(
        Domain::ErrorCodes::MalformedMessage,
        Origin::RequestBody,
        400U,
        "invalid_request");
    requireMapping(
        Domain::ErrorCodes::PayloadTooLarge,
        Origin::RequestBody,
        413U,
        "payload_too_large");
    requireMapping(
        Domain::ErrorCodes::LimitExceeded,
        Origin::RequestBody,
        413U,
        "payload_too_large");
}

void mapsDependencyFailuresWithoutReflectingDetails()
{
    using Origin = Dashboard::DashboardApplicationErrorOrigin;
    const std::vector<std::string_view> forbidden{
        Domain::ErrorCodes::Unauthorized,
        Domain::ErrorCodes::ProjectScopeMismatch,
        Domain::ErrorCodes::PathOutsideAuthority,
        Domain::ErrorCodes::RedactionRejected,
        Domain::ErrorCodes::ShellDisabled,
    };
    for (const auto code : forbidden) {
        requireMapping(code, Origin::Dependency, 403U, "forbidden");
    }

    const std::vector<std::string_view> missing{
        Domain::ErrorCodes::ProjectNotFound,
        Domain::ErrorCodes::RecordNotFound,
        Domain::ErrorCodes::AgentNotFound,
        Domain::ErrorCodes::SessionNotFound,
    };
    for (const auto code : missing) {
        requireMapping(code, Origin::Dependency, 404U, "not_found");
    }

    requireMapping(
        Domain::ErrorCodes::OwnershipConflict,
        Origin::Dependency,
        409U,
        "conflict");
    requireMapping(
        Domain::ErrorCodes::Conflict,
        Origin::Dependency,
        409U,
        "conflict");
    requireMapping(
        Domain::ErrorCodes::PayloadTooLarge,
        Origin::Dependency,
        413U,
        "payload_too_large");
    requireMapping(
        Domain::ErrorCodes::UnsupportedVersion,
        Origin::Dependency,
        422U,
        "unsupported_version");
    requireMapping(
        Domain::ErrorCodes::LimitExceeded,
        Origin::Dependency,
        429U,
        "rate_limited");
    requireMapping(
        Domain::ErrorCodes::RateLimited,
        Origin::Dependency,
        429U,
        "rate_limited");

    const std::vector<std::string_view> unavailable{
        Domain::ErrorCodes::DatabaseBusy,
        Domain::ErrorCodes::DeadlineExceeded,
        Domain::ErrorCodes::Cancelled,
        Domain::ErrorCodes::TransportClosed,
        Domain::ErrorCodes::HostCapabilityUnavailable,
        Domain::ErrorCodes::AcknowledgementTimeout,
    };
    for (const auto code : unavailable) {
        requireMapping(
            code,
            Origin::Dependency,
            503U,
            "service_unavailable");
    }
}

void appliesRouteSpecificAndSafeFallbackMappings()
{
    using Origin = Dashboard::DashboardApplicationErrorOrigin;
    requireMapping(
        Domain::ErrorCodes::LimitExceeded,
        Origin::SseSubscription,
        503U,
        "stream_unavailable");
    requireMapping(
        Domain::ErrorCodes::RateLimited,
        Origin::SseSubscription,
        503U,
        "stream_unavailable");
    requireMapping(
        Domain::ErrorCodes::TransportClosed,
        Origin::SseSubscription,
        503U,
        "stream_unavailable");
    requireMapping(
        Domain::ErrorCodes::StorageFull,
        Origin::ResponseEncoding,
        500U,
        "response_too_large");
    requireMapping(
        Domain::ErrorCodes::IntegrityFailure,
        Origin::Dependency,
        500U,
        "internal_failure");
    requireMapping(
        "unknown_and_untrusted",
        Origin::Dependency,
        500U,
        "internal_failure");
}

} // namespace

int main()
{
    try {
        mapsRequestBodyFailures();
        mapsDependencyFailuresWithoutReflectingDetails();
        appliesRouteSpecificAndSafeFallbackMappings();
        std::cout << "Dashboard application error mapper tests passed ("
                  << assertions << " assertions).\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Dashboard application error mapper tests failed: "
                  << error.what() << '\n';
        return 1;
    } catch (...) {
        std::cerr << "Dashboard application error mapper tests failed with an "
                     "unknown error.\n";
        return 1;
    }
}
