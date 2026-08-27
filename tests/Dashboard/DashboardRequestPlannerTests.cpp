#include "ForgeConductor/Dashboard/DashboardRequestPlanner.h"

#include "ForgeConductor/Domain/Error.h"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
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

static_assert(std::is_final_v<Dashboard::DashboardRequestPlan>);
static_assert(std::is_final_v<Dashboard::DashboardRequestPlanner>);
static_assert(std::is_copy_constructible_v<Dashboard::DashboardRequestPlan>);
static_assert(std::is_nothrow_move_constructible_v<
              Dashboard::DashboardRequestPlan>);
static_assert(!std::is_copy_assignable_v<Dashboard::DashboardRequestPlan>);
static_assert(!std::is_move_assignable_v<Dashboard::DashboardRequestPlan>);
static_assert(noexcept(Dashboard::DashboardRequestPlanner::plan(
    std::declval<const Dashboard::DashboardRequestPolicy&>(),
    std::declval<const Dashboard::DashboardHttpRequest&>(),
    true,
    std::declval<const Domain::ResourceBudgets&>())));
static_assert(noexcept(
    std::declval<const Dashboard::DashboardRequestPlan&>().kind()));
static_assert(noexcept(
    std::declval<const Dashboard::DashboardRequestPlan&>().rejection()));
static_assert(noexcept(
    std::declval<const Dashboard::DashboardRequestPlan&>().route()));
static_assert(noexcept(
    std::declval<const Dashboard::DashboardRequestPlan&>().staticResource()));
static_assert(noexcept(
    std::declval<const Dashboard::DashboardRequestPlan&>().streamRate()));

template <typename T>
[[nodiscard]] T take(Domain::Result<T> result)
{
    if (!result) {
        throw std::runtime_error{
            result.error().code + ": " + result.error().message};
    }
    return std::move(result).value();
}

[[nodiscard]] Domain::Sha256Digest token()
{
    return take(Domain::Sha256Digest::parse(std::string(64U, 'a')));
}

[[nodiscard]] Dashboard::DashboardRequestPolicy policy()
{
    return take(Dashboard::DashboardRequestPolicy::create(
        "127.0.0.1", 47820U, token()));
}

[[nodiscard]] std::string bearer()
{
    return "Bearer " + std::string(64U, 'a');
}

[[nodiscard]] std::vector<Dashboard::DashboardHttpHeader> headers(
    const bool authorized = false,
    const bool json = false,
    std::string host = "127.0.0.1:47820")
{
    std::vector<Dashboard::DashboardHttpHeader> result{
        {"host", std::move(host)}};
    if (authorized) {
        result.push_back({"authorization", bearer()});
    }
    if (json) {
        result.push_back({"content-type", "application/json"});
    }
    return result;
}

[[nodiscard]] Dashboard::DashboardHttpRequest request(
    std::string method,
    std::string target,
    std::vector<Dashboard::DashboardHttpHeader> requestHeaders)
{
    return Dashboard::DashboardHttpRequest{
        std::move(method),
        std::move(target),
        std::move(requestHeaders),
        {}};
}

[[nodiscard]] Domain::ResourceBudgets budgets(const double sampleHz)
{
    Domain::ResourceBudgets result{};
    result.telemetrySampleHz = sampleHz;
    return result;
}

[[nodiscard]] Dashboard::DashboardRequestPlan planned(
    const Dashboard::DashboardRequestPolicy& requestPolicy,
    Dashboard::DashboardHttpRequest input,
    const bool serviceActive = true,
    const double sampleHz = 2.0)
{
    return Dashboard::DashboardRequestPlanner::plan(
        requestPolicy, input, serviceActive, budgets(sampleHz));
}

void requireRejected(
    const Dashboard::DashboardRequestPlan& result,
    const std::uint16_t status,
    const std::string_view code)
{
    REQUIRE(result.kind() == Dashboard::DashboardRequestPlanKind::Rejected);
    REQUIRE(result.rejection() != nullptr);
    REQUIRE(result.rejection()->status == status);
    REQUIRE(result.rejection()->code == code);
    REQUIRE(result.route() == nullptr);
    REQUIRE(result.staticResource() == nullptr);
    REQUIRE(result.streamRate() == nullptr);
}

void producesOwnedPlansWithExclusivePayloads()
{
    const auto requestPolicy = policy();

    const auto ordinary = planned(
        requestPolicy,
        request("GET", "/api/status", headers(true)));
    REQUIRE(ordinary.kind() == Dashboard::DashboardRequestPlanKind::Dispatch);
    REQUIRE(ordinary.rejection() == nullptr);
    REQUIRE(ordinary.route() != nullptr);
    REQUIRE(ordinary.route()->id() == Dashboard::DashboardRouteId::Status);
    REQUIRE(ordinary.staticResource() == nullptr);
    REQUIRE(ordinary.streamRate() == nullptr);

    const auto resource = planned(
        requestPolicy,
        request("GET", "/static/app.js?v=alpha", headers()));
    REQUIRE(
        resource.kind() == Dashboard::DashboardRequestPlanKind::StaticResource);
    REQUIRE(resource.rejection() == nullptr);
    REQUIRE(resource.route() != nullptr);
    REQUIRE(resource.route()->id() ==
            Dashboard::DashboardRouteId::StaticResource);
    REQUIRE(resource.staticResource() != nullptr);
    REQUIRE(resource.staticResource()->relativePath() == "app.js");
    REQUIRE(resource.staticResource()->mimeType() ==
            "application/javascript; charset=utf-8");
    REQUIRE(resource.streamRate() == nullptr);

    const auto stream = planned(
        requestPolicy,
        request("GET", "/api/stream", headers(true)),
        true,
        1.0);
    REQUIRE(
        stream.kind() == Dashboard::DashboardRequestPlanKind::ServerSentEvents);
    REQUIRE(stream.rejection() == nullptr);
    REQUIRE(stream.route() != nullptr);
    REQUIRE(stream.route()->id() ==
            Dashboard::DashboardRouteId::TelemetryStream);
    REQUIRE(stream.staticResource() == nullptr);
    REQUIRE(stream.streamRate() != nullptr);
    REQUIRE(stream.streamRate()->normalizedRequestedHz == 20.0);
    REQUIRE(stream.streamRate()->deliveryHz == 1.0);
    REQUIRE(stream.streamRate()->source ==
            Dashboard::DashboardStreamRateSource::ProfileDefault);
    REQUIRE(!stream.streamRate()->compatibilityClamped);
    REQUIRE(stream.streamRate()->resourceCapped);
}

void policyAlwaysPrecedesRoutingAndRouteSpecificDecoding()
{
    const auto requestPolicy = policy();

    const auto badHost = planned(
        requestPolicy,
        request("GET", "/unknown", headers(false, false, "localhost:47820")));
    requireRejected(badHost, 403U, "forbidden");

    const auto unknownWithoutAuthentication = planned(
        requestPolicy,
        request("GET", "/unknown", headers()));
    requireRejected(unknownWithoutAuthentication, 401U, "unauthorized");
    REQUIRE(unknownWithoutAuthentication.rejection()->responseHeaders.size() ==
            1U);
    REQUIRE((
        unknownWithoutAuthentication.rejection()->responseHeaders.front() ==
        Dashboard::DashboardHttpHeader{"www-authenticate", "Bearer"}));

    const auto unavailableWithoutAuthentication = planned(
        requestPolicy,
        request("POST", "/api/tools/call", headers()));
    requireRejected(unavailableWithoutAuthentication, 401U, "unauthorized");

    const auto unavailableWithWrongMediaType = planned(
        requestPolicy,
        request("POST", "/api/tools/call", headers(true)));
    requireRejected(
        unavailableWithWrongMediaType, 415U, "unsupported_media_type");

    const auto unavailable = planned(
        requestPolicy,
        request("POST", "/api/tools/call", headers(true, true)));
    requireRejected(unavailable, 404U, "not_found");

    const auto traversalWithBadHost = planned(
        requestPolicy,
        request(
            "GET",
            "/static/%2e%2e/app.js",
            headers(false, false, "localhost:47820")));
    requireRejected(traversalWithBadHost, 403U, "forbidden");

    const auto traversal = planned(
        requestPolicy,
        request("GET", "/static/%2e%2e/app.js", headers()));
    requireRejected(traversal, 404U, "not_found");

    const auto invalidStreamWithoutAuthentication = planned(
        requestPolicy,
        request("GET", "/api/stream?hz=", headers()));
    requireRejected(
        invalidStreamWithoutAuthentication, 401U, "unauthorized");

    const auto invalidStream = planned(
        requestPolicy,
        request("GET", "/api/stream?hz=", headers(true)));
    requireRejected(invalidStream, 400U, "invalid_query");
}

void mapsEveryRouteDispositionToAStableHttpRejection()
{
    const auto requestPolicy = policy();

    const auto globalOptions = planned(
        requestPolicy,
        request("OPTIONS", "/", headers()));
    requireRejected(globalOptions, 405U, "method_not_allowed");
    REQUIRE(globalOptions.rejection()->responseHeaders.size() == 1U);
    REQUIRE((globalOptions.rejection()->responseHeaders.front() ==
             Dashboard::DashboardHttpHeader{"allow", "GET"}));

    const auto unknownOptions = planned(
        requestPolicy,
        request("OPTIONS", "/unknown", headers(true)));
    requireRejected(unknownOptions, 405U, "method_not_allowed");
    REQUIRE(unknownOptions.rejection()->responseHeaders.empty());

    const auto stopped = planned(
        requestPolicy,
        request("GET", "/api/doctor", headers(true)),
        false);
    requireRejected(stopped, 503U, "service_stopped");

    const auto unknown = planned(
        requestPolicy,
        request("GET", "/unknown", headers(true)));
    requireRejected(unknown, 404U, "not_found");

    const auto readOnlyWrongMethod = planned(
        requestPolicy,
        request("POST", "/api/status", headers(true, true)));
    requireRejected(readOnlyWrongMethod, 405U, "method_not_allowed");
    REQUIRE(readOnlyWrongMethod.rejection()->responseHeaders.size() == 1U);
    REQUIRE((readOnlyWrongMethod.rejection()->responseHeaders.front() ==
             Dashboard::DashboardHttpHeader{"allow", "GET"}));

    const auto settingsWrongMethod = planned(
        requestPolicy,
        request("PATCH", "/api/manager/settings", headers(true, true)));
    requireRejected(settingsWrongMethod, 405U, "method_not_allowed");
    REQUIRE(settingsWrongMethod.rejection()->responseHeaders.size() == 1U);
    REQUIRE((settingsWrongMethod.rejection()->responseHeaders.front() ==
             Dashboard::DashboardHttpHeader{
                 "allow", "GET, POST, PUT"}));
}

void derivesCanonicalStaticResourcesWithoutConsumingCacheBusters()
{
    const auto requestPolicy = policy();
    for (const auto& target : {
             std::string{"/static/app.js"},
             std::string{"/static/app.js?v=1"},
             std::string{"/static/%61pp.js?cache=two"}}) {
        const auto result = planned(
            requestPolicy, request("GET", target, headers()));
        REQUIRE(
            result.kind() == Dashboard::DashboardRequestPlanKind::StaticResource);
        REQUIRE(result.staticResource() != nullptr);
        REQUIRE(result.staticResource()->relativePath() == "app.js");
    }

    for (const auto& target : {
             std::string{"/static/"},
             std::string{"/static/App.js"},
             std::string{"/static/app.exe"},
             std::string{"/static/app%2fchild.js"},
             std::string{"/static/con.js"}}) {
        const auto result = planned(
            requestPolicy, request("GET", target, headers()));
        requireRejected(result, 404U, "not_found");
    }
}

void preservesCompatibilityRatesThenAppliesTheResourceCeiling()
{
    const auto requestPolicy = policy();

    const auto low = planned(
        requestPolicy,
        request("GET", "/api/stream?hz=0.5", headers(true)));
    REQUIRE(low.streamRate() != nullptr);
    REQUIRE(low.streamRate()->normalizedRequestedHz == 1.0);
    REQUIRE(low.streamRate()->deliveryHz == 1.0);
    REQUIRE(low.streamRate()->source ==
            Dashboard::DashboardStreamRateSource::ExplicitHertz);
    REQUIRE(low.streamRate()->compatibilityClamped);
    REQUIRE(!low.streamRate()->resourceCapped);

    const auto high = planned(
        requestPolicy,
        request("GET", "/api/stream?hz=100", headers(true)));
    REQUIRE(high.streamRate() != nullptr);
    REQUIRE(high.streamRate()->normalizedRequestedHz == 60.0);
    REQUIRE(high.streamRate()->deliveryHz == 2.0);
    REQUIRE(high.streamRate()->compatibilityClamped);
    REQUIRE(high.streamRate()->resourceCapped);

    const auto legacy = planned(
        requestPolicy,
        request("GET", "/api/stream?interval=0.05", headers(true)));
    REQUIRE(legacy.streamRate() != nullptr);
    REQUIRE(std::abs(legacy.streamRate()->normalizedRequestedHz - 20.0) <
            0.000001);
    REQUIRE(legacy.streamRate()->deliveryHz == 2.0);
    REQUIRE(legacy.streamRate()->source ==
            Dashboard::DashboardStreamRateSource::LegacyInterval);
    REQUIRE(!legacy.streamRate()->compatibilityClamped);
    REQUIRE(legacy.streamRate()->resourceCapped);

    for (const auto& target : {
             std::string{"/api/stream?"},
             std::string{"/api/stream?hz=nan"},
             std::string{"/api/stream?hz=2&extra=1"},
             std::string{"/api/stream?interval=%30.1"}}) {
        const auto result = planned(
            requestPolicy, request("GET", target, headers(true)));
        requireRejected(result, 400U, "invalid_query");
    }

    const auto invalidBudget = planned(
        requestPolicy,
        request("GET", "/api/stream", headers(true)),
        true,
        0.0);
    requireRejected(invalidBudget, 400U, "invalid_query");
}

} // namespace

int main()
{
    try {
        producesOwnedPlansWithExclusivePayloads();
        policyAlwaysPrecedesRoutingAndRouteSpecificDecoding();
        mapsEveryRouteDispositionToAStableHttpRejection();
        derivesCanonicalStaticResourcesWithoutConsumingCacheBusters();
        preservesCompatibilityRatesThenAppliesTheResourceCeiling();
        std::cout << "Dashboard request planner tests passed (" << assertions
                  << " assertions).\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Dashboard request planner tests failed: " << error.what()
                  << '\n';
        return 1;
    } catch (...) {
        std::cerr
            << "Dashboard request planner tests failed with an unknown error.\n";
        return 1;
    }
}
