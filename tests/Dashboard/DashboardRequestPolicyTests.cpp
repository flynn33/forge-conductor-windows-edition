#include "ForgeConductor/Dashboard/DashboardRequestPolicy.h"

#include "ForgeConductor/Domain/Error.h"

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

static_assert(std::is_final_v<Dashboard::DashboardRequestPolicy>);
static_assert(std::is_same_v<
              decltype(Dashboard::DashboardRequestPolicy::create(
                  std::declval<std::string>(),
                  std::declval<std::uint16_t>(),
                  std::declval<Domain::Sha256Digest>())),
              Domain::Result<Dashboard::DashboardRequestPolicy>>);
static_assert(noexcept(Dashboard::DashboardRequestPolicy::create(
    std::declval<std::string>(),
    std::declval<std::uint16_t>(),
    std::declval<Domain::Sha256Digest>())));
static_assert(noexcept(Dashboard::DashboardRequestPolicy::isConfiguredLoopbackHost(
    std::declval<std::string_view>())));
static_assert(noexcept(std::declval<const Dashboard::DashboardRequestPolicy&>()
                           .rejectionFor(
                               std::declval<const Dashboard::DashboardHttpRequest&>())));

[[noreturn]] void fail(const std::string_view expression)
{
    throw std::runtime_error{"requirement failed: " + std::string{expression}};
}

void require(const bool condition, const std::string_view expression)
{
    if (!condition) {
        fail(expression);
    }
}

#define REQUIRE(condition) require(static_cast<bool>(condition), #condition)

template <typename T>
[[nodiscard]] T take(Domain::Result<T> result)
{
    if (!result) {
        throw std::runtime_error{result.error().code + ": " + result.error().message};
    }
    return std::move(result).value();
}

[[nodiscard]] Domain::Sha256Digest token(const char character = 'a')
{
    return take(Domain::Sha256Digest::parse(std::string(64U, character)));
}

[[nodiscard]] Dashboard::DashboardRequestPolicy policy(
    std::string host = "127.0.0.1",
    const std::uint16_t port = 47820U)
{
    return take(Dashboard::DashboardRequestPolicy::create(
        std::move(host), port, token()));
}

[[nodiscard]] Dashboard::DashboardHttpRequest request(
    std::string method,
    std::string target,
    std::vector<Dashboard::DashboardHttpHeader> headers = {},
    std::vector<std::byte> body = {})
{
    return Dashboard::DashboardHttpRequest{
        std::move(method),
        std::move(target),
        std::move(headers),
        std::move(body)};
}

[[nodiscard]] std::vector<Dashboard::DashboardHttpHeader> headers(
    std::string host = "127.0.0.1:47820",
    std::optional<std::string> authorization = std::nullopt,
    std::optional<std::string> contentType = std::nullopt,
    std::optional<std::string> origin = std::nullopt,
    std::optional<std::string> fetchSite = std::nullopt)
{
    std::vector<Dashboard::DashboardHttpHeader> result{
        {"host", std::move(host)}};
    if (authorization) {
        result.push_back({"authorization", std::move(*authorization)});
    }
    if (contentType) {
        result.push_back({"content-type", std::move(*contentType)});
    }
    if (origin) {
        result.push_back({"origin", std::move(*origin)});
    }
    if (fetchSite) {
        result.push_back({"sec-fetch-site", std::move(*fetchSite)});
    }
    return result;
}

[[nodiscard]] std::string bearer(const char character = 'a')
{
    return "Bearer " + std::string(64U, character);
}

void requireAccepted(
    const Dashboard::DashboardRequestPolicy& candidate,
    const Dashboard::DashboardHttpRequest& input)
{
    REQUIRE(!candidate.rejectionFor(input).has_value());
}

void requireRejected(
    const Dashboard::DashboardRequestPolicy& candidate,
    const Dashboard::DashboardHttpRequest& input,
    const std::uint16_t status,
    const std::string_view code)
{
    const auto rejected = candidate.rejectionFor(input);
    REQUIRE(rejected.has_value());
    REQUIRE(rejected->status == status);
    REQUIRE(rejected->code == code);
}

void creationAcceptsOnlyExactLoopbackLiteralsAndNonzeroPorts()
{
    REQUIRE(Dashboard::DashboardRequestPolicy::isConfiguredLoopbackHost("127.0.0.1"));
    REQUIRE(Dashboard::DashboardRequestPolicy::isConfiguredLoopbackHost("::1"));
    REQUIRE(!Dashboard::DashboardRequestPolicy::isConfiguredLoopbackHost("localhost"));
    REQUIRE(!Dashboard::DashboardRequestPolicy::isConfiguredLoopbackHost("127.0.0.1 "));
    REQUIRE(!Dashboard::DashboardRequestPolicy::isConfiguredLoopbackHost("0.0.0.0"));
    REQUIRE(!Dashboard::DashboardRequestPolicy::isConfiguredLoopbackHost("::ffff:127.0.0.1"));

    const auto ipv4 = Dashboard::DashboardRequestPolicy::create(
        "127.0.0.1", 1U, token());
    const auto ipv6 = Dashboard::DashboardRequestPolicy::create(
        "::1", 65535U, token());
    REQUIRE(ipv4);
    REQUIRE(ipv4.value().bindHost() == "127.0.0.1");
    REQUIRE(ipv4.value().bindPort() == 1U);
    REQUIRE(ipv6);
    REQUIRE(ipv6.value().bindHost() == "::1");
    REQUIRE(ipv6.value().bindPort() == 65535U);

    for (const auto invalid : {
             "", "localhost", "LOCALHOST", "127.0.0.1 ", " 127.0.0.1",
             "0.0.0.0", "::", "[::1]", "::ffff:127.0.0.1"}) {
        const auto result = Dashboard::DashboardRequestPolicy::create(
            invalid, 47820U, token());
        REQUIRE(!result);
        REQUIRE(result.error().code == Domain::ErrorCodes::InvalidRequest);
    }
    const auto zeroPort = Dashboard::DashboardRequestPolicy::create(
        "127.0.0.1", 0U, token());
    REQUIRE(!zeroPort);
    REQUIRE(zeroPort.error().code == Domain::ErrorCodes::InvalidRequest);
}

void hostMustExactlyMatchTheActiveAuthority()
{
    const auto ipv4 = policy();
    requireAccepted(ipv4, request("GET", "/", headers()));

    for (const auto invalid : {
             "", "localhost:47820", "127.0.0.1", "127.0.0.1:47821",
             "127.0.0.1:047820", "127.0.0.1:47820 ", "[::1]:47820"}) {
        requireRejected(
            ipv4,
            request("GET", "/", headers(invalid)),
            403U,
            "forbidden");
    }
    requireRejected(
        ipv4,
        request("GET", "/"),
        403U,
        "forbidden");

    auto duplicates = headers();
    duplicates.push_back({"host", "127.0.0.1:47820"});
    requireRejected(
        ipv4,
        request("GET", "/", std::move(duplicates)),
        403U,
        "forbidden");

    const auto ipv6 = policy("::1");
    requireAccepted(
        ipv6,
        request("GET", "/", headers("[::1]:47820")));
    requireRejected(
        ipv6,
        request("GET", "/", headers("::1:47820")),
        403U,
        "forbidden");
    requireRejected(
        ipv6,
        request("GET", "/", headers("127.0.0.1:47820")),
        403U,
        "forbidden");
}

void bootstrapAllowlistIsExactAndEverythingElseIsAuthenticated()
{
    const auto candidate = policy();
    for (const auto path : {
             "/", "/index.html", "/control", "/manager", "/ping",
             "/static/app.js", "/static/", "/?view=manager",
             "/static/app.js?v=1"}) {
        requireAccepted(candidate, request("GET", path, headers()));
    }

    for (const auto path : {
             "/api", "/api/status", "/api/unknown", "/api/tools/call",
             "/api/stream", "/static", "/manager/extra", "/ping/extra",
             "/index.html/extra", "/unknown", "/%6danager"}) {
        requireRejected(
            candidate,
            request("GET", path, headers()),
            401U,
            "unauthorized");
    }

    requireAccepted(
        candidate,
        request("GET", "/api/status", headers(
            "127.0.0.1:47820", bearer())));
    requireAccepted(
        candidate,
        request("GET", "/unknown", headers(
            "127.0.0.1:47820", bearer())));
}

void bearerSyntaxAndDigestAreBothRequired()
{
    const auto candidate = policy();
    const auto input = [&candidate](std::optional<std::string> authorization) {
        return candidate.rejectionFor(request(
            "GET",
            "/api/status",
            headers("127.0.0.1:47820", std::move(authorization))));
    };

    REQUIRE(!input(bearer()).has_value());
    for (const auto& invalid : std::vector<std::optional<std::string>>{
             std::nullopt,
             std::string{},
             std::string{"bearer "} + std::string(64U, 'a'),
             std::string{"BEARER "} + std::string(64U, 'a'),
             std::string{"Bearer"} + std::string(64U, 'a'),
             std::string{"Bearer  "} + std::string(64U, 'a'),
             std::string{"Bearer "} + std::string(63U, 'a'),
             std::string{"Bearer "} + std::string(65U, 'a'),
             std::string{"Bearer "} + std::string(64U, 'A'),
             std::string{"Bearer "} + std::string(63U, 'a') + "g",
             bearer('b'),
             bearer() + " "}) {
        const auto rejected = input(invalid);
        REQUIRE(rejected.has_value());
        REQUIRE(rejected->status == 401U);
        REQUIRE(rejected->code == "unauthorized");
        REQUIRE(rejected->responseHeaders.size() == 1U);
        const Dashboard::DashboardHttpHeader challenge{
            "www-authenticate", "Bearer"};
        REQUIRE(rejected->responseHeaders.front() == challenge);
    }

    auto duplicates = headers("127.0.0.1:47820", bearer());
    duplicates.push_back({"authorization", bearer()});
    requireRejected(
        candidate,
        request("GET", "/api/status", std::move(duplicates)),
        401U,
        "unauthorized");
}

void mutationsRequireTheNarrowJsonMediaTypeGrammar()
{
    const auto candidate = policy();
    for (const auto method : {"POST", "PUT", "PATCH", "DELETE"}) {
        for (const auto contentType : {
                 "application/json",
                 "Application/JSON",
                 " application/json ",
                 "application/json;charset=utf-8",
                 "application/json ; charset = utf-8",
                 "APPLICATION/JSON;CHARSET=UTF-8",
                 "application/json;\tcharset\t=\tutf-8\t"}) {
            requireAccepted(
                candidate,
                request(
                    method,
                    "/api/settings",
                    headers(
                        "127.0.0.1:47820",
                        bearer(),
                        contentType)));
        }

        requireRejected(
            candidate,
            request(
                method,
                "/api/settings",
                headers("127.0.0.1:47820", bearer())),
            415U,
            "unsupported_media_type");
        for (const auto contentType : {
                 "", "text/plain", "application/jsonp",
                 "application/json;", "application/json; charset",
                 "application/json; charset=", "application/json; charset=ascii",
                 "application/json; charset=\"utf-8\"",
                 "application/json; charset=utf-8; charset=utf-8",
                 "application/json; charset=utf-8; version=1",
                 "application/json; version=1",
                 "application/json; charset=utf-8=extra"}) {
            requireRejected(
                candidate,
                request(
                    method,
                    "/api/settings",
                    headers(
                        "127.0.0.1:47820",
                        bearer(),
                        contentType)),
                415U,
                "unsupported_media_type");
        }
    }

    auto duplicates = headers(
        "127.0.0.1:47820", bearer(), "application/json");
    duplicates.push_back({"content-type", "application/json"});
    requireRejected(
        candidate,
        request("POST", "/api/settings", std::move(duplicates)),
        415U,
        "unsupported_media_type");

    requireAccepted(
        candidate,
        request(
            "GET",
            "/api/status",
            headers(
                "127.0.0.1:47820",
                bearer(),
                "text/plain")));
}

void mutationOriginAndFetchMetadataAreConstrained()
{
    const auto candidate = policy();
    const auto valid = [&candidate](
                           std::optional<std::string> origin,
                           std::optional<std::string> fetchSite) {
        return candidate.rejectionFor(request(
            "POST",
            "/api/manager/restart",
            headers(
                "127.0.0.1:47820",
                bearer(),
                "application/json",
                std::move(origin),
                std::move(fetchSite))));
    };

    REQUIRE(!valid(std::nullopt, std::nullopt).has_value());
    REQUIRE(!valid("http://127.0.0.1:47820", "same-origin").has_value());
    REQUIRE(!valid("http://127.0.0.1:47820", "none").has_value());

    for (const auto invalidOrigin : {
             "http://localhost:47820", "http://127.0.0.1:47821",
             "https://127.0.0.1:47820", "HTTP://127.0.0.1:47820",
             "http://127.0.0.1:47820/", " http://127.0.0.1:47820"}) {
        const auto rejected = valid(invalidOrigin, "same-origin");
        REQUIRE(rejected.has_value());
        REQUIRE(rejected->status == 403U);
        REQUIRE(rejected->code == "forbidden");
    }
    for (const auto invalidFetch : {
             "cross-site", "same-site", "SAME-ORIGIN", " none", ""}) {
        const auto rejected = valid(
            "http://127.0.0.1:47820", invalidFetch);
        REQUIRE(rejected.has_value());
        REQUIRE(rejected->status == 403U);
        REQUIRE(rejected->code == "forbidden");
    }

    auto duplicateOrigin = headers(
        "127.0.0.1:47820",
        bearer(),
        "application/json",
        "http://127.0.0.1:47820",
        "same-origin");
    duplicateOrigin.push_back({"origin", "http://127.0.0.1:47820"});
    requireRejected(
        candidate,
        request("POST", "/api/settings", std::move(duplicateOrigin)),
        403U,
        "forbidden");

    auto duplicateFetch = headers(
        "127.0.0.1:47820",
        bearer(),
        "application/json",
        "http://127.0.0.1:47820",
        "same-origin");
    duplicateFetch.push_back({"sec-fetch-site", "same-origin"});
    requireRejected(
        candidate,
        request("POST", "/api/settings", std::move(duplicateFetch)),
        403U,
        "forbidden");

    const auto ipv6 = policy("::1");
    requireAccepted(
        ipv6,
        request(
            "POST",
            "/api/settings",
            headers(
                "[::1]:47820",
                bearer(),
                "application/json",
                "http://[::1]:47820",
                "same-origin")));
}

void cookiesAndQueryValuesNeverSubstituteForAuthorization()
{
    const auto candidate = policy();
    auto cookieOnly = headers();
    cookieOnly.push_back({"cookie", "authorization=" + bearer()});
    requireRejected(
        candidate,
        request(
            "GET",
            "/api/status?access_token=" + std::string(64U, 'a'),
            std::move(cookieOnly)),
        401U,
        "unauthorized");

    auto authorized = headers("127.0.0.1:47820", bearer());
    authorized.push_back({"cookie", "authorization=Bearer invalid"});
    requireAccepted(
        candidate,
        request(
            "GET",
            "/api/status?access_token=invalid",
            std::move(authorized)));
}

void rejectionPrecedenceIsHostThenAuthenticationThenMutationPolicy()
{
    const auto candidate = policy();
    requireRejected(
        candidate,
        request(
            "POST",
            "/api/settings",
            headers("localhost:47820")),
        403U,
        "forbidden");
    requireRejected(
        candidate,
        request(
            "POST",
            "/api/settings",
            headers("127.0.0.1:47820", std::nullopt, "text/plain")),
        401U,
        "unauthorized");
    requireRejected(
        candidate,
        request(
            "POST",
            "/api/settings",
            headers(
                "127.0.0.1:47820",
                bearer(),
                "text/plain",
                "http://evil.invalid")),
        415U,
        "unsupported_media_type");
    requireRejected(
        candidate,
        request(
            "POST",
            "/api/settings",
            headers(
                "127.0.0.1:47820",
                bearer(),
                "application/json",
                "http://evil.invalid")),
        403U,
        "forbidden");
}

} // namespace

int main()
{
    try {
        creationAcceptsOnlyExactLoopbackLiteralsAndNonzeroPorts();
        hostMustExactlyMatchTheActiveAuthority();
        bootstrapAllowlistIsExactAndEverythingElseIsAuthenticated();
        bearerSyntaxAndDigestAreBothRequired();
        mutationsRequireTheNarrowJsonMediaTypeGrammar();
        mutationOriginAndFetchMetadataAreConstrained();
        cookiesAndQueryValuesNeverSubstituteForAuthorization();
        rejectionPrecedenceIsHostThenAuthenticationThenMutationPolicy();
        std::cout << "Dashboard request policy tests passed.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Dashboard request policy tests failed: " << error.what()
                  << '\n';
        return 1;
    } catch (...) {
        std::cerr << "Dashboard request policy tests failed with an unknown error.\n";
        return 1;
    }
}
