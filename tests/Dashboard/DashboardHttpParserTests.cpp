#include "ForgeConductor/Dashboard/DashboardHttpParser.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

namespace Dashboard = ForgeConductor::Dashboard;

std::size_t assertions{};

#define REQUIRE(condition)                                                       \
    do {                                                                         \
        ++assertions;                                                            \
        if (!(condition)) {                                                      \
            throw std::runtime_error{std::string{"Requirement failed: "} +      \
                                     #condition};                                \
        }                                                                        \
    } while (false)

[[nodiscard]] std::vector<std::byte> bytes(const std::string_view value)
{
    std::vector<std::byte> result;
    result.reserve(value.size());
    std::transform(
        value.begin(), value.end(), std::back_inserter(result),
        [](const char character) {
            return static_cast<std::byte>(
                static_cast<unsigned char>(character));
        });
    return result;
}

[[nodiscard]] Dashboard::DashboardHttpParseResult parse(
    const std::string_view wire,
    const bool streamComplete = true)
{
    const auto input = bytes(wire);
    return Dashboard::DashboardHttpParser{}.parse(input, streamComplete);
}

[[nodiscard]] const Dashboard::DashboardHttpRequest& accepted(
    const Dashboard::DashboardHttpParseResult& result)
{
    REQUIRE(result.kind() == Dashboard::DashboardHttpParseResult::Kind::Accepted);
    REQUIRE(result.request() != nullptr);
    REQUIRE(result.rejection() == nullptr);
    return *result.request();
}

[[nodiscard]] const Dashboard::DashboardHttpRejection& rejected(
    const Dashboard::DashboardHttpParseResult& result,
    const std::uint16_t status)
{
    REQUIRE(result.kind() == Dashboard::DashboardHttpParseResult::Kind::Rejected);
    REQUIRE(result.request() == nullptr);
    REQUIRE(result.rejection() != nullptr);
    REQUIRE(result.rejection()->status == status);
    REQUIRE(result.consumedBytes() == 0U);
    return *result.rejection();
}

void acceptsAndCanonicalizesABoundedRequest()
{
    std::string wire =
        "pOsT /api/status?name=forge%20conductor HTTP/1.1\r\n"
        "Host:\t127.0.0.1:7788 \t\r\n"
        "X-Display: Forge \xE2\x98\x83\r\n"
        "X-Tab: left\tright\r\n"
        "Content-Length: 4\r\n"
        "\r\n";
    wire.append(std::string_view{"{}\xFF\0", 4U});
    const auto result = parse(wire);
    const auto& request = accepted(result);

    REQUIRE(request.method() == "POST");
    REQUIRE(request.target() == "/api/status?name=forge%20conductor");
    REQUIRE(request.path() == "/api/status");
    REQUIRE(request.isMutation());
    REQUIRE(request.headers().size() == 4U);
    REQUIRE(request.headers()[0].name == "host");
    REQUIRE(request.headers()[0].value == "127.0.0.1:7788");
    REQUIRE(request.header("x-display") == "Forge \xE2\x98\x83");
    REQUIRE(request.header("x-tab") == "left\tright");
    REQUIRE(!request.header("Host").has_value());
    REQUIRE(request.body() == bytes(std::string_view{"{}\xFF\0", 4U}));
    REQUIRE(result.consumedBytes() == wire.size());

    const auto extension = parse("m-search / HTTP/1.1\r\n\r\n");
    REQUIRE(accepted(extension).method() == "M-SEARCH");
    const auto encoded = parse("GET /a%2fb?x=%7E HTTP/1.1\r\n\r\n");
    REQUIRE(accepted(encoded).target() == "/a%2fb?x=%7E");
}

void distinguishesIncompleteInputFromClosedStreams()
{
    const auto empty = parse({}, false);
    REQUIRE(empty.kind() == Dashboard::DashboardHttpParseResult::Kind::Incomplete);
    REQUIRE(empty.request() == nullptr);
    REQUIRE(empty.rejection() == nullptr);
    REQUIRE(empty.consumedBytes() == 0U);

    REQUIRE(
        parse("GET / HTTP/1.1\r\nHost: example", false).kind() ==
        Dashboard::DashboardHttpParseResult::Kind::Incomplete);
    static_cast<void>(rejected(
        parse("GET / HTTP/1.1\r\nHost: example", true), 400U));

    const auto shortBody = parse(
        "POST / HTTP/1.1\r\nContent-Length: 4\r\n\r\n{}", false);
    REQUIRE(
        shortBody.kind() ==
        Dashboard::DashboardHttpParseResult::Kind::Incomplete);
    static_cast<void>(rejected(
        parse("POST / HTTP/1.1\r\nContent-Length: 4\r\n\r\n{}", true),
        400U));
}

void enforcesStrictRequestLineAndOriginFormTargets()
{
    const std::vector<std::string> invalidRequests{
        "GET / HTTP/1.0\r\n\r\n",
        "GET  / HTTP/1.1\r\n\r\n",
        "GET /  HTTP/1.1\r\n\r\n",
        "GET / HTTP/1.1 extra\r\n\r\n",
        "GET\t/ HTTP/1.1\r\n\r\n",
        "G@T / HTTP/1.1\r\n\r\n",
        " http / HTTP/1.1\r\n\r\n",
        "GET http://127.0.0.1/ HTTP/1.1\r\n\r\n",
        "GET * HTTP/1.1\r\n\r\n",
        "GET /fragment#part HTTP/1.1\r\n\r\n",
        "GET /back\\slash HTTP/1.1\r\n\r\n",
        "GET /bad% HTTP/1.1\r\n\r\n",
        "GET /bad%2 HTTP/1.1\r\n\r\n",
        "GET /bad%GG HTTP/1.1\r\n\r\n",
        std::string{"GET /bad"} + static_cast<char>(0x7f) +
            " HTTP/1.1\r\n\r\n",
        std::string{"GET /bad"} + "\xC3\xA9" + " HTTP/1.1\r\n\r\n"};

    for (const auto& wire : invalidRequests) {
        static_cast<void>(rejected(parse(wire), 400U));
    }

    std::string exactTarget(Dashboard::DashboardHttpParser::MaximumTargetBytes, 'a');
    exactTarget.front() = '/';
    const auto exact = parse("GET " + exactTarget + " HTTP/1.1\r\n\r\n");
    REQUIRE(accepted(exact).target().size() == exactTarget.size());

    exactTarget.push_back('a');
    static_cast<void>(rejected(
        parse("GET " + exactTarget + " HTTP/1.1\r\n\r\n"), 414U));
}

void enforcesHeaderByteAndCountLimits()
{
    const std::string requestLine{"GET / HTTP/1.1"};
    const auto fixedBytes = requestLine.size() + std::string_view{"\r\nX:"}.size();
    REQUIRE(fixedBytes < Dashboard::DashboardHttpParser::MaximumHeaderBytes);

    std::string exact = requestLine + "\r\nX:";
    exact.append(
        Dashboard::DashboardHttpParser::MaximumHeaderBytes - fixedBytes,
        'a');
    exact += "\r\n\r\n";
    REQUIRE(accepted(parse(exact)).header("x").has_value());

    std::string over = exact;
    over.insert(over.size() - 4U, 1U, 'a');
    static_cast<void>(rejected(parse(over), 431U));

    const std::string noDelimiterExact(
        Dashboard::DashboardHttpParser::MaximumHeaderBytes, 'A');
    REQUIRE(
        parse(noDelimiterExact, false).kind() ==
        Dashboard::DashboardHttpParseResult::Kind::Incomplete);
    static_cast<void>(rejected(parse(noDelimiterExact + "A", false), 431U));

    std::string partialAtBoundary = noDelimiterExact;
    partialAtBoundary += "\r\n\r";
    REQUIRE(
        parse(partialAtBoundary, false).kind() ==
        Dashboard::DashboardHttpParseResult::Kind::Incomplete);
    partialAtBoundary += "\n";
    static_cast<void>(rejected(parse(partialAtBoundary, false), 400U));

    std::string sixtyFour{"GET / HTTP/1.1\r\n"};
    for (std::size_t index{};
         index < Dashboard::DashboardHttpParser::MaximumHeaderCount;
         ++index) {
        sixtyFour += "X-" + std::to_string(index) + ": v\r\n";
    }
    sixtyFour += "\r\n";
    REQUIRE(
        accepted(parse(sixtyFour)).headers().size() ==
        Dashboard::DashboardHttpParser::MaximumHeaderCount);

    auto sixtyFive = sixtyFour;
    sixtyFive.insert(sixtyFive.size() - 2U, "X-over: v\r\n");
    static_cast<void>(rejected(parse(sixtyFive), 431U));
}

void rejectsMalformedAndAmbiguousHeaders()
{
    const std::vector<std::string> invalidHeaders{
        "GET / HTTP/1.1\nHost: value\r\n\r\n",
        "GET / HTTP/1.1\r\n Host: value\r\n\r\n",
        "GET / HTTP/1.1\r\n\tcontinued\r\n\r\n",
        "GET / HTTP/1.1\r\nMissing-Colon\r\n\r\n",
        "GET / HTTP/1.1\r\n: empty-name\r\n\r\n",
        "GET / HTTP/1.1\r\nBad@Name: value\r\n\r\n",
        "GET / HTTP/1.1\r\nHost: one\r\nhOsT: two\r\n\r\n",
        "GET / HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n",
        "GET / HTTP/1.1\r\nContent-Length: 0\r\ncontent-length: 0\r\n\r\n",
        std::string{"GET / HTTP/1.1\r\nX: a"} + static_cast<char>(0x01) +
            "b\r\n\r\n",
        std::string{"GET / HTTP/1.1\r\nX: a"} + static_cast<char>(0x7f) +
            "b\r\n\r\n",
        std::string{"GET / HTTP/1.1\r\nX: "} + "\xC0\xAF" +
            "\r\n\r\n",
        std::string{"GET / HTTP/1.1\r\nX: "} + "\xED\xA0\x80" +
            "\r\n\r\n",
        std::string{"GET / HTTP/1.1\r\nX: "} + "\xF4\x90\x80\x80" +
            "\r\n\r\n",
        std::string{"GET / HTTP/1.1\r\nX: "} + "\xE2\x82" +
            "\r\n\r\n"};

    for (const auto& wire : invalidHeaders) {
        static_cast<void>(rejected(parse(wire), 400U));
    }
}

void enforcesContentLengthAndSingleRequestFraming()
{
    const std::vector<std::string> malformedLengths{
        "",
        "+1",
        "-1",
        " 1 0 ",
        "1,1",
        "1x",
        "184467440737095516160"};
    for (const auto& length : malformedLengths) {
        const auto result = parse(
            "POST / HTTP/1.1\r\nContent-Length: " + length +
            "\r\n\r\n");
        const auto expected = length == "184467440737095516160"
            ? static_cast<std::uint16_t>(413U)
            : static_cast<std::uint16_t>(400U);
        static_cast<void>(rejected(result, expected));
    }

    const auto overLimit =
        std::to_string(Dashboard::DashboardHttpParser::MaximumBodyBytes + 1U);
    static_cast<void>(rejected(
        parse("POST / HTTP/1.1\r\nContent-Length: " + overLimit +
              "\r\n\r\n",
              false),
        413U));

    std::string maximumBody(
        Dashboard::DashboardHttpParser::MaximumBodyBytes, 'x');
    const auto maximumRequest =
        "POST / HTTP/1.1\r\nContent-Length: " +
        std::to_string(maximumBody.size()) + "\r\n\r\n" + maximumBody;
    REQUIRE(
        accepted(parse(maximumRequest)).body().size() ==
        Dashboard::DashboardHttpParser::MaximumBodyBytes);

    static_cast<void>(rejected(
        parse("GET / HTTP/1.1\r\n\r\nx", false), 400U));
    static_cast<void>(rejected(
        parse("POST / HTTP/1.1\r\nContent-Length: 2\r\n\r\nabc", false),
        400U));
    static_cast<void>(rejected(
        parse("GET /one HTTP/1.1\r\n\r\nGET /two HTTP/1.1\r\n\r\n", false),
        400U));
}

} // namespace

int main()
{
    try {
        acceptsAndCanonicalizesABoundedRequest();
        distinguishesIncompleteInputFromClosedStreams();
        enforcesStrictRequestLineAndOriginFormTargets();
        enforcesHeaderByteAndCountLimits();
        rejectsMalformedAndAmbiguousHeaders();
        enforcesContentLengthAndSingleRequestFraming();
        std::cout << "Dashboard HTTP parser tests passed (" << assertions
                  << " assertions).\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Dashboard HTTP parser tests failed: " << error.what()
                  << '\n';
        return 1;
    } catch (...) {
        std::cerr << "Dashboard HTTP parser tests failed with an unknown error.\n";
        return 1;
    }
}
