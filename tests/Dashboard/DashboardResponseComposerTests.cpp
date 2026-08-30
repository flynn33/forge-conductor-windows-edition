#include "ForgeConductor/Dashboard/DashboardResponseComposer.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstddef>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

namespace Dashboard = ForgeConductor::Dashboard;
namespace Domain = ForgeConductor::Domain;
using Json = nlohmann::json;

std::size_t assertions{};

#define REQUIRE(condition)                                                       \
    do {                                                                         \
        ++assertions;                                                            \
        if (!(condition)) {                                                      \
            throw std::runtime_error{std::string{"Requirement failed: "} +      \
                                     #condition};                                \
        }                                                                        \
    } while (false)

template <typename Value>
[[nodiscard]] Value take(Domain::Result<Value> result)
{
    if (!result) {
        throw std::runtime_error{
            result.error().code + ": " + result.error().message};
    }
    return std::move(result).value();
}

template <typename Value>
void requireError(
    const Domain::Result<Value>& result,
    const std::string_view expectedCode)
{
    REQUIRE(!result);
    REQUIRE(result.error().code == expectedCode);
    REQUIRE(!result.error().message.empty());
}

[[nodiscard]] std::string text(
    const std::vector<std::byte>& bytes)
{
    if (bytes.empty()) {
        return {};
    }
    return std::string{
        reinterpret_cast<const char*>(bytes.data()), bytes.size()};
}

[[nodiscard]] std::string wireText(
    const Dashboard::DashboardPreparedExchange& exchange)
{
    REQUIRE(
        exchange.kind() ==
        Dashboard::DashboardPreparedExchange::Kind::Complete);
    REQUIRE(exchange.completeExchange() != nullptr);
    return text(exchange.completeExchange()->encodedResponse().bytes());
}

[[nodiscard]] Json wireJson(
    const Dashboard::DashboardPreparedExchange& exchange)
{
    const auto wire = wireText(exchange);
    const auto separator = wire.find("\r\n\r\n");
    REQUIRE(separator != std::string::npos);
    return Json::parse(wire.substr(separator + 4U));
}

struct SubscriptionState final {
    std::size_t closes{};
    std::size_t destructions{};
};

class Subscription final : public Dashboard::IDashboardSseSubscription {
public:
    explicit Subscription(std::shared_ptr<SubscriptionState> state) noexcept
        : state_{std::move(state)}
    {
    }

    ~Subscription() noexcept override { ++state_->destructions; }

    [[nodiscard]] double deliveryHz() const noexcept override { return 2.0; }

    void attachReadySink(
        std::weak_ptr<Dashboard::IDashboardSseReadySink>) noexcept override
    {
    }

    [[nodiscard]] Dashboard::DashboardSseFramePair::ImmutableFrame takeLatest()
        noexcept override
    {
        return {};
    }

    [[nodiscard]] std::size_t pendingCount() const noexcept override
    {
        return 0U;
    }

    void close() noexcept override
    {
        if (!closed_) {
            closed_ = true;
            ++state_->closes;
        }
    }

private:
    const std::shared_ptr<SubscriptionState> state_;
    bool closed_{};
};

void reservesTheHeaderBudgetAndOwnsCompleteBodies()
{
    std::vector<std::byte> exact(
        Dashboard::DashboardResponseComposer::MaximumResponseBodyBytes,
        std::byte{0x5a});
    auto exchange = take(Dashboard::DashboardResponseComposer::complete(
        200U,
        "application/octet-stream",
        exact));
    const auto& encoded =
        exchange.completeExchange()->encodedResponse().bytes();
    REQUIRE(encoded.size() <=
            Dashboard::DashboardHttpResponseEncoder::
                MaximumEncodedResponseBytes);
    REQUIRE(encoded.size() > exact.size());
    REQUIRE(encoded.back() == std::byte{0x5a});

    exact.front() = std::byte{0x00};
    REQUIRE(encoded.back() == std::byte{0x5a});

    std::vector<std::byte> oversized(
        Dashboard::DashboardResponseComposer::MaximumResponseBodyBytes + 1U,
        std::byte{0x5a});
    requireError(
        Dashboard::DashboardResponseComposer::complete(
            200U,
            "application/octet-stream",
            std::move(oversized)),
        Domain::ErrorCodes::PayloadTooLarge);

    const auto noContent = take(Dashboard::DashboardResponseComposer::complete(
        204U,
        "application/json; charset=utf-8",
        {}));
    const auto noContentWire = wireText(noContent);
    REQUIRE(noContentWire.starts_with("HTTP/1.1 204 No Content\r\n"));
    REQUIRE(noContentWire.find("Content-Length:") == std::string::npos);
}

void composesTextHeadAndOneShotPostDeliveryActions()
{
    auto response = take(Dashboard::DashboardResponseComposer::completeText(
        200U,
        "application/json; charset=utf-8",
        "{\"ok\":true}",
        {},
        Dashboard::DashboardPostDeliveryAction::RequestManagerRestart));
    const auto wire = wireText(response);
    REQUIRE(wire.ends_with("{\"ok\":true}"));
    REQUIRE(
        response.completeExchange()->takePostDeliveryAction() ==
        Dashboard::DashboardPostDeliveryAction::RequestManagerRestart);
    REQUIRE(
        response.completeExchange()->takePostDeliveryAction() ==
        Dashboard::DashboardPostDeliveryAction::None);

    const auto head = take(Dashboard::DashboardResponseComposer::head(
        200U,
        "text/html; charset=utf-8",
        123U));
    const auto headWire = wireText(head);
    REQUIRE(headWire.find("Content-Length: 123\r\n") != std::string::npos);
    REQUIRE(headWire.ends_with("\r\n\r\n"));
    requireError(
        Dashboard::DashboardResponseComposer::head(
            200U,
            "text/html; charset=utf-8",
            Dashboard::DashboardResponseComposer::MaximumResponseBodyBytes +
                1U),
        Domain::ErrorCodes::PayloadTooLarge);
}

void encodesClosedJsonErrorsAndFallsBackSafely()
{
    const Dashboard::DashboardHttpRejection rejection{
        405U,
        "method_not_allowed",
        "Use \"GET\" only.\nRetry later.",
        {{"allow", "GET"}}};
    const auto response = take(
        Dashboard::DashboardResponseComposer::rejection(rejection));
    const auto wire = wireText(response);
    REQUIRE(wire.starts_with("HTTP/1.1 405 Method Not Allowed\r\n"));
    REQUIRE(wire.find("Allow: GET\r\n") != std::string::npos);
    const auto document = wireJson(response);
    REQUIRE(document.size() == 3U);
    REQUIRE(document.at("ok") == false);
    REQUIRE(document.at("code") == "method_not_allowed");
    REQUIRE(document.at("message") == rejection.message);

    const auto headResponse = take(
        Dashboard::DashboardResponseComposer::headRejection(rejection));
    const auto headWire = wireText(headResponse);
    REQUIRE(headWire.starts_with("HTTP/1.1 405 Method Not Allowed\r\n"));
    REQUIRE(headWire.find("Allow: GET\r\n") != std::string::npos);
    REQUIRE(headWire.ends_with("\r\n\r\n"));
    const auto bodyOffset = wire.find("\r\n\r\n") + 4U;
    const auto bodyLength = wire.size() - bodyOffset;
    REQUIRE(headWire.find(
        "Content-Length: " + std::to_string(bodyLength) + "\r\n") !=
        std::string::npos);

    const std::string embeddedNull{"bad\0code", 8U};
    const auto invalidMetadata = take(
        Dashboard::DashboardResponseComposer::errorResponse(
            400U,
            embeddedNull,
            "message",
            {{"allow", "GET"}}));
    const auto invalidWire = wireText(invalidMetadata);
    REQUIRE(invalidWire.starts_with(
        "HTTP/1.1 500 Internal Server Error\r\n"));
    REQUIRE(invalidWire.find("Allow:") == std::string::npos);
    const auto invalidDocument = wireJson(invalidMetadata);
    REQUIRE(invalidDocument.at("code") == "internal_failure");
    REQUIRE(invalidDocument.at("message") ==
            "The dashboard request failed safely.");

    const auto invalidHead = take(
        Dashboard::DashboardResponseComposer::headErrorResponse(
            400U,
            embeddedNull,
            "message",
            {{"allow", "GET"}}));
    const auto invalidHeadWire = wireText(invalidHead);
    REQUIRE(invalidHeadWire.starts_with(
        "HTTP/1.1 500 Internal Server Error\r\n"));
    REQUIRE(invalidHeadWire.find("Allow:") == std::string::npos);
    REQUIRE(invalidHeadWire.ends_with("\r\n\r\n"));

    const auto invalidHeader = take(
        Dashboard::DashboardResponseComposer::errorResponse(
            400U,
            "invalid_request",
            "message",
            {{"x-unapproved", "value"}}));
    REQUIRE(wireText(invalidHeader).starts_with(
        "HTTP/1.1 500 Internal Server Error\r\n"));

    const auto unsupportedStatus = take(
        Dashboard::DashboardResponseComposer::errorResponse(
            418U,
            "invalid_request",
            "message"));
    REQUIRE(wireText(unsupportedStatus).starts_with(
        "HTTP/1.1 500 Internal Server Error\r\n"));
}

void composesAndClosesSseOwnership()
{
    const auto state = std::make_shared<SubscriptionState>();
    {
        auto response = take(
            Dashboard::DashboardResponseComposer::serverSentEvents(
                std::make_unique<Subscription>(state)));
        REQUIRE(
            response.kind() ==
            Dashboard::DashboardPreparedExchange::Kind::ServerSentEvents);
        REQUIRE(response.sseExchange() != nullptr);
        REQUIRE(response.sseExchange()->subscription() != nullptr);
        REQUIRE(response.sseExchange()->subscription()->deliveryHz() == 2.0);
        REQUIRE(
            response.sseExchange()->encodedHead().kind() ==
            Dashboard::DashboardHttpEncodingResult::Kind::SseBootstrapHead);
        REQUIRE(
            text(response.sseExchange()->connectedCommentBytes()) ==
            ": connected realtime\n\n");
        REQUIRE(state->closes == 0U);
        REQUIRE(state->destructions == 0U);
    }
    REQUIRE(state->closes == 1U);
    REQUIRE(state->destructions == 1U);

    requireError(
        Dashboard::DashboardResponseComposer::serverSentEvents({}),
        Domain::ErrorCodes::IntegrityFailure);
}

} // namespace

int main()
{
    try {
        reservesTheHeaderBudgetAndOwnsCompleteBodies();
        composesTextHeadAndOneShotPostDeliveryActions();
        encodesClosedJsonErrorsAndFallsBackSafely();
        composesAndClosesSseOwnership();
        std::cout << "Dashboard response composer tests passed ("
                  << assertions << " assertions).\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Dashboard response composer tests failed: "
                  << error.what() << '\n';
        return 1;
    } catch (...) {
        std::cerr << "Dashboard response composer tests failed with an "
                     "unknown error.\n";
        return 1;
    }
}
