#include "ForgeConductor/Dashboard/DashboardStaticResourcePath.h"

#include "ForgeConductor/Domain/Error.h"

#include <array>
#include <cstddef>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

namespace {

namespace Dashboard = ForgeConductor::Dashboard;
namespace Domain = ForgeConductor::Domain;

static_assert(std::is_final_v<Dashboard::DashboardStaticResourcePath>);
static_assert(!std::is_default_constructible_v<
              Dashboard::DashboardStaticResourcePath>);
static_assert(std::is_same_v<
              decltype(Dashboard::DashboardStaticResourcePath::decode(
                  std::declval<std::string_view>())),
              Domain::Result<Dashboard::DashboardStaticResourcePath>>);
static_assert(noexcept(Dashboard::DashboardStaticResourcePath::decode(
    std::declval<std::string_view>())));
static_assert(noexcept(
    std::declval<const Dashboard::DashboardStaticResourcePath&>()
        .relativePath()));
static_assert(noexcept(
    std::declval<const Dashboard::DashboardStaticResourcePath&>().mimeType()));

std::size_t assertions{};

[[noreturn]] void fail(
    const std::string_view expression,
    const std::string_view detail = {})
{
    std::string message{"requirement failed: "};
    message.append(expression);
    if (!detail.empty()) {
        message.append(" (");
        message.append(detail);
        message.push_back(')');
    }
    throw std::runtime_error{std::move(message)};
}

void require(
    const bool condition,
    const std::string_view expression,
    const std::string_view detail = {})
{
    ++assertions;
    if (!condition) {
        fail(expression, detail);
    }
}

#define REQUIRE(condition) require(static_cast<bool>(condition), #condition)

[[nodiscard]] Dashboard::DashboardStaticResourcePath accepted(
    const std::string_view rawTarget)
{
    auto result = Dashboard::DashboardStaticResourcePath::decode(rawTarget);
    if (!result) {
        fail(rawTarget, result.error().code + ": " + result.error().message);
    }
    return std::move(result).value();
}

void rejected(
    const std::string_view rawTarget,
    const std::string_view errorCode,
    const std::string_view exactMessage = {})
{
    const auto result = Dashboard::DashboardStaticResourcePath::decode(rawTarget);
    require(!result, "!result", rawTarget);
    if (result) {
        return;
    }
    require(result.error().code == errorCode, "error code", rawTarget);
    require(!result.error().retryable, "!retryable", rawTarget);
    require(!result.error().evidenceId.has_value(), "!evidenceId", rawTarget);
    if (!exactMessage.empty()) {
        require(result.error().message == exactMessage, "error message", rawTarget);
    }
}

[[nodiscard]] std::string percentByte(const unsigned int value)
{
    constexpr std::string_view digits = "0123456789abcdef";
    std::string escaped{"%00"};
    escaped[1U] = digits[(value >> 4U) & 0x0fU];
    escaped[2U] = digits[value & 0x0fU];
    return escaped;
}

void decodesCanonicalPathsAndClosedMimeAllowlist()
{
    struct AcceptedCase final {
        std::string_view rawTarget;
        std::string_view relativePath;
        std::string_view mimeType;
    };
    constexpr std::array cases{
        AcceptedCase{
            "/static/index.html", "index.html", "text/html; charset=utf-8"},
        AcceptedCase{
            "/static/app.css", "app.css", "text/css; charset=utf-8"},
        AcceptedCase{
            "/static/app.js", "app.js",
            "application/javascript; charset=utf-8"},
        AcceptedCase{
            "/static/config.json", "config.json",
            "application/json; charset=utf-8"},
        AcceptedCase{
            "/static/assets/app.min.js", "assets/app.min.js",
            "application/javascript; charset=utf-8"},
        AcceptedCase{
            "/static/v2/dashboard-shell_01.css",
            "v2/dashboard-shell_01.css", "text/css; charset=utf-8"},
        AcceptedCase{
            "/static/%61pp.%6as", "app.js",
            "application/javascript; charset=utf-8"},
        AcceptedCase{
            "/static/%61pp.%6As", "app.js",
            "application/javascript; charset=utf-8"},
        AcceptedCase{
            "/static/a%2Db/data%5fset.json", "a-b/data_set.json",
            "application/json; charset=utf-8"},
    };

    for (const auto& entry : cases) {
        const auto result = accepted(entry.rawTarget);
        REQUIRE(result.relativePath() == entry.relativePath);
        REQUIRE(result.mimeType() == entry.mimeType);
    }

    for (const auto target : {
             "/static/readme", "/static/readme.txt", "/static/app.mjs",
             "/static/app.htm", "/static/app.svg", "/static/app.png",
             "/static/app.js.map", "/static/app.exe"}) {
        rejected(
            target,
            Domain::ErrorCodes::InvalidRequest,
            target == std::string_view{"/static/readme"}
                ? "Dashboard static resources require an allowed file extension."
                : std::string_view{});
    }
    rejected(
        "/static/.js",
        Domain::ErrorCodes::PathOutsideAuthority,
        "Dashboard static-resource paths must use canonical lowercase ASCII segments.");
}

void requiresTheExactAlreadyClassifiedPrefixAndNoQueryAmbiguity()
{
    for (const auto target : {
             "", "/", "/static", "/STATIC/app.js", "/Static/app.js",
             "/static%2fapp.js", "/%73tatic/app.js", "static/app.js",
             "//static/app.js", "/api/static/app.js"}) {
        rejected(
            target,
            Domain::ErrorCodes::InvalidRequest,
            "The dashboard static-resource target must begin with /static/.");
    }

    rejected(
        "/static/",
        Domain::ErrorCodes::InvalidRequest,
        "The dashboard static-resource suffix is empty.");
    for (const auto target : {
             "/static/app.js?", "/static/app.js?v=1",
             "/static/app.js?next=/api/status", "/static/app.js#",
             "/static/app.js#fragment", "/static/a?b/c.js"}) {
        rejected(
            target,
            Domain::ErrorCodes::InvalidRequest,
            "Dashboard static-resource paths may not contain a query or fragment delimiter.");
    }
    for (const auto target : {
             "/static/app%3f.js", "/static/app%3F.js",
             "/static/app%23.js"}) {
        rejected(
            target,
            Domain::ErrorCodes::InvalidRequest,
            "Dashboard static-resource paths may not contain encoded query or fragment delimiters.");
    }
}

void rejectsEveryMalformedPercentAndUtf8Form()
{
    for (const auto target : {
             "/static/%.js", "/static/%0.js", "/static/%gg.js",
             "/static/%0x.js", "/static/%x0.js", "/static/app.%",
             "/static/app.%2", "/static/app.%2g", "/static/%u0061pp.js"}) {
        rejected(
            target,
            Domain::ErrorCodes::InvalidRequest,
            "Dashboard static-resource paths contain a malformed percent escape.");
    }

    for (const auto target : {
             "/static/%80.js", "/static/%bf.js", "/static/%c0%af.js",
             "/static/%c1%bf.js", "/static/%e0%80%af.js",
             "/static/%ed%a0%80.js", "/static/%f0%80%80%af.js",
             "/static/%f4%90%80%80.js", "/static/%f5%80%80%80.js",
             "/static/%ff.js", "/static/%c2.js", "/static/%e1%80.js",
             "/static/%f1%80%80.js", "/static/%c2a.js",
             "/static/%e1%80a.js", "/static/%f1%80%80a.js"}) {
        rejected(
            target,
            Domain::ErrorCodes::InvalidRequest,
            "The decoded dashboard static-resource path must be strict control-free UTF-8.");
    }
}

void rejectsNulAndAllControlCodePoints()
{
    for (unsigned int value = 0U; value <= 0x1fU; ++value) {
        const auto encoded = "/static/a" + percentByte(value) + ".js";
        rejected(
            encoded,
            Domain::ErrorCodes::InvalidRequest,
            "The decoded dashboard static-resource path must be strict control-free UTF-8.");

        std::string literal{"/static/a"};
        literal.push_back(static_cast<char>(value));
        literal.append(".js");
        rejected(
            literal,
            Domain::ErrorCodes::InvalidRequest,
            "The decoded dashboard static-resource path must be strict control-free UTF-8.");
    }
    rejected(
        "/static/a%7f.js",
        Domain::ErrorCodes::InvalidRequest,
        "The decoded dashboard static-resource path must be strict control-free UTF-8.");

    for (unsigned int value = 0x80U; value <= 0x9fU; ++value) {
        const auto encoded =
            "/static/a%c2" + percentByte(value) + ".js";
        rejected(
            encoded,
            Domain::ErrorCodes::InvalidRequest,
            "The decoded dashboard static-resource path must be strict control-free UTF-8.");
    }
}

void rejectsTraversalAndEveryUnsafeSeparatorSpelling()
{
    for (const auto target : {
             "/static/./app.js", "/static/../app.js",
             "/static/a/./app.js", "/static/a/../app.js",
             "/static/app.js/.", "/static/app.js/..",
             "/static/%2e/app.js", "/static/%2E/app.js",
             "/static/%2e%2e/app.js", "/static/%2E%2E/app.js",
             "/static/.%2e/app.js", "/static/%2e./app.js"}) {
        rejected(
            target,
            Domain::ErrorCodes::PathOutsideAuthority,
            "Dashboard static-resource paths may not contain empty, dot, or dot-dot segments.");
    }

    for (const auto target : {
             "/static//app.js", "/static/a//app.js", "/static/app.js/",
             "/static///app.js"}) {
        rejected(
            target,
            Domain::ErrorCodes::PathOutsideAuthority,
            "Dashboard static-resource paths may not contain empty, dot, or dot-dot segments.");
    }

    for (const auto target : {
             "/static/%2fapp.js", "/static/%2Fapp.js",
             "/static/a%2fb.js", "/static/a%2Fb.js",
             "/static/%5capp.js", "/static/%5Capp.js",
             "/static/a\\b.js", "/static/\\\\server\\app.js"}) {
        rejected(
            target,
            Domain::ErrorCodes::PathOutsideAuthority,
            "Dashboard static-resource paths may not contain encoded separators or backslashes.");
    }

    rejected(
        "/static/%252e%252e/app.js",
        Domain::ErrorCodes::PathOutsideAuthority,
        "Dashboard static-resource paths must use canonical lowercase ASCII segments.");
}

void rejectsWindowsRootDeviceAndLexicalAliases()
{
    for (const auto target : {
             "/static/c:/app.js", "/static/c%3a/app.js",
             "/static/file:/app.js", "/static/app.js:stream",
             "/static/app%3a.js", "/static/app .js",
             "/static/app%20.js", "/static/app.js.",
             "/static/.hidden.js", "/static/-app.js",
             "/static/app~1.js", "/static/app<1.js",
             "/static/app>1.js", "/static/app\"1.js",
             "/static/app|1.js", "/static/app*1.js"}) {
        rejected(target, Domain::ErrorCodes::PathOutsideAuthority);
    }

    constexpr std::array fixedDevices{"con", "prn", "aux", "nul"};
    for (const auto device : fixedDevices) {
        rejected(
            "/static/" + std::string{device} + ".js",
            Domain::ErrorCodes::PathOutsideAuthority,
            "Dashboard static-resource paths may not use Windows device names.");
        rejected(
            "/static/assets/" + std::string{device} + ".json",
            Domain::ErrorCodes::PathOutsideAuthority,
            "Dashboard static-resource paths may not use Windows device names.");
    }
    for (const auto prefix : {"com", "lpt"}) {
        for (char number = '0'; number <= '9'; ++number) {
            const std::string device = std::string{prefix} + number;
            rejected(
                "/static/" + device + ".css",
                Domain::ErrorCodes::PathOutsideAuthority,
                "Dashboard static-resource paths may not use Windows device names.");
        }
    }

    REQUIRE(accepted("/static/conductor.js").relativePath() == "conductor.js");
    REQUIRE(accepted("/static/com10.js").relativePath() == "com10.js");
    REQUIRE(accepted("/static/lpt10.js").relativePath() == "lpt10.js");
}

void rejectsCaseAndUnicodeNormalizationAliases()
{
    for (char uppercase = 'A'; uppercase <= 'Z'; ++uppercase) {
        std::string target{"/static/a"};
        target.push_back(uppercase);
        target.append(".js");
        rejected(
            target,
            Domain::ErrorCodes::PathOutsideAuthority,
            "Dashboard static-resource paths must use canonical lowercase ASCII segments.");
    }
    for (const auto target : {
             "/static/App.js", "/static/app.JS", "/static/APP.JS",
             "/static/%41pp.js", "/static/app.%4as",
             "/static/a%50p.js"}) {
        rejected(
            target,
            Domain::ErrorCodes::PathOutsideAuthority,
            "Dashboard static-resource paths must use canonical lowercase ASCII segments.");
    }

    const std::array unicodeAliases{
        std::string{"/static/caf\xc3\xa9.js"},
        std::string{"/static/cafe\xcc\x81.js"},
        std::string{"/static/%63%61%66%c3%a9.js"},
        std::string{"/static/%63%61%66%65%cc%81.js"},
        std::string{"/static/%ef%bd%81pp.js"},
    };
    for (const auto& target : unicodeAliases) {
        rejected(
            target,
            Domain::ErrorCodes::PathOutsideAuthority,
            "Dashboard static-resource paths must use canonical lowercase ASCII segments.");
    }
}

void enforcesRawDecodedAndSegmentBounds()
{
    std::string exactDecoded = "/static/";
    exactDecoded.append(255U, 'a');
    exactDecoded.push_back('/');
    exactDecoded.append(255U, 'b');
    exactDecoded.push_back('/');
    exactDecoded.append(255U, 'c');
    exactDecoded.push_back('/');
    exactDecoded.append(251U, 'd');
    exactDecoded.append("/a.js");
    const auto exact = accepted(exactDecoded);
    REQUIRE(exact.relativePath().size() ==
            Dashboard::DashboardStaticResourcePath::MaximumDecodedBytes);
    REQUIRE(exact.mimeType() == "application/javascript; charset=utf-8");

    std::string decodedOverflow = "/static/";
    decodedOverflow.append(255U, 'a');
    decodedOverflow.push_back('/');
    decodedOverflow.append(255U, 'b');
    decodedOverflow.push_back('/');
    decodedOverflow.append(255U, 'c');
    decodedOverflow.push_back('/');
    decodedOverflow.append(252U, 'd');
    decodedOverflow.append("/a.js");
    rejected(
        decodedOverflow,
        Domain::ErrorCodes::PayloadTooLarge,
        "The decoded dashboard static-resource path exceeds the 1024-byte limit.");

    std::string segmentAtLimit = "/static/";
    segmentAtLimit.append(252U, 'a');
    segmentAtLimit.append(".js");
    REQUIRE(accepted(segmentAtLimit).relativePath().size() ==
            Dashboard::DashboardStaticResourcePath::MaximumSegmentBytes);

    std::string segmentOverflow = "/static/";
    segmentOverflow.append(253U, 'a');
    segmentOverflow.append(".js");
    rejected(
        segmentOverflow,
        Domain::ErrorCodes::PayloadTooLarge,
        "A dashboard static-resource path segment exceeds the 255-byte limit.");

    std::string percentDecodedOverflow{"/static/"};
    for (std::size_t index = 0U;
         index < Dashboard::DashboardStaticResourcePath::MaximumDecodedBytes + 1U;
         ++index) {
        percentDecodedOverflow.append("%61");
    }
    percentDecodedOverflow.replace(
        percentDecodedOverflow.size() - 9U, 9U, "%2e%6a%73");
    rejected(
        percentDecodedOverflow,
        Domain::ErrorCodes::PayloadTooLarge,
        "The decoded dashboard static-resource path exceeds the 1024-byte limit.");

    std::string rawOverflow(
        Dashboard::DashboardStaticResourcePath::MaximumRawTargetBytes + 1U,
        'a');
    rawOverflow.replace(0U, std::string_view{"/static/"}.size(), "/static/");
    rejected(
        rawOverflow,
        Domain::ErrorCodes::PayloadTooLarge,
        "The dashboard static-resource target exceeds the 8192-byte limit.");
}

void resultsAreOwnedAndDeterministic()
{
    std::string target{"/static/assets/%61pp.min.js"};
    auto first = accepted(target);
    target.assign("/static/changed.css");
    REQUIRE(first.relativePath() == "assets/app.min.js");
    REQUIRE(first.mimeType() == "application/javascript; charset=utf-8");

    const auto second = accepted("/static/assets/%61pp.min.js");
    REQUIRE(first == second);
    REQUIRE(first.relativePath().data() != target.data());

    const auto firstFailure = Dashboard::DashboardStaticResourcePath::decode(
        "/static/../app.js");
    const auto secondFailure = Dashboard::DashboardStaticResourcePath::decode(
        "/static/../app.js");
    REQUIRE(!firstFailure);
    REQUIRE(!secondFailure);
    REQUIRE(firstFailure.error() == secondFailure.error());
}

} // namespace

int main()
{
    try {
        decodesCanonicalPathsAndClosedMimeAllowlist();
        requiresTheExactAlreadyClassifiedPrefixAndNoQueryAmbiguity();
        rejectsEveryMalformedPercentAndUtf8Form();
        rejectsNulAndAllControlCodePoints();
        rejectsTraversalAndEveryUnsafeSeparatorSpelling();
        rejectsWindowsRootDeviceAndLexicalAliases();
        rejectsCaseAndUnicodeNormalizationAliases();
        enforcesRawDecodedAndSegmentBounds();
        resultsAreOwnedAndDeterministic();
        std::cout << "Dashboard static-resource path tests passed ("
                  << assertions << " assertions).\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Dashboard static-resource path tests failed: "
                  << error.what() << '\n';
        return 1;
    } catch (...) {
        std::cerr << "Dashboard static-resource path tests failed with an unknown error.\n";
        return 1;
    }
}
