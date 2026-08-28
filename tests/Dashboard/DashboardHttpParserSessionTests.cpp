#include "ForgeConductor/Dashboard/DashboardHttpParserSession.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <iterator>
#include <span>
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

[[nodiscard]] Dashboard::DashboardHttpParseResult directParse(
    const std::vector<std::byte>& wire,
    const bool streamComplete = true)
{
    return Dashboard::DashboardHttpParser{}.parse(wire, streamComplete);
}

void requireRejectionEquals(
    const Dashboard::DashboardHttpRejection& actual,
    const Dashboard::DashboardHttpRejection& expected)
{
    REQUIRE(actual.status == expected.status);
    REQUIRE(actual.code == expected.code);
    REQUIRE(actual.message == expected.message);
    REQUIRE(actual.responseHeaders == expected.responseHeaders);
}

void appendFragments(
    Dashboard::DashboardHttpParserSession& session,
    const std::vector<std::byte>& wire,
    const std::span<const std::size_t> pattern)
{
    REQUIRE(!pattern.empty());
    std::size_t offset{};
    std::size_t patternIndex{};
    while (offset < wire.size() &&
           session.state() !=
               Dashboard::DashboardHttpParserSessionState::Rejected) {
        const auto requested = pattern[patternIndex % pattern.size()];
        REQUIRE(requested != 0U);
        const auto length = (std::min)(requested, wire.size() - offset);
        static_cast<void>(session.append(
            std::span<const std::byte>{wire}.subspan(offset, length)));
        offset += length;
        ++patternIndex;
    }
}

void requiresHeaderValidationBeforePublishingFraming()
{
    const std::string header =
        "pOsT /api/status?name=forge%20conductor HTTP/1.1\r\n"
        "Host:\t127.0.0.1:7788 \t\r\n"
        "X-Display: Forge \xE2\x98\x83\r\n"
        "Content-Length: 4\r\n"
        "\r\n";
    std::string wire = header;
    wire.append(std::string_view{"{}\xFF\0", 4U});
    const auto input = bytes(wire);

    Dashboard::DashboardHttpParserSession session;
    REQUIRE(
        session.state() ==
        Dashboard::DashboardHttpParserSessionState::ReceivingHeader);
    REQUIRE(!session.expectedTotalBytes().has_value());
    REQUIRE(!session.remainingBytes().has_value());

    for (std::size_t index{}; index < input.size(); ++index) {
        const auto update = session.append(
            std::span<const std::byte>{input}.subspan(index, 1U));
        REQUIRE(!update.hasError());
        REQUIRE(session.bufferedBytes() == index + 1U);
        if (index + 1U < header.size()) {
            REQUIRE(
                session.state() ==
                Dashboard::DashboardHttpParserSessionState::ReceivingHeader);
            REQUIRE(!session.expectedTotalBytes().has_value());
            REQUIRE(!session.remainingBytes().has_value());
        } else if (index + 1U < input.size()) {
            REQUIRE(
                session.state() ==
                Dashboard::DashboardHttpParserSessionState::ReceivingBody);
            REQUIRE(session.expectedTotalBytes() == input.size());
            REQUIRE(session.remainingBytes() == input.size() - index - 1U);
        }
    }

    REQUIRE(
        session.state() ==
        Dashboard::DashboardHttpParserSessionState::Complete);
    REQUIRE(session.expectedTotalBytes() == input.size());
    REQUIRE(session.remainingBytes() == 0U);
    REQUIRE(session.rejection() == nullptr);

    const auto direct = directParse(input);
    REQUIRE(direct.request() != nullptr);
    auto taken = session.takeRequest();
    REQUIRE(taken.hasValue());
    REQUIRE(taken.error() == Dashboard::DashboardHttpParserSessionError::None);
    REQUIRE(taken.request() != nullptr);
    REQUIRE(*taken.request() == *direct.request());
    auto request = std::move(taken).value();
    REQUIRE(request.method() == "POST");
    REQUIRE(request.body() == bytes(std::string_view{"{}\xFF\0", 4U}));
    REQUIRE(
        session.state() == Dashboard::DashboardHttpParserSessionState::Closed);
    REQUIRE(session.bufferedBytes() == 0U);
    REQUIRE(!session.expectedTotalBytes().has_value());
    REQUIRE(!session.remainingBytes().has_value());

    const auto secondTake = session.takeRequest();
    REQUIRE(!secondTake.hasValue());
    REQUIRE(
        secondTake.error() ==
        Dashboard::DashboardHttpParserSessionError::RequestAlreadyTaken);
    const auto trailing = bytes("x");
    const auto postClose = session.append(trailing);
    REQUIRE(
        postClose.state() ==
        Dashboard::DashboardHttpParserSessionState::Closed);
    REQUIRE(
        postClose.error() ==
        Dashboard::DashboardHttpParserSessionError::TerminalState);
}

void transitionsAtHeaderAndBodyBoundaries()
{
    const std::string header =
        "POST /api/sessions/close HTTP/1.1\r\n"
        "Content-Length:\t5 \r\n"
        "\r\n";
    const auto headerBytes = bytes(header);
    const auto tail = bytes("12");
    const auto finalBody = bytes("345");

    Dashboard::DashboardHttpParserSession session;
    auto update = session.append(
        std::span<const std::byte>{headerBytes}.first(headerBytes.size() - 1U));
    REQUIRE(!update.hasError());
    REQUIRE(
        update.state() ==
        Dashboard::DashboardHttpParserSessionState::ReceivingHeader);
    REQUIRE(!session.expectedTotalBytes().has_value());

    std::vector<std::byte> transition;
    transition.push_back(headerBytes.back());
    transition.insert(transition.end(), tail.begin(), tail.end());
    update = session.append(transition);
    REQUIRE(!update.hasError());
    REQUIRE(
        update.state() ==
        Dashboard::DashboardHttpParserSessionState::ReceivingBody);
    REQUIRE(session.expectedTotalBytes() == header.size() + 5U);
    REQUIRE(session.remainingBytes() == 3U);

    update = session.append(finalBody);
    REQUIRE(!update.hasError());
    REQUIRE(
        update.state() == Dashboard::DashboardHttpParserSessionState::Complete);
    REQUIRE(session.remainingBytes() == 0U);
    REQUIRE(!session.finish().hasError());
    REQUIRE(
        session.state() ==
        Dashboard::DashboardHttpParserSessionState::Complete);

    Dashboard::DashboardHttpParserSession zeroBody;
    const auto zeroWire = bytes("GET / HTTP/1.1\r\nHost: local\r\n\r\n");
    update = zeroBody.append(zeroWire);
    REQUIRE(!update.hasError());
    REQUIRE(
        zeroBody.state() ==
        Dashboard::DashboardHttpParserSessionState::Complete);
    REQUIRE(zeroBody.expectedTotalBytes() == zeroWire.size());
    REQUIRE(zeroBody.remainingBytes() == 0U);
    REQUIRE(zeroBody.takeRequest().hasValue());
}

void rejectsIncompleteInvalidAndOversizedInput()
{
    Dashboard::DashboardHttpParserSession empty;
    auto update = empty.finish();
    REQUIRE(
        update.state() == Dashboard::DashboardHttpParserSessionState::Rejected);
    REQUIRE(
        update.error() ==
        Dashboard::DashboardHttpParserSessionError::HttpRejected);
    REQUIRE(empty.rejection() != nullptr);
    REQUIRE(empty.rejection()->status == 400U);
    REQUIRE(empty.bufferedBytes() == 0U);
    REQUIRE(!empty.expectedTotalBytes().has_value());

    Dashboard::DashboardHttpParserSession shortBody;
    const auto incomplete =
        bytes("POST / HTTP/1.1\r\nContent-Length: 4\r\n\r\n{}");
    update = shortBody.append(incomplete);
    REQUIRE(!update.hasError());
    REQUIRE(
        shortBody.state() ==
        Dashboard::DashboardHttpParserSessionState::ReceivingBody);
    REQUIRE(shortBody.remainingBytes() == 2U);
    update = shortBody.finish();
    REQUIRE(
        update.state() == Dashboard::DashboardHttpParserSessionState::Rejected);
    REQUIRE(shortBody.rejection() != nullptr);
    REQUIRE(shortBody.bufferedBytes() == 0U);
    REQUIRE(!shortBody.expectedTotalBytes().has_value());
    REQUIRE(!shortBody.remainingBytes().has_value());
    const auto directIncomplete = directParse(incomplete, true);
    REQUIRE(directIncomplete.rejection() != nullptr);
    requireRejectionEquals(
        *shortBody.rejection(), *directIncomplete.rejection());

    Dashboard::DashboardHttpParserSession invalid;
    const auto invalidWire =
        bytes("GET / HTTP/1.1\r\nHost: one\r\nhOsT: two\r\n\r\n");
    update = invalid.append(invalidWire);
    REQUIRE(
        update.state() == Dashboard::DashboardHttpParserSessionState::Rejected);
    REQUIRE(invalid.rejection() != nullptr);
    REQUIRE(invalid.rejection()->status == 400U);
    REQUIRE(invalid.bufferedBytes() == 0U);
    REQUIRE(!invalid.expectedTotalBytes().has_value());
    REQUIRE(!invalid.remainingBytes().has_value());

    Dashboard::DashboardHttpParserSession largeHeader;
    const std::vector<std::byte> headerOverflow(
        Dashboard::DashboardHttpParserSession::MaximumHeaderWireBytes,
        std::byte{static_cast<unsigned char>('A')});
    update = largeHeader.append(headerOverflow);
    REQUIRE(
        update.state() == Dashboard::DashboardHttpParserSessionState::Rejected);
    REQUIRE(largeHeader.rejection() != nullptr);
    REQUIRE(largeHeader.rejection()->status == 431U);
    REQUIRE(largeHeader.bufferedBytes() == 0U);
    REQUIRE(!largeHeader.expectedTotalBytes().has_value());

    const std::string requestLine{"GET / HTTP/1.1"};
    const auto fixedHeaderBytes =
        requestLine.size() + std::string_view{"\r\nX:"}.size();
    std::string exactHeader = requestLine + "\r\nX:";
    exactHeader.append(
        Dashboard::DashboardHttpParser::MaximumHeaderBytes - fixedHeaderBytes,
        'a');
    exactHeader += "\r\n\r\n";
    Dashboard::DashboardHttpParserSession exactHeaderSession;
    update = exactHeaderSession.append(bytes(exactHeader));
    REQUIRE(!update.hasError());
    REQUIRE(
        update.state() ==
        Dashboard::DashboardHttpParserSessionState::Complete);
    REQUIRE(
        exactHeaderSession.bufferedBytes() ==
        Dashboard::DashboardHttpParserSession::MaximumHeaderWireBytes);
    REQUIRE(exactHeaderSession.takeRequest().hasValue());

    exactHeader.insert(exactHeader.size() - 4U, 1U, 'a');
    Dashboard::DashboardHttpParserSession overHeaderSession;
    update = overHeaderSession.append(bytes(exactHeader));
    REQUIRE(
        update.state() == Dashboard::DashboardHttpParserSessionState::Rejected);
    REQUIRE(overHeaderSession.rejection() != nullptr);
    REQUIRE(overHeaderSession.rejection()->status == 431U);

    Dashboard::DashboardHttpParserSession largeBody;
    const auto bodyOverflow = bytes(
        "POST / HTTP/1.1\r\nContent-Length: " +
        std::to_string(Dashboard::DashboardHttpParser::MaximumBodyBytes + 1U) +
        "\r\n\r\n");
    update = largeBody.append(bodyOverflow);
    REQUIRE(
        update.state() == Dashboard::DashboardHttpParserSessionState::Rejected);
    REQUIRE(largeBody.rejection() != nullptr);
    REQUIRE(largeBody.rejection()->status == 413U);
    REQUIRE(!largeBody.expectedTotalBytes().has_value());

    const auto maximumBodyHeader = bytes(
        "POST / HTTP/1.1\r\nContent-Length: " +
        std::to_string(Dashboard::DashboardHttpParser::MaximumBodyBytes) +
        "\r\n\r\n");
    const std::vector<std::byte> maximumBody(
        Dashboard::DashboardHttpParser::MaximumBodyBytes,
        std::byte{static_cast<unsigned char>('x')});
    Dashboard::DashboardHttpParserSession exactMaximum;
    update = exactMaximum.append(maximumBodyHeader);
    REQUIRE(!update.hasError());
    REQUIRE(
        update.state() ==
        Dashboard::DashboardHttpParserSessionState::ReceivingBody);
    REQUIRE(
        exactMaximum.expectedTotalBytes() ==
        maximumBodyHeader.size() + maximumBody.size());
    REQUIRE(exactMaximum.remainingBytes() == maximumBody.size());
    update = exactMaximum.append(maximumBody);
    REQUIRE(!update.hasError());
    REQUIRE(
        update.state() ==
        Dashboard::DashboardHttpParserSessionState::Complete);
    auto maximumTaken = exactMaximum.takeRequest();
    REQUIRE(maximumTaken.hasValue());
    REQUIRE(maximumTaken.request() != nullptr);
    REQUIRE(
        maximumTaken.request()->body().size() ==
        Dashboard::DashboardHttpParser::MaximumBodyBytes);
}

void rejectsTrailingAndPipelinedBytes()
{
    const auto zeroTrailing = bytes("GET / HTTP/1.1\r\n\r\nx");
    Dashboard::DashboardHttpParserSession zero;
    auto update = zero.append(zeroTrailing);
    REQUIRE(
        update.state() == Dashboard::DashboardHttpParserSessionState::Rejected);
    REQUIRE(
        update.error() ==
        Dashboard::DashboardHttpParserSessionError::TrailingBytes);
    REQUIRE(zero.rejection() != nullptr);
    const auto zeroDirect = directParse(zeroTrailing, false);
    REQUIRE(zeroDirect.rejection() != nullptr);
    requireRejectionEquals(*zero.rejection(), *zeroDirect.rejection());

    const auto bodyTrailing =
        bytes("POST / HTTP/1.1\r\nContent-Length: 2\r\n\r\nabc");
    Dashboard::DashboardHttpParserSession body;
    update = body.append(bodyTrailing);
    REQUIRE(
        update.state() == Dashboard::DashboardHttpParserSessionState::Rejected);
    REQUIRE(
        update.error() ==
        Dashboard::DashboardHttpParserSessionError::TrailingBytes);
    REQUIRE(body.rejection() != nullptr);
    const auto bodyDirect = directParse(bodyTrailing, false);
    REQUIRE(bodyDirect.rejection() != nullptr);
    requireRejectionEquals(*body.rejection(), *bodyDirect.rejection());

    const auto pipelined = bytes(
        "GET /one HTTP/1.1\r\n\r\nGET /two HTTP/1.1\r\n\r\n");
    Dashboard::DashboardHttpParserSession pipeline;
    update = pipeline.append(pipelined);
    REQUIRE(
        update.state() == Dashboard::DashboardHttpParserSessionState::Rejected);
    REQUIRE(
        update.error() ==
        Dashboard::DashboardHttpParserSessionError::TrailingBytes);

    Dashboard::DashboardHttpParserSession late;
    const auto complete = bytes("GET / HTTP/1.1\r\n\r\n");
    update = late.append(complete);
    REQUIRE(
        update.state() == Dashboard::DashboardHttpParserSessionState::Complete);
    const auto latePipeline = bytes("GET /two HTTP/1.1\r\n\r\n");
    update = late.append(latePipeline);
    REQUIRE(
        update.state() == Dashboard::DashboardHttpParserSessionState::Rejected);
    REQUIRE(
        update.error() ==
        Dashboard::DashboardHttpParserSessionError::TrailingBytes);
    REQUIRE(!late.takeRequest().hasValue());
}

void keepsTerminalOperationsTypedAndStable()
{
    Dashboard::DashboardHttpParserSession receiving;
    REQUIRE(
        receiving.takeRequest().error() ==
        Dashboard::DashboardHttpParserSessionError::RequestNotReady);
    REQUIRE(
        receiving.state() ==
        Dashboard::DashboardHttpParserSessionState::ReceivingHeader);

    const auto malformed = bytes("GET / HTTP/1.0\r\n\r\n");
    auto update = receiving.append(malformed);
    REQUIRE(
        update.state() == Dashboard::DashboardHttpParserSessionState::Rejected);
    REQUIRE(receiving.rejection() != nullptr);
    const auto saved = *receiving.rejection();

    update = receiving.append({});
    REQUIRE(
        update.state() == Dashboard::DashboardHttpParserSessionState::Rejected);
    REQUIRE(
        update.error() ==
        Dashboard::DashboardHttpParserSessionError::TerminalState);
    const auto ignored = bytes("ignored");
    update = receiving.append(ignored);
    REQUIRE(
        update.error() ==
        Dashboard::DashboardHttpParserSessionError::TerminalState);
    update = receiving.finish();
    REQUIRE(
        update.error() ==
        Dashboard::DashboardHttpParserSessionError::TerminalState);
    REQUIRE(receiving.rejection() != nullptr);
    requireRejectionEquals(*receiving.rejection(), saved);
    REQUIRE(
        receiving.takeRequest().error() ==
        Dashboard::DashboardHttpParserSessionError::HttpRejected);
}

void matchesCanonicalParserAcrossFragmentationPatterns()
{
    std::vector<std::string> cases{
        "GET / HTTP/1.1\r\n\r\n",
        "m-search /api/status?x=%7E HTTP/1.1\r\nHost: local\r\n\r\n",
        "POST /api/sessions/close HTTP/1.1\r\nContent-Length: 5\r\n\r\n12345",
        "POST / HTTP/1.1\r\ncOnTeNt-LeNgTh:\t0003 \r\n\r\nabc",
        "GET / HTTP/1.0\r\n\r\n",
        "GET / HTTP/1.1\r\r\n\r\n",
        "GET http://localhost/ HTTP/1.1\r\n\r\n",
        "GET / HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n",
        "POST / HTTP/1.1\r\nContent-Length: +1\r\n\r\n",
        "GET /one HTTP/1.1\r\n\r\nGET /two HTTP/1.1\r\n\r\n",
    };
    cases.push_back(
        std::string{"GET / HTTP/1.1\r\nX: "} + "\xC0\xAF" +
        "\r\n\r\n");

    const std::vector<std::vector<std::size_t>> patterns{
        {1U},
        {2U, 3U, 5U, 7U},
        {64U},
    };

    for (const auto& wireText : cases) {
        const auto wire = bytes(wireText);
        const auto direct = directParse(wire, true);
        REQUIRE(
            direct.kind() !=
            Dashboard::DashboardHttpParseResult::Kind::Incomplete);

        for (const auto& pattern : patterns) {
            Dashboard::DashboardHttpParserSession session;
            appendFragments(session, wire, pattern);
            if (session.state() ==
                    Dashboard::DashboardHttpParserSessionState::ReceivingHeader ||
                session.state() ==
                    Dashboard::DashboardHttpParserSessionState::ReceivingBody) {
                static_cast<void>(session.finish());
            }

            if (direct.kind() ==
                Dashboard::DashboardHttpParseResult::Kind::Accepted) {
                REQUIRE(
                    session.state() ==
                    Dashboard::DashboardHttpParserSessionState::Complete);
                auto taken = session.takeRequest();
                REQUIRE(taken.hasValue());
                REQUIRE(taken.request() != nullptr);
                REQUIRE(*taken.request() == *direct.request());
            } else {
                REQUIRE(
                    session.state() ==
                    Dashboard::DashboardHttpParserSessionState::Rejected);
                REQUIRE(session.rejection() != nullptr);
                REQUIRE(direct.rejection() != nullptr);
                requireRejectionEquals(
                    *session.rejection(), *direct.rejection());
            }
        }
    }
}

} // namespace

int main()
{
    try {
        requiresHeaderValidationBeforePublishingFraming();
        transitionsAtHeaderAndBodyBoundaries();
        rejectsIncompleteInvalidAndOversizedInput();
        rejectsTrailingAndPipelinedBytes();
        keepsTerminalOperationsTypedAndStable();
        matchesCanonicalParserAcrossFragmentationPatterns();
        std::cout << "Dashboard HTTP parser session tests passed ("
                  << assertions << " assertions).\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Dashboard HTTP parser session tests failed: "
                  << error.what() << '\n';
        return 1;
    } catch (...) {
        std::cerr << "Dashboard HTTP parser session tests failed with an "
                     "unknown error.\n";
        return 1;
    }
}
