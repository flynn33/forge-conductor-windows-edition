#pragma once

#include "PlatformBoundaryFakeTestSupport.h"
#include "Fakes/DeterministicWorkspaceAuthority.h"

#include <stop_token>
#include <type_traits>
#include <vector>

namespace ForgeConductor::Tests {

inline void runAuthorityContextFakeContractTests()
{
    namespace Support = PlatformBoundaryTestSupport;
    namespace Fakes = ForgeConductor::Tests::Fakes;

    static_assert(std::is_final_v<Fakes::DeterministicWorkspaceAuthority>);

    const Domain::MonotonicTimePoint now{};
    const auto root = Support::path("C:/platform-boundary");
    Fakes::DeterministicWorkspaceAuthority issuer{
        Support::authorityId(),
        Support::clientId(),
        {root},
        Domain::FileAccess::Execute,
        {
            Domain::FileAccess::Read,
            Domain::FileAccess::Write,
            Domain::FileAccess::Execute},
        {Domain::FileAccess::Delete},
        true,
        1};
    issuer.setNow(now);

    const auto active = Support::activeContext(now);
    const auto authority = Support::take(
        issuer.authorityFor(Support::projectId(), active));
    const auto request = Domain::PathAuthorizationRequest{
        Support::path("C:/platform-boundary/file.txt"),
        std::optional<Domain::PathText>{root},
        Domain::FileAccess::Read,
        false};
    const auto authorized = Support::take(
        issuer.authorize(authority, request, active));
    Support::require(
        authorized.authorityId() == authority.authorityId() &&
            authorized.authorityRoot() == root &&
            authorized.access() == Domain::FileAccess::Read,
        "authority path binding changed");

    std::stop_source cancellation;
    cancellation.request_stop();
    const auto cancelled =
        Support::cancelledContext(now, cancellation.get_token());
    const auto expired = Support::expiredContext(now);

    Support::requireError(
        issuer.authorityFor(Support::projectId(), cancelled),
        Domain::ErrorCodes::Cancelled,
        "authorityFor accepted cancellation");
    Support::requireError(
        issuer.authorityFor(Support::projectId(), expired),
        Domain::ErrorCodes::DeadlineExceeded,
        "authorityFor accepted expiration");

    Support::requireError(
        issuer.narrow(
            authority,
            {root},
            {Domain::FileAccess::Execute},
            true,
            2,
            cancelled),
        Domain::ErrorCodes::Cancelled,
        "authority narrowing accepted cancellation");
    Support::requireError(
        issuer.narrow(
            authority,
            {root},
            {Domain::FileAccess::Execute},
            true,
            2,
            expired),
        Domain::ErrorCodes::DeadlineExceeded,
        "authority narrowing accepted expiration");

    Support::requireError(
        issuer.authorize(authority, request, cancelled),
        Domain::ErrorCodes::Cancelled,
        "path authorization accepted cancellation");
    Support::requireError(
        issuer.authorize(authority, request, expired),
        Domain::ErrorCodes::DeadlineExceeded,
        "path authorization accepted expiration");
    Support::require(
        issuer.lastContext() &&
            issuer.lastContext()->correlationId == expired.correlationId,
        "authority fake did not retain the latest context");
}

} // namespace ForgeConductor::Tests
