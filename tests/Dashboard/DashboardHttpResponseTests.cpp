#include "ForgeConductor/Dashboard/DashboardHttpResponse.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <iostream>
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

static_assert(noexcept(Dashboard::DashboardHttpResponseEncoder::encode(
    std::declval<const Dashboard::DashboardHttpResponse&>())));
static_assert(noexcept(Dashboard::DashboardHttpResponseEncoder::encodeHead(
    std::declval<const Dashboard::DashboardHttpHeadResponse&>())));
static_assert(noexcept(
    Dashboard::DashboardHttpResponseEncoder::encodeSseBootstrap(
        std::declval<const Dashboard::DashboardSseBootstrap&>())));
static_assert(
    Dashboard::DashboardHttpResponseEncoder::MaximumEncodedResponseBytes ==
    2'097'152U);

[[nodiscard]] std::vector<std::byte> bytes(const std::string_view value)
{
    std::vector<std::byte> result;
    result.reserve(value.size());
    std::transform(
        value.begin(),
        value.end(),
        std::back_inserter(result),
        [](const unsigned char character) {
            return static_cast<std::byte>(character);
        });
    return result;
}

[[nodiscard]] std::string text(const std::vector<std::byte>& value)
{
    std::string result(value.size(), '\0');
    std::transform(
        value.begin(),
        value.end(),
        result.begin(),
        [](const std::byte byte) {
            return static_cast<char>(std::to_integer<unsigned char>(byte));
        });
    return result;
}

[[nodiscard]] const std::vector<std::byte>& accepted(
    const Dashboard::DashboardHttpEncodingResult& result,
    const Dashboard::DashboardHttpEncodingResult::Kind kind)
{
    REQUIRE(result.hasValue());
    REQUIRE(static_cast<bool>(result));
    REQUIRE(result.kind() == kind);
    REQUIRE(result.error() == Dashboard::DashboardHttpEncodingError::None);
    REQUIRE(!result.bytes().empty());
    return result.bytes();
}

void rejected(
    const Dashboard::DashboardHttpEncodingResult& result,
    const Dashboard::DashboardHttpEncodingError error)
{
    REQUIRE(!result.hasValue());
    REQUIRE(!static_cast<bool>(result));
    REQUIRE(
        result.kind() ==
        Dashboard::DashboardHttpEncodingResult::Kind::Failure);
    REQUIRE(result.error() == error);
    REQUIRE(result.bytes().empty());
}

[[nodiscard]] Dashboard::DashboardHttpEncodingResult encode(
    const std::uint16_t status,
    const std::string_view contentType,
    const std::string_view body,
    std::vector<Dashboard::DashboardHttpHeader> headers = {})
{
    return Dashboard::DashboardHttpResponseEncoder::encode(
        Dashboard::DashboardHttpResponse{
            status,
            std::string{contentType},
            bytes(body),
            std::move(headers)});
}

[[nodiscard]] Dashboard::DashboardHttpEncodingResult encodeHead(
    const std::uint16_t status,
    const std::string_view contentType,
    const std::size_t representationLength,
    std::vector<Dashboard::DashboardHttpHeader> headers = {})
{
    return Dashboard::DashboardHttpResponseEncoder::encodeHead(
        Dashboard::DashboardHttpHeadResponse{
            status,
            std::string{contentType},
            representationLength,
            std::move(headers)});
}

void emitsTheExactDeterministicCompleteResponse()
{
    const auto result = encode(
        401U,
        "application/json; charset=utf-8",
        "{\"ok\":false}",
        {{"www-authenticate", "Bearer"}});
    const auto& encoded = accepted(
        result,
        Dashboard::DashboardHttpEncodingResult::Kind::CompleteResponse);

    const std::string expected =
        "HTTP/1.1 401 Unauthorized\r\n"
        "Content-Type: application/json; charset=utf-8\r\n"
        "Content-Length: 12\r\n"
        "Connection: close\r\n"
        "Cache-Control: no-store\r\n"
        "X-Content-Type-Options: nosniff\r\n"
        "Referrer-Policy: no-referrer\r\n"
        "Cross-Origin-Resource-Policy: same-origin\r\n"
        "Content-Security-Policy: default-src 'self'; connect-src 'self'; "
        "img-src 'self' data:; style-src 'self' 'unsafe-inline'; "
        "script-src 'self' 'unsafe-inline'\r\n"
        "WWW-Authenticate: Bearer\r\n"
        "\r\n"
        "{\"ok\":false}";
    REQUIRE(encoded == bytes(expected));
    REQUIRE(text(encoded).find("Access-Control-") == std::string::npos);
}

void usesClosedStatusReasonMappings()
{
    const std::array statuses{
        std::pair<std::uint16_t, std::string_view>{
            static_cast<std::uint16_t>(200U), "OK"},
        std::pair<std::uint16_t, std::string_view>{
            static_cast<std::uint16_t>(201U), "Created"},
        std::pair<std::uint16_t, std::string_view>{
            static_cast<std::uint16_t>(202U), "Accepted"},
        std::pair<std::uint16_t, std::string_view>{
            static_cast<std::uint16_t>(204U), "No Content"},
        std::pair<std::uint16_t, std::string_view>{
            static_cast<std::uint16_t>(400U), "Bad Request"},
        std::pair<std::uint16_t, std::string_view>{
            static_cast<std::uint16_t>(401U), "Unauthorized"},
        std::pair<std::uint16_t, std::string_view>{
            static_cast<std::uint16_t>(403U), "Forbidden"},
        std::pair<std::uint16_t, std::string_view>{
            static_cast<std::uint16_t>(404U), "Not Found"},
        std::pair<std::uint16_t, std::string_view>{
            static_cast<std::uint16_t>(405U), "Method Not Allowed"},
        std::pair<std::uint16_t, std::string_view>{
            static_cast<std::uint16_t>(409U), "Conflict"},
        std::pair<std::uint16_t, std::string_view>{
            static_cast<std::uint16_t>(413U), "Content Too Large"},
        std::pair<std::uint16_t, std::string_view>{
            static_cast<std::uint16_t>(414U), "URI Too Long"},
        std::pair<std::uint16_t, std::string_view>{
            static_cast<std::uint16_t>(415U), "Unsupported Media Type"},
        std::pair<std::uint16_t, std::string_view>{
            static_cast<std::uint16_t>(422U), "Unprocessable Content"},
        std::pair<std::uint16_t, std::string_view>{
            static_cast<std::uint16_t>(429U), "Too Many Requests"},
        std::pair<std::uint16_t, std::string_view>{
            static_cast<std::uint16_t>(431U),
            "Request Header Fields Too Large"},
        std::pair<std::uint16_t, std::string_view>{
            static_cast<std::uint16_t>(500U), "Internal Server Error"},
        std::pair<std::uint16_t, std::string_view>{
            static_cast<std::uint16_t>(501U), "Not Implemented"},
        std::pair<std::uint16_t, std::string_view>{
            static_cast<std::uint16_t>(503U), "Service Unavailable"},
    };

    for (const auto& [status, reason] : statuses) {
        const auto result = encode(
            status,
            "text/plain; charset=utf-8",
            status == 204U ? std::string_view{} : std::string_view{"x"});
        const auto wire = text(accepted(
            result,
            Dashboard::DashboardHttpEncodingResult::Kind::CompleteResponse));
        const auto statusLine =
            "HTTP/1.1 " + std::to_string(status) + " " +
            std::string{reason} + "\r\n";
        REQUIRE(wire.starts_with(statusLine));
        if (status == 204U) {
            REQUIRE(wire.find("Content-Length:") == std::string::npos);
            REQUIRE(wire.ends_with("\r\n\r\n"));
        }
    }

    rejected(
        encode(418U, "text/plain", "teapot"),
        Dashboard::DashboardHttpEncodingError::UnsupportedStatus);
    rejected(
        encode(204U, "text/plain", "x"),
        Dashboard::DashboardHttpEncodingError::BodyNotAllowed);
}

void rejectsHeaderInjectionAmbiguityAndUnvettedFields()
{
    const auto withHeader = [](std::string name, std::string value) {
        return encode(
            200U,
            "text/plain",
            "ok",
            {{std::move(name), std::move(value)}});
    };

    rejected(
        withHeader("Bad Name", "value"),
        Dashboard::DashboardHttpEncodingError::InvalidHeaderName);
    rejected(
        withHeader("Bad\rName", "value"),
        Dashboard::DashboardHttpEncodingError::InvalidHeaderName);
    rejected(
        withHeader(std::string(65U, 'a'), "value"),
        Dashboard::DashboardHttpEncodingError::InvalidHeaderName);
    rejected(
        withHeader("Retry-After", ""),
        Dashboard::DashboardHttpEncodingError::InvalidHeaderValue);
    rejected(
        withHeader("Retry-After", " 1"),
        Dashboard::DashboardHttpEncodingError::InvalidHeaderValue);
    rejected(
        withHeader("Retry-After", "1 "),
        Dashboard::DashboardHttpEncodingError::InvalidHeaderValue);
    rejected(
        withHeader("Retry-After", "1\r\nX-Evil: yes"),
        Dashboard::DashboardHttpEncodingError::InvalidHeaderValue);
    rejected(
        withHeader("Retry-After", std::string{"1\0evil", 6U}),
        Dashboard::DashboardHttpEncodingError::InvalidHeaderValue);
    rejected(
        withHeader("Retry-After", "1\t2"),
        Dashboard::DashboardHttpEncodingError::InvalidHeaderValue);
    rejected(
        withHeader("Retry-After", std::string(2'049U, '1')),
        Dashboard::DashboardHttpEncodingError::InvalidHeaderValue);
    rejected(
        withHeader("X-Arbitrary", "value"),
        Dashboard::DashboardHttpEncodingError::UnsupportedExtraHeader);
    rejected(
        withHeader("Content-Length", "0"),
        Dashboard::DashboardHttpEncodingError::UnsupportedExtraHeader);
    rejected(
        withHeader("Access-Control-Allow-Origin", "*"),
        Dashboard::DashboardHttpEncodingError::UnsupportedExtraHeader);

    rejected(
        encode(
            401U,
            "text/plain",
            "no",
            {{"WWW-Authenticate", "Bearer"},
             {"www-authenticate", "Basic realm=forge"}}),
        Dashboard::DashboardHttpEncodingError::DuplicateHeader);

    std::vector<Dashboard::DashboardHttpHeader> tooMany;
    for (std::size_t index{};
         index <= Dashboard::DashboardHttpResponseEncoder::MaximumExtraHeaderCount;
         ++index) {
        tooMany.push_back({"Allow", "GET"});
    }
    rejected(
        encode(200U, "text/plain", "ok", std::move(tooMany)),
        Dashboard::DashboardHttpEncodingError::TooManyExtraHeaders);
}

void rejectsInvalidContentTypes()
{
    const std::vector<std::string> invalid{
        {},
        " text/plain",
        "text/plain ",
        "text/\tplain",
        "text/plain\r\nX-Evil: yes",
        std::string{"text/plain\0evil", 15U},
        std::string{"text/"} + static_cast<char>(0x80),
        std::string(257U, 'a'),
    };
    for (const auto& contentType : invalid) {
        rejected(
            encode(200U, contentType, "ok"),
            Dashboard::DashboardHttpEncodingError::InvalidContentType);
    }
}

[[nodiscard]] std::size_t decimalDigits(std::size_t value) noexcept
{
    std::size_t digits{1U};
    while (value >= 10U) {
        value /= 10U;
        ++digits;
    }
    return digits;
}

void enforcesTheCompleteWireBoundaryExactly()
{
    const auto baseline = encode(200U, "text/plain", "");
    const auto baselineBytes = accepted(
        baseline,
        Dashboard::DashboardHttpEncodingResult::Kind::CompleteResponse)
                                   .size();
    REQUIRE(baselineBytes > 1U);

    std::size_t bodyBytes =
        Dashboard::DashboardHttpResponseEncoder::MaximumEncodedResponseBytes -
        baselineBytes;
    for (;;) {
        const auto adjustedHeader =
            baselineBytes - 1U + decimalDigits(bodyBytes);
        const auto adjustedBody =
            Dashboard::DashboardHttpResponseEncoder::
                MaximumEncodedResponseBytes -
            adjustedHeader;
        if (adjustedBody == bodyBytes) {
            break;
        }
        bodyBytes = adjustedBody;
    }

    std::vector<std::byte> exactBody(bodyBytes, std::byte{0x5a});
    const auto exact = Dashboard::DashboardHttpResponseEncoder::encode(
        Dashboard::DashboardHttpResponse{
            200U, "text/plain", std::move(exactBody)});
    REQUIRE(accepted(
                exact,
                Dashboard::DashboardHttpEncodingResult::Kind::CompleteResponse)
                .size() ==
            Dashboard::DashboardHttpResponseEncoder::
                MaximumEncodedResponseBytes);

    std::vector<std::byte> overBody(bodyBytes + 1U, std::byte{0x5a});
    rejected(
        Dashboard::DashboardHttpResponseEncoder::encode(
            Dashboard::DashboardHttpResponse{
                200U, "text/plain", std::move(overBody)}),
        Dashboard::DashboardHttpEncodingError::ResponseTooLarge);
}

[[nodiscard]] const std::array<std::string_view, 8U>& extraHeaderNames()
{
    static constexpr std::array Names{
        std::string_view{"Allow"},
        std::string_view{"Content-Disposition"},
        std::string_view{"Content-Language"},
        std::string_view{"ETag"},
        std::string_view{"Last-Modified"},
        std::string_view{"Location"},
        std::string_view{"Retry-After"},
        std::string_view{"WWW-Authenticate"},
    };
    return Names;
}

[[nodiscard]] std::size_t extraLineBytes(
    const std::string_view name,
    const std::size_t valueBytes) noexcept
{
    return name.size() + 2U + valueBytes + 2U;
}

template <typename EncodeBaseline, typename EncodeWithHeaders>
void verifiesExactHeaderBoundary(
    EncodeBaseline&& encodeBaseline,
    EncodeWithHeaders&& encodeWithHeaders,
    const Dashboard::DashboardHttpEncodingResult::Kind expectedKind)
{
    const auto baseline = encodeBaseline();
    const auto baselineBytes = accepted(baseline, expectedKind).size();
    REQUIRE(
        baselineBytes <
        Dashboard::DashboardHttpResponseEncoder::MaximumEncodedHeaderBytes);

    std::vector<Dashboard::DashboardHttpHeader> headers;
    headers.reserve(extraHeaderNames().size());
    std::size_t used = baselineBytes;
    for (std::size_t index{}; index + 1U < extraHeaderNames().size(); ++index) {
        headers.push_back({
            std::string{extraHeaderNames()[index]},
            std::string(
                Dashboard::DashboardHttpResponseEncoder::
                    MaximumExtraHeaderValueBytes,
                'a')});
        used += extraLineBytes(
            extraHeaderNames()[index],
            Dashboard::DashboardHttpResponseEncoder::
                MaximumExtraHeaderValueBytes);
    }

    const auto lastName = extraHeaderNames().back();
    REQUIRE(
        used + extraLineBytes(lastName, 1U) <=
        Dashboard::DashboardHttpResponseEncoder::MaximumEncodedHeaderBytes);
    const auto lastValueBytes =
        Dashboard::DashboardHttpResponseEncoder::MaximumEncodedHeaderBytes -
        used - extraLineBytes(lastName, 0U);
    REQUIRE(lastValueBytes >= 1U);
    REQUIRE(
        lastValueBytes <=
        Dashboard::DashboardHttpResponseEncoder::MaximumExtraHeaderValueBytes);
    headers.push_back(
        {std::string{lastName}, std::string(lastValueBytes, 'b')});

    const auto exact = encodeWithHeaders(headers);
    REQUIRE(accepted(exact, expectedKind).size() ==
            Dashboard::DashboardHttpResponseEncoder::MaximumEncodedHeaderBytes);

    headers.back().value.push_back('b');
    rejected(
        encodeWithHeaders(headers),
        Dashboard::DashboardHttpEncodingError::HeaderTooLarge);
}

void enforcesIndependentCompleteAndSseHeaderBoundaries()
{
    verifiesExactHeaderBoundary(
        [] { return encode(200U, "text/plain", ""); },
        [](const std::vector<Dashboard::DashboardHttpHeader>& headers) {
            return encode(200U, "text/plain", "", headers);
        },
        Dashboard::DashboardHttpEncodingResult::Kind::CompleteResponse);

    verifiesExactHeaderBoundary(
        [] { return encodeHead(200U, "text/plain", 0U); },
        [](const std::vector<Dashboard::DashboardHttpHeader>& headers) {
            return encodeHead(200U, "text/plain", 0U, headers);
        },
        Dashboard::DashboardHttpEncodingResult::Kind::HeadResponseHead);

    verifiesExactHeaderBoundary(
        [] {
            return Dashboard::DashboardHttpResponseEncoder::encodeSseBootstrap();
        },
        [](const std::vector<Dashboard::DashboardHttpHeader>& headers) {
            return Dashboard::DashboardHttpResponseEncoder::encodeSseBootstrap(
                Dashboard::DashboardSseBootstrap{headers});
        },
        Dashboard::DashboardHttpEncodingResult::Kind::SseBootstrapHead);
}

void emitsASeparateHeadResponseWithoutRepresentationBytes()
{
    const auto result = encodeHead(
        405U,
        "application/json; charset=utf-8",
        37U,
        {{"allow", "GET"}});
    const auto& encoded = accepted(
        result,
        Dashboard::DashboardHttpEncodingResult::Kind::HeadResponseHead);

    const auto wire = text(encoded);
    REQUIRE(wire.starts_with("HTTP/1.1 405 Method Not Allowed\r\n"));
    REQUIRE(wire.find(
                "Content-Type: application/json; charset=utf-8\r\n") !=
            std::string::npos);
    REQUIRE(wire.find("Content-Length: 37\r\n") != std::string::npos);
    REQUIRE(wire.find("Allow: GET\r\n") != std::string::npos);
    REQUIRE(wire.ends_with("\r\n\r\n"));
    REQUIRE(wire.find('{') == std::string::npos);

    const auto noContent = encodeHead(204U, "text/plain", 0U);
    const auto noContentWire = text(accepted(
        noContent,
        Dashboard::DashboardHttpEncodingResult::Kind::HeadResponseHead));
    REQUIRE(noContentWire.find("Content-Length:") == std::string::npos);
    REQUIRE(noContentWire.ends_with("\r\n\r\n"));

    rejected(
        encodeHead(204U, "text/plain", 1U),
        Dashboard::DashboardHttpEncodingError::BodyNotAllowed);
    rejected(
        encodeHead(
            200U,
            "text/plain",
            Dashboard::DashboardHttpResponseEncoder::
                    MaximumEncodedResponseBytes +
                1U),
        Dashboard::DashboardHttpEncodingError::ResponseTooLarge);
}

void preservesBinaryAndUtf8BodyBytes()
{
    std::vector<std::byte> body{
        std::byte{0xe2},
        std::byte{0x98},
        std::byte{0x83},
        std::byte{0x00},
        std::byte{0xff},
    };
    const auto expectedBody = body;
    const auto result = Dashboard::DashboardHttpResponseEncoder::encode(
        Dashboard::DashboardHttpResponse{
            200U, "application/octet-stream", std::move(body)});
    const auto& wire = accepted(
        result,
        Dashboard::DashboardHttpEncodingResult::Kind::CompleteResponse);
    const auto wireText = text(wire);
    const auto delimiter = wireText.find("\r\n\r\n");
    REQUIRE(delimiter != std::string::npos);
    REQUIRE(wireText.find("Content-Length: 5\r\n") != std::string::npos);
    const auto bodyOffset = delimiter + 4U;
    REQUIRE(wire.size() - bodyOffset == expectedBody.size());
    REQUIRE(std::equal(
        expectedBody.begin(),
        expectedBody.end(),
        wire.begin() + static_cast<std::ptrdiff_t>(bodyOffset)));
}

void emitsASeparateBoundedSseBootstrapHead()
{
    const auto result =
        Dashboard::DashboardHttpResponseEncoder::encodeSseBootstrap();
    const auto& encoded = accepted(
        result,
        Dashboard::DashboardHttpEncodingResult::Kind::SseBootstrapHead);
    const std::string expected =
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: text/event-stream; charset=utf-8\r\n"
        "Cache-Control: no-cache, no-transform\r\n"
        "Connection: keep-alive\r\n"
        "X-Content-Type-Options: nosniff\r\n"
        "Referrer-Policy: no-referrer\r\n"
        "Cross-Origin-Resource-Policy: same-origin\r\n"
        "Content-Security-Policy: default-src 'self'; connect-src 'self'; "
        "img-src 'self' data:; style-src 'self' 'unsafe-inline'; "
        "script-src 'self' 'unsafe-inline'\r\n"
        "X-Accel-Buffering: no\r\n"
        "\r\n";
    REQUIRE(encoded == bytes(expected));
    const auto wire = text(encoded);
    REQUIRE(wire.find("Content-Length:") == std::string::npos);
    REQUIRE(wire.find("Connection: close") == std::string::npos);
    REQUIRE(wire.find("Cache-Control: no-store") == std::string::npos);
    REQUIRE(wire.find("Access-Control-") == std::string::npos);

    rejected(
        Dashboard::DashboardHttpResponseEncoder::encodeSseBootstrap(
            Dashboard::DashboardSseBootstrap{
                {{"Retry-After", "1\r\nAccess-Control-Allow-Origin: *"}}}),
        Dashboard::DashboardHttpEncodingError::InvalidHeaderValue);
}

} // namespace

int main()
{
    try {
        emitsTheExactDeterministicCompleteResponse();
        usesClosedStatusReasonMappings();
        rejectsHeaderInjectionAmbiguityAndUnvettedFields();
        rejectsInvalidContentTypes();
        enforcesTheCompleteWireBoundaryExactly();
        enforcesIndependentCompleteAndSseHeaderBoundaries();
        emitsASeparateHeadResponseWithoutRepresentationBytes();
        preservesBinaryAndUtf8BodyBytes();
        emitsASeparateBoundedSseBootstrapHead();
        std::cout << "Dashboard HTTP response tests passed (" << assertions
                  << " assertions).\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Dashboard HTTP response tests failed: " << error.what()
                  << '\n';
        return 1;
    } catch (...) {
        std::cerr
            << "Dashboard HTTP response tests failed with an unknown error.\n";
        return 1;
    }
}
