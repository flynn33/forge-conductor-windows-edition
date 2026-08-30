#include "Infrastructure/Windows/Detail/DashboardHandlerOperations.h"

#include "ForgeConductor/Dashboard/DashboardHttpParser.h"
#include "ForgeConductor/Dashboard/DashboardResponseComposer.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace {

namespace Dashboard = ForgeConductor::Dashboard;
namespace Detail = ForgeConductor::Infrastructure::Windows::Detail;
namespace Domain = ForgeConductor::Domain;
namespace Windows = ForgeConductor::Infrastructure::Windows;

using CompletionKind = Windows::DashboardHandlerCompletionKind;
using HandlerOperation = Windows::IDashboardHandlerOperation;
using PostOperation = Detail::DashboardPostDeliveryHandlerOperation;
using PrepareOperation = Detail::DashboardPrepareHandlerOperation;

using namespace std::chrono_literals;

std::size_t assertionCount{};

void require(const bool condition, const std::string_view message)
{
    ++assertionCount;
    if (!condition) {
        throw std::runtime_error{std::string{message}};
    }
}

template <typename Value>
[[nodiscard]] Value take(Domain::Result<Value> result)
{
    if (!result) {
        throw std::runtime_error{
            result.error().code + ": " + result.error().message};
    }
    return std::move(result).value();
}

static_assert(std::is_final_v<PrepareOperation>);
static_assert(std::is_final_v<PostOperation>);
static_assert(std::is_base_of_v<HandlerOperation, PrepareOperation>);
static_assert(std::is_base_of_v<HandlerOperation, PostOperation>);
static_assert(!std::is_copy_constructible_v<PrepareOperation>);
static_assert(!std::is_move_constructible_v<PrepareOperation>);
static_assert(!std::is_copy_constructible_v<PostOperation>);
static_assert(!std::is_move_constructible_v<PostOperation>);
static_assert(noexcept(PrepareOperation::createRequest(
    std::declval<
        std::shared_ptr<Dashboard::IDashboardConnectionApplication>>(),
    std::declval<Dashboard::DashboardHttpRequest>(),
    true)));
static_assert(noexcept(PrepareOperation::createRejection(
    std::declval<Dashboard::DashboardHttpRejection>())));
static_assert(noexcept(PostOperation::create(
    std::declval<
        std::shared_ptr<Dashboard::IDashboardConnectionApplication>>(),
    Dashboard::DashboardPostDeliveryAction::None)));
static_assert(noexcept(
    std::declval<const PrepareOperation&>().completionKind()));
static_assert(noexcept(
    std::declval<const PostOperation&>().completionKind()));

[[nodiscard]] Domain::OperationContext context(const std::uint64_t sequence)
{
    auto operation = Domain::OperationId::parse(
        "10000000-0000-4000-8000-00000000000" +
        std::to_string(sequence));
    auto correlation = Domain::CorrelationId::parse(
        "dashboard-handler-operation-" + std::to_string(sequence));
    require(operation.hasValue(), "test operation identity was invalid");
    require(correlation.hasValue(), "test correlation identity was invalid");
    return Domain::OperationContext{
        std::move(operation).value(),
        std::chrono::steady_clock::now() + 5s,
        {},
        std::move(correlation).value()};
}

[[nodiscard]] std::vector<std::byte> bytes(const std::string_view text)
{
    std::vector<std::byte> output(text.size());
    if (!text.empty()) {
        std::memcpy(output.data(), text.data(), text.size());
    }
    return output;
}

[[nodiscard]] std::string text(const std::vector<std::byte>& value)
{
    return std::string{
        reinterpret_cast<const char*>(value.data()), value.size()};
}

class RecordingApplication final
    : public Dashboard::IDashboardConnectionApplication {
public:
    [[nodiscard]] Domain::Result<Dashboard::DashboardPreparedExchange> prepare(
        Dashboard::DashboardHttpRequest request,
        const bool operationalServiceActive,
        Domain::OperationContext operationContext) noexcept override
    {
        try {
            ++prepareCalls;
            preparedRequest.emplace(std::move(request));
            preparedOperationalState = operationalServiceActive;
            preparedContext.emplace(std::move(operationContext));
            if (prepareError.has_value()) {
                return Domain::Result<
                    Dashboard::DashboardPreparedExchange>::failure(
                    *prepareError);
            }
            return Dashboard::DashboardResponseComposer::completeText(
                200U, "text/plain; charset=utf-8", "prepared");
        } catch (...) {
            return Domain::Result<Dashboard::DashboardPreparedExchange>::failure(
                Domain::makeError(
                    Domain::ErrorCodes::InternalFailure,
                    "The recording application failed."));
        }
    }

    [[nodiscard]] Domain::Result<void> executePostDelivery(
        const Dashboard::DashboardPostDeliveryAction action,
        Domain::OperationContext operationContext) noexcept override
    {
        try {
            ++postDeliveryCalls;
            postDeliveryAction = action;
            postDeliveryContext.emplace(std::move(operationContext));
            if (postDeliveryError.has_value()) {
                return Domain::Result<void>::failure(*postDeliveryError);
            }
            return Domain::Result<void>::success();
        } catch (...) {
            return Domain::Result<void>::failure(Domain::makeError(
                Domain::ErrorCodes::InternalFailure,
                "The recording post-delivery action failed."));
        }
    }

    std::size_t prepareCalls{};
    std::size_t postDeliveryCalls{};
    std::optional<Dashboard::DashboardHttpRequest> preparedRequest;
    std::optional<Domain::OperationContext> preparedContext;
    std::optional<Domain::OperationContext> postDeliveryContext;
    std::optional<Domain::Error> prepareError;
    std::optional<Domain::Error> postDeliveryError;
    Dashboard::DashboardPostDeliveryAction postDeliveryAction{
        Dashboard::DashboardPostDeliveryAction::None};
    bool preparedOperationalState{};
};

void factoriesRejectInvalidDependenciesAndActions()
{
    const auto missingApplication = PrepareOperation::createRequest(
        {}, Dashboard::DashboardHttpRequest{"GET", "/", {}, {}}, true);
    require(!missingApplication, "request operation accepted a null app");
    require(missingApplication.error().code ==
                Domain::ErrorCodes::InvalidRequest,
            "null request app used the wrong error code");

    const auto app = std::make_shared<RecordingApplication>();
    const auto missingPostApplication = PostOperation::create(
        {}, Dashboard::DashboardPostDeliveryAction::RequestManagerShutdown);
    require(!missingPostApplication,
            "post-delivery operation accepted a null app");
    const auto emptyAction = PostOperation::create(
        app, Dashboard::DashboardPostDeliveryAction::None);
    require(!emptyAction, "post-delivery operation accepted None");
    const auto restartAction = PostOperation::create(
        app, Dashboard::DashboardPostDeliveryAction::RequestManagerRestart);
    require(restartAction.hasValue(),
            "post-delivery operation rejected manager restart");
    const auto shutdownAction = PostOperation::create(
        app, Dashboard::DashboardPostDeliveryAction::RequestManagerShutdown);
    require(shutdownAction.hasValue(),
            "post-delivery operation rejected manager shutdown");
    const auto undefinedAction = PostOperation::create(
        app, static_cast<Dashboard::DashboardPostDeliveryAction>(0xffU));
    require(!undefinedAction,
            "post-delivery operation accepted an undefined action");
}

void requestOperationOwnsAndForwardsOneSnapshotExactlyOnce()
{
    auto app = std::make_shared<RecordingApplication>();
    const auto originalContext = context(1U);
    auto created = PrepareOperation::createRequest(
        app,
        Dashboard::DashboardHttpRequest{
            "POST",
            "/api/manager/settings",
            {{"content-type", "application/json"}},
            bytes("{}")},
        false);
    require(created.hasValue(), "valid request operation was rejected");
    std::unique_ptr<HandlerOperation> operation{std::move(created).value()};
    require(operation->completionKind() == CompletionKind::PreparedExchange,
            "request operation exposed the wrong completion kind");

    auto completion = operation->execute(originalContext);
    require(completion.kind() == CompletionKind::PreparedExchange,
            "request execution changed completion kind");
    auto* result = completion.preparedResult();
    require(result != nullptr && result->hasValue(),
            "request execution did not return a prepared exchange");
    require(result->value().kind() ==
                Dashboard::DashboardPreparedExchange::Kind::Complete,
            "request execution returned the wrong exchange kind");
    require(app->prepareCalls == 1U,
            "request operation did not call the app exactly once");
    require(app->preparedRequest.has_value() &&
                app->preparedRequest->method() == "POST" &&
                app->preparedRequest->target() == "/api/manager/settings" &&
                app->preparedRequest->body() == bytes("{}"),
            "request operation changed its owned request");
    require(!app->preparedOperationalState,
            "request operation changed the operational-state snapshot");
    require(app->preparedContext.has_value() &&
                app->preparedContext->operationId ==
                    originalContext.operationId &&
                app->preparedContext->correlationId ==
                    originalContext.correlationId &&
                app->preparedContext->deadline == originalContext.deadline,
            "request operation changed its context");

    auto repeated = operation->execute(originalContext);
    require(repeated.kind() == CompletionKind::PreparedExchange,
            "repeated request execution changed completion kind");
    require(repeated.preparedResult() != nullptr &&
                !repeated.preparedResult()->hasValue() &&
                repeated.preparedResult()->error().code ==
                    Domain::ErrorCodes::IntegrityFailure,
            "repeated request execution was not rejected as integrity failure");
    require(app->prepareCalls == 1U,
            "repeated request execution called the app again");
}

void requestOperationPreservesApplicationFailureAndOwnership()
{
    auto app = std::make_shared<RecordingApplication>();
    app->prepareError = Domain::makeError(
        Domain::ErrorCodes::DatabaseBusy, "Injected prepare failure.", true);
    std::weak_ptr<RecordingApplication> weak = app;
    auto operation = take(PrepareOperation::createRequest(
        app,
        Dashboard::DashboardHttpRequest{"GET", "/api/status", {}, {}},
        true));
    app.reset();
    require(!weak.expired(),
            "request operation did not retain the application");

    auto completion = operation->execute(context(2U));
    require(completion.preparedResult() != nullptr &&
                !completion.preparedResult()->hasValue(),
            "application prepare failure became success");
    require(completion.preparedResult()->error().code ==
                Domain::ErrorCodes::DatabaseBusy &&
                completion.preparedResult()->error().retryable,
            "application prepare failure metadata changed");
    operation.reset();
    require(weak.expired(),
            "destroyed request operation retained the application");
}

void canonicalParserRejectionBecomesOneCompleteResponse()
{
    Dashboard::DashboardHttpParser parser;
    const auto wire = bytes("GET / HTTP/1.0\r\n\r\n");
    const auto parsed = parser.parse(wire, true);
    require(parsed.kind() == Dashboard::DashboardHttpParseResult::Kind::Rejected &&
                parsed.rejection() != nullptr,
            "test wire did not produce a canonical parser rejection");

    auto operation = take(
        PrepareOperation::createRejection(*parsed.rejection()));
    require(operation->completionKind() == CompletionKind::PreparedExchange,
            "rejection operation exposed the wrong completion kind");
    auto completion = operation->execute(context(3U));
    auto* prepared = completion.preparedResult();
    require(prepared != nullptr && prepared->hasValue(),
            "parser rejection did not compose a prepared response");
    auto* exchange = prepared->value().completeExchange();
    require(exchange != nullptr,
            "parser rejection did not compose a complete response");
    const auto responseText = text(exchange->encodedResponse().bytes());
    require(responseText.starts_with("HTTP/1.1 400 Bad Request\r\n"),
            "parser rejection changed its status");
    require(responseText.find("\"code\":\"invalid_request\"") !=
                std::string::npos,
            "parser rejection changed its canonical code");
    require(exchange->takePostDeliveryAction() ==
                Dashboard::DashboardPostDeliveryAction::None,
            "parser rejection acquired a post-delivery action");

    auto repeated = operation->execute(context(4U));
    require(repeated.preparedResult() != nullptr &&
                !repeated.preparedResult()->hasValue() &&
                repeated.preparedResult()->error().code ==
                    Domain::ErrorCodes::IntegrityFailure,
            "parser rejection operation executed more than once");
}

void postDeliveryOperationOwnsActionAndForwardsFailureExactlyOnce()
{
    auto app = std::make_shared<RecordingApplication>();
    app->postDeliveryError = Domain::makeError(
        Domain::ErrorCodes::TransportClosed,
        "Injected post-delivery failure.",
        true);
    std::weak_ptr<RecordingApplication> weak = app;
    auto operation = take(PostOperation::create(
        app,
        Dashboard::DashboardPostDeliveryAction::RequestManagerRestart));
    app.reset();
    require(!weak.expired(),
            "post-delivery operation did not retain the application");
    require(operation->completionKind() == CompletionKind::PostDelivery,
            "post-delivery operation exposed the wrong completion kind");

    const auto originalContext = context(5U);
    auto completion = operation->execute(originalContext);
    require(completion.kind() == CompletionKind::PostDelivery,
            "post-delivery execution changed completion kind");
    auto* result = completion.postDeliveryResult();
    require(result != nullptr && !result->hasValue(),
            "post-delivery failure became success");
    require(result->error().code == Domain::ErrorCodes::TransportClosed &&
                result->error().retryable,
            "post-delivery failure metadata changed");
    auto retained = weak.lock();
    require(retained != nullptr && retained->postDeliveryCalls == 1U,
            "post-delivery action did not execute exactly once");
    require(retained->postDeliveryAction ==
                Dashboard::DashboardPostDeliveryAction::RequestManagerRestart,
            "post-delivery action changed");
    require(retained->postDeliveryContext.has_value() &&
                retained->postDeliveryContext->operationId ==
                    originalContext.operationId,
            "post-delivery context changed");

    auto repeated = operation->execute(originalContext);
    require(repeated.postDeliveryResult() != nullptr &&
                !repeated.postDeliveryResult()->hasValue() &&
                repeated.postDeliveryResult()->error().code ==
                    Domain::ErrorCodes::IntegrityFailure,
            "post-delivery action executed more than once");
    require(retained->postDeliveryCalls == 1U,
            "repeated post-delivery execution called the app again");
    retained.reset();
    operation.reset();
    require(weak.expired(),
            "destroyed post-delivery operation retained the application");
}

} // namespace

int main()
{
    try {
        factoriesRejectInvalidDependenciesAndActions();
        requestOperationOwnsAndForwardsOneSnapshotExactlyOnce();
        requestOperationPreservesApplicationFailureAndOwnership();
        canonicalParserRejectionBecomesOneCompleteResponse();
        postDeliveryOperationOwnsActionAndForwardsFailureExactlyOnce();
        std::cout << "Dashboard handler operation tests passed ("
                  << assertionCount << " assertions).\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Dashboard handler operation tests failed: "
                  << error.what() << '\n';
        return 1;
    } catch (...) {
        std::cerr << "Dashboard handler operation tests failed with an unknown "
                     "error.\n";
        return 1;
    }
}
