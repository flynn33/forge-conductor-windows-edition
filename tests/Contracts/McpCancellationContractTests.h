#pragma once

#include "Fakes/FoundationFakes.h"
#include "Fakes/McpTransportFake.h"
#include "ForgeConductor/Contracts/IMcpServer.h"

#include <chrono>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace ForgeConductor::Tests {

namespace McpCancellationContractTestsDetail {

namespace Domain = ForgeConductor::Domain;
namespace Fakes = ForgeConductor::Tests::Fakes;
namespace ProductContracts = ForgeConductor::Contracts;
using namespace std::chrono_literals;

using McpServerCancelSignature = void (
    ProductContracts::IMcpServer::*)(
        const Domain::OperationId&) noexcept;

static_assert(std::is_same_v<
              decltype(&ProductContracts::IMcpServer::cancel),
              McpServerCancelSignature>);

inline void require(
    const bool condition,
    const std::string_view message)
{
    if (!condition) {
        throw std::runtime_error{std::string{message}};
    }
}

template <typename T>
[[nodiscard]] T parseId(const std::string_view value)
{
    auto parsed = T::parse(value);
    if (!parsed) {
        throw std::runtime_error{parsed.error().message};
    }
    return std::move(parsed).value();
}

template <typename T>
void requireError(
    const Domain::Result<T>& result,
    const std::string_view expectedCode,
    const std::string_view message)
{
    require(!result, message);
    require(result.error().code == expectedCode, message);
}

struct Fixture final {
    Domain::OperationId operationId{
        parseId<Domain::OperationId>(
            "99999999-9999-4999-8999-999999999999")};
    Domain::CorrelationId correlationId{
        parseId<Domain::CorrelationId>("mcp-cancellation-test")};
    Domain::MonotonicTimePoint now{
        Domain::MonotonicTimePoint{} + 100ms};

    [[nodiscard]] Domain::OperationContext activeContext() const
    {
        return Domain::OperationContext{
            operationId,
            now + 1s,
            std::stop_token{},
            correlationId};
    }

    [[nodiscard]] Domain::OperationContext expiredContext() const
    {
        return Domain::OperationContext{
            operationId,
            now,
            std::stop_token{},
            correlationId};
    }

    [[nodiscard]] Domain::OperationContext cancelledContext() const
    {
        std::stop_source cancellation;
        cancellation.request_stop();
        return Domain::OperationContext{
            operationId,
            now + 1s,
            cancellation.get_token(),
            correlationId};
    }

    [[nodiscard]] Domain::ResourceBudgets budgets() const noexcept
    {
        Domain::ResourceBudgets value{};
        value.mcpInputLineBytesMaximum = 16;
        return value;
    }
};

inline void testDeadlineScheduler(const Fixture& fixture)
{
    Fakes::FakeDeadlineScheduler scheduler{fixture.now};
    const auto active = fixture.activeContext();

    require(
        scheduler.waitUntil(active).hasValue(),
        "deadline scheduler rejected an active context");
    require(
        scheduler.lastContext() &&
            scheduler.lastContext()->operationId == active.operationId &&
            scheduler.lastContext()->correlationId == active.correlationId &&
            scheduler.lastContext()->deadline == active.deadline,
        "deadline scheduler did not retain the complete operation context");

    const auto expired = scheduler.waitUntil(fixture.expiredContext());
    requireError(
        expired,
        Domain::ErrorCodes::DeadlineExceeded,
        "deadline scheduler did not reject an expired context");

    const auto cancelled = scheduler.waitUntil(fixture.cancelledContext());
    requireError(
        cancelled,
        Domain::ErrorCodes::Cancelled,
        "deadline scheduler did not reject a cancelled context");

    scheduler.shutdown();
    const auto stopped = scheduler.waitUntil(active);
    requireError(
        stopped,
        Domain::ErrorCodes::Cancelled,
        "deadline scheduler accepted work after shutdown");
}

inline void testMcpFrameValidationAndEof(const Fixture& fixture)
{
    std::vector<std::string> inbound;
    inbound.emplace_back("{}");
    inbound.emplace_back(16, 'x');
    inbound.emplace_back("{}\n");
    inbound.emplace_back(std::string{"x\0y", 3});
    inbound.emplace_back(17, 'x');
    inbound.emplace_back("");

    Fakes::McpTransportFake transport{
        std::move(inbound),
        fixture.budgets(),
        fixture.now,
        6};
    const auto active = fixture.activeContext();

    auto validInbound = transport.receive(active);
    require(
        validInbound && validInbound.value() &&
            validInbound.value()->utf8Json == "{}",
        "MCP fake did not return the valid inbound frame");

    const auto exactCapInbound = transport.receive(active);
    require(
        exactCapInbound && exactCapInbound.value() &&
            exactCapInbound.value()->utf8Json.size() ==
                fixture.budgets().mcpInputLineBytesMaximum,
        "MCP fake rejected an inbound frame at the exact byte cap");


    const auto newlineInbound = transport.receive(active);
    requireError(
        newlineInbound,
        Domain::ErrorCodes::MalformedMessage,
        "MCP fake accepted an inbound frame containing a newline");

    const auto nulInbound = transport.receive(active);
    requireError(
        nulInbound,
        Domain::ErrorCodes::MalformedMessage,
        "MCP fake accepted an inbound frame containing NUL");

    const auto oversizedInbound = transport.receive(active);
    requireError(
        oversizedInbound,
        Domain::ErrorCodes::PayloadTooLarge,
        "MCP fake accepted an oversized inbound frame");

    const auto emptyInbound = transport.receive(active);
    requireError(
        emptyInbound,
        Domain::ErrorCodes::MalformedMessage,
        "MCP fake confused an empty wire frame with clean EOF");

    const auto cleanEof = transport.receive(active);
    require(
        cleanEof && !cleanEof.value(),
        "MCP fake did not preserve the clean EOF distinction");
    require(
        transport.remainingInboundCount() == 0,
        "MCP fake did not consume its bounded inbound queue");

    require(
        transport.send(Domain::McpFrame{"{}"}, active).hasValue(),
        "MCP fake rejected a valid outbound frame");
    require(
        transport.send(Domain::McpFrame{std::string(16, 'x')}, active).hasValue(),
        "MCP fake rejected an outbound frame at the exact byte cap");
    requireError(
        transport.send(Domain::McpFrame{"{}\n"}, active),
        Domain::ErrorCodes::MalformedMessage,
        "MCP fake accepted an outbound frame containing a newline");
    requireError(
        transport.send(
            Domain::McpFrame{std::string{"x\0y", 3}},
            active),
        Domain::ErrorCodes::MalformedMessage,
        "MCP fake accepted an outbound frame containing NUL");
    requireError(
        transport.send(Domain::McpFrame{std::string(17, 'x')}, active),
        Domain::ErrorCodes::PayloadTooLarge,
        "MCP fake accepted an oversized outbound frame");
    requireError(
        transport.send(Domain::McpFrame{""}, active),
        Domain::ErrorCodes::MalformedMessage,
        "MCP fake accepted an empty outbound frame");
    require(
        transport.outbound().size() == 2,
        "MCP fake retained an invalid outbound frame");
}

inline void testMcpBoundsAndOperationContext(const Fixture& fixture)
{
    const auto active = fixture.activeContext();
    const Domain::McpFrame validFrame{"{}"};

    Fakes::McpTransportFake bounded{
        std::vector<std::string>{},
        fixture.budgets(),
        fixture.now,
        1};
    require(
        bounded.send(validFrame, active).hasValue(),
        "bounded MCP fake rejected its first outbound frame");
    requireError(
        bounded.send(validFrame, active),
        Domain::ErrorCodes::LimitExceeded,
        "bounded MCP fake exceeded its outbound queue capacity");

    Fakes::McpTransportFake contextAware{
        std::vector<std::string>{"{}"},
        fixture.budgets(),
        fixture.now,
        2};
    const auto cancelled = fixture.cancelledContext();
    requireError(
        contextAware.receive(cancelled),
        Domain::ErrorCodes::Cancelled,
        "MCP receive ignored operation cancellation");
    requireError(
        contextAware.send(validFrame, cancelled),
        Domain::ErrorCodes::Cancelled,
        "MCP send ignored operation cancellation");

    const auto expired = fixture.expiredContext();
    requireError(
        contextAware.receive(expired),
        Domain::ErrorCodes::DeadlineExceeded,
        "MCP receive ignored the operation deadline");
    requireError(
        contextAware.send(validFrame, expired),
        Domain::ErrorCodes::DeadlineExceeded,
        "MCP send ignored the operation deadline");

    contextAware.shutdown();
    requireError(
        contextAware.receive(active),
        Domain::ErrorCodes::TransportClosed,
        "MCP receive accepted work after shutdown");
    requireError(
        contextAware.send(validFrame, active),
        Domain::ErrorCodes::TransportClosed,
        "MCP send accepted work after shutdown");
}

} // namespace McpCancellationContractTestsDetail

inline void runMcpCancellationContractTests()
{
    const McpCancellationContractTestsDetail::Fixture fixture;
    McpCancellationContractTestsDetail::testDeadlineScheduler(fixture);
    McpCancellationContractTestsDetail::testMcpFrameValidationAndEof(fixture);
    McpCancellationContractTestsDetail::testMcpBoundsAndOperationContext(fixture);
}

} // namespace ForgeConductor::Tests
