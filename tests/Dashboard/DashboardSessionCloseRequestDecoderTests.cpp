#include "ForgeConductor/Dashboard/DashboardSessionCloseRequestDecoder.h"

#include "ForgeConductor/Domain/AgentModels.h"
#include "ForgeConductor/Domain/Error.h"

#include <algorithm>
#include <cstddef>
#include <iostream>
#include <iterator>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace {

namespace Dashboard = ForgeConductor::Dashboard;
namespace Domain = ForgeConductor::Domain;

constexpr std::string_view SessionId{
    "12345678-1234-4234-8234-123456789abc"};

std::size_t assertions{};

[[noreturn]] void fail(
    const std::string_view expression,
    const std::size_t line)
{
    throw std::runtime_error{
        "requirement failed at line " + std::to_string(line) + ": " +
        std::string{expression}};
}

void require(
    const bool condition,
    const std::string_view expression,
    const std::size_t line)
{
    ++assertions;
    if (!condition) fail(expression, line);
}

#define REQUIRE(condition) \
    require(static_cast<bool>(condition), #condition, __LINE__)

[[nodiscard]] std::vector<std::byte> bytes(const std::string_view value)
{
    std::vector<std::byte> result;
    result.reserve(value.size());
    std::transform(
        value.begin(),
        value.end(),
        std::back_inserter(result),
        [](const char value) {
            return static_cast<std::byte>(
                static_cast<unsigned char>(value));
        });
    return result;
}

[[nodiscard]] Domain::Result<Dashboard::DashboardSessionCloseRequest> decode(
    const std::string_view value) noexcept
{
    const auto body = bytes(value);
    return Dashboard::DashboardSessionCloseRequestDecoder::decode(body);
}

[[nodiscard]] Dashboard::DashboardSessionCloseRequest take(
    Domain::Result<Dashboard::DashboardSessionCloseRequest> result)
{
    if (!result) {
        throw std::runtime_error{
            result.error().code + ": " + result.error().message};
    }
    return std::move(result).value();
}

void requireError(
    const Domain::Result<Dashboard::DashboardSessionCloseRequest>& result,
    const std::string_view code,
    const std::string_view messageFragment = {})
{
    REQUIRE(!result);
    REQUIRE(result.error().code == code);
    REQUIRE(!result.error().message.empty());
    if (!messageFragment.empty()) {
        REQUIRE(result.error().message.find(messageFragment) !=
                std::string::npos);
    }
}

[[nodiscard]] std::string requestWithSummary(const std::string_view summary)
{
    std::string result{"{\"session_id\":\""};
    result += SessionId;
    result += "\",\"summary\":\"";
    result += summary;
    result += "\"}";
    return result;
}

void exposesImmutableBoundedContract()
{
    static_assert(std::is_final_v<Dashboard::DashboardSessionCloseRequest>);
    static_assert(
        std::is_final_v<Dashboard::DashboardSessionCloseRequestDecoder>);
    static_assert(
        Dashboard::DashboardSessionCloseRequestDecoder::MaximumRequestBytes ==
        65'536U);
    static_assert(
        Dashboard::DashboardSessionCloseRequestDecoder::MaximumJsonNesting ==
        16U);
    static_assert(
        Dashboard::DashboardSessionCloseRequest::DefaultSummary ==
        "Closed from dashboard");
    static_assert(!std::is_default_constructible_v<
                  Dashboard::DashboardSessionCloseRequest>);
    static_assert(!std::is_copy_assignable_v<
                  Dashboard::DashboardSessionCloseRequest>);
    static_assert(!std::is_move_assignable_v<
                  Dashboard::DashboardSessionCloseRequest>);
    static_assert(noexcept(
        std::declval<const Dashboard::DashboardSessionCloseRequest&>()
            .sessionId()));
    static_assert(noexcept(
        std::declval<const Dashboard::DashboardSessionCloseRequest&>()
            .summary()));
    static_assert(noexcept(
        Dashboard::DashboardSessionCloseRequestDecoder::decode(
            std::declval<std::span<const std::byte>>(),
            std::declval<std::size_t>())));

    const auto request = take(decode(
        R"json({"session_id":"12345678-1234-4234-8234-123456789ABC"})json"));
    REQUIRE(request.sessionId().value() == SessionId);
    REQUIRE(request.summary() == "Closed from dashboard");
}

void acceptsExplicitBoundedSummaries()
{
    const auto explicitSummary = take(decode(
        R"json({"session_id":"12345678-1234-4234-8234-123456789abc","summary":"operator closed"})json"));
    REQUIRE(explicitSummary.sessionId().value() == SessionId);
    REQUIRE(explicitSummary.summary() == "operator closed");

    const auto emptySummary = take(decode(
        R"json({"session_id":"12345678-1234-4234-8234-123456789abc","summary":""})json"));
    REQUIRE(emptySummary.summary().empty());

    const auto escaped = take(decode(
        R"json({"session_id":"12345678-1234-4234-8234-123456789abc","summary":"line one\nline two \ud83d\ude00"})json"));
    REQUIRE(escaped.summary() == "line one\nline two \xf0\x9f\x98\x80");
}

void preservesRequiredSessionIdCompatibilityMessage()
{
    const std::vector<std::string> inputs{
        "{}",
        R"json({"summary":"close"})json",
        R"json({"session_id":null})json",
        R"json({"session_id":false})json",
        R"json({"session_id":1})json",
        R"json({"session_id":{}})json",
        R"json({"session_id":[]})json"};
    for (const auto& input : inputs) {
        requireError(
            decode(input),
            Domain::ErrorCodes::InvalidRequest,
            "session_id required");
    }
}

void rejectsInvalidIdentifiersTypesAndUnknownFields()
{
    const std::vector<std::string> invalidIds{
        "",
        "12345678-1234-4234-8234-123456789ab",
        "123456781234-4234-8234-123456789abc",
        "12345678-1234-4234-8234-123456789abz",
        " 12345678-1234-4234-8234-123456789abc"};
    for (const auto& value : invalidIds) {
        std::string input{"{\"session_id\":\""};
        input += value;
        input += "\"}";
        requireError(
            decode(input),
            Domain::ErrorCodes::InvalidRequest,
            "canonical UUID");
    }

    requireError(
        decode("null"), Domain::ErrorCodes::InvalidRequest, "JSON object");
    requireError(
        decode("[]"), Domain::ErrorCodes::InvalidRequest, "JSON object");
    requireError(
        decode(R"json({"session_id":"12345678-1234-4234-8234-123456789abc","summary":7})json"),
        Domain::ErrorCodes::InvalidRequest,
        "summary");
    requireError(
        decode(R"json({"session_id":"12345678-1234-4234-8234-123456789abc","unexpected":true})json"),
        Domain::ErrorCodes::InvalidRequest,
        "unknown field");
}

void rejectsDuplicateKeysBeforeSchemaMapping()
{
    const std::vector<std::string> inputs{
        R"json({"session_id":"12345678-1234-4234-8234-123456789abc","session_id":"aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa"})json",
        R"json({"session_id":"12345678-1234-4234-8234-123456789abc","summary":"first","summary":"second"})json",
        R"json({"session_id":"12345678-1234-4234-8234-123456789abc","sess\u0069on_id":"aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa"})json",
        R"json({"session_id":"12345678-1234-4234-8234-123456789abc","unexpected":{"key":1,"key":2}})json"};
    for (const auto& input : inputs) {
        requireError(
            decode(input), Domain::ErrorCodes::MalformedMessage, "duplicate");
    }
}

void rejectsMalformedNulInvalidUtf8AndExcessiveDepth()
{
    requireError(decode(""), Domain::ErrorCodes::MalformedMessage);
    requireError(decode("{"), Domain::ErrorCodes::MalformedMessage);
    requireError(
        decode("{} trailing"), Domain::ErrorCodes::MalformedMessage);
    requireError(
        decode(R"json({"session_id":"12345678-1234-4234-8234-123456789abc","summary":"bad\u0000text"})json"),
        Domain::ErrorCodes::MalformedMessage);
    requireError(
        decode(R"json({"session_\u0000id":"12345678-1234-4234-8234-123456789abc"})json"),
        Domain::ErrorCodes::MalformedMessage);

    auto rawNul = bytes(
        R"json({"session_id":"12345678-1234-4234-8234-123456789abc"})json");
    rawNul.insert(rawNul.begin() + 2, std::byte{0});
    requireError(
        Dashboard::DashboardSessionCloseRequestDecoder::decode(rawNul),
        Domain::ErrorCodes::MalformedMessage);

    auto invalidUtf8 = bytes(requestWithSummary("bad"));
    invalidUtf8.insert(invalidUtf8.end() - 2, std::byte{0xff});
    requireError(
        Dashboard::DashboardSessionCloseRequestDecoder::decode(invalidUtf8),
        Domain::ErrorCodes::MalformedMessage);

    std::string deep{"{\"unexpected\":"};
    deep.append(
        Dashboard::DashboardSessionCloseRequestDecoder::MaximumJsonNesting,
        '[');
    deep += '0';
    deep.append(
        Dashboard::DashboardSessionCloseRequestDecoder::MaximumJsonNesting,
        ']');
    deep += '}';
    requireError(
        decode(deep), Domain::ErrorCodes::LimitExceeded, "nesting");
}

void enforcesRequestAndUnicodeScalarCeilings()
{
    std::vector<std::byte> oversized(
        Dashboard::DashboardSessionCloseRequestDecoder::MaximumRequestBytes +
            1U,
        static_cast<std::byte>('x'));
    requireError(
        Dashboard::DashboardSessionCloseRequestDecoder::decode(oversized),
        Domain::ErrorCodes::PayloadTooLarge);

    const auto minimal = bytes(
        R"json({"session_id":"12345678-1234-4234-8234-123456789abc"})json");
    REQUIRE(Dashboard::DashboardSessionCloseRequestDecoder::decode(
                minimal, minimal.size()));
    requireError(
        Dashboard::DashboardSessionCloseRequestDecoder::decode(
            minimal, minimal.size() - 1U),
        Domain::ErrorCodes::PayloadTooLarge);
    requireError(
        Dashboard::DashboardSessionCloseRequestDecoder::decode(minimal, 0U),
        Domain::ErrorCodes::InvalidRequest);
    requireError(
        Dashboard::DashboardSessionCloseRequestDecoder::decode(
            minimal,
            Dashboard::DashboardSessionCloseRequestDecoder::
                    MaximumRequestBytes +
                1U),
        Domain::ErrorCodes::InvalidRequest);

    const std::string maximumAscii(
        Domain::AgentSessionLimits::MaximumSummaryUnits, 'a');
    REQUIRE(take(decode(requestWithSummary(maximumAscii))).summary() ==
            maximumAscii);
    const std::string excessAscii(
        Domain::AgentSessionLimits::MaximumSummaryUnits + 1U, 'a');
    requireError(
        decode(requestWithSummary(excessAscii)),
        Domain::ErrorCodes::PayloadTooLarge,
        "summary");

    constexpr std::string_view Emoji{"\xf0\x9f\x98\x80"};
    std::string maximumUnicode;
    maximumUnicode.reserve(
        Domain::AgentSessionLimits::MaximumSummaryUnits * Emoji.size());
    for (std::size_t index{};
         index < Domain::AgentSessionLimits::MaximumSummaryUnits;
         ++index) {
        maximumUnicode += Emoji;
    }
    REQUIRE(take(decode(requestWithSummary(maximumUnicode))).summary() ==
            maximumUnicode);
    maximumUnicode += Emoji;
    requireError(
        decode(requestWithSummary(maximumUnicode)),
        Domain::ErrorCodes::PayloadTooLarge,
        "summary");
}

void remainsStableAcrossRepeatedDecoding()
{
    const auto payload = requestWithSummary("bounded repeat");
    for (std::size_t iteration{}; iteration < 250U; ++iteration) {
        const auto decoded = take(decode(payload));
        REQUIRE(decoded.sessionId().value() == SessionId);
        REQUIRE(decoded.summary() == "bounded repeat");
    }
}

} // namespace

int main()
{
    try {
        exposesImmutableBoundedContract();
        acceptsExplicitBoundedSummaries();
        preservesRequiredSessionIdCompatibilityMessage();
        rejectsInvalidIdentifiersTypesAndUnknownFields();
        rejectsDuplicateKeysBeforeSchemaMapping();
        rejectsMalformedNulInvalidUtf8AndExcessiveDepth();
        enforcesRequestAndUnicodeScalarCeilings();
        remainsStableAcrossRepeatedDecoding();
        std::cout << "Dashboard session-close request decoder tests passed ("
                  << assertions << " assertions).\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Dashboard session-close request decoder tests failed: "
                  << error.what() << '\n';
        return 1;
    } catch (...) {
        std::cerr << "Dashboard session-close request decoder tests failed "
                     "with an unknown error.\n";
        return 1;
    }
}
