#include "ForgeConductor/Dashboard/IDashboardConnectionApplication.h"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <limits>
#include <memory>
#include <stdexcept>
#include <stop_token>
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

static_assert(std::is_abstract_v<Dashboard::IDashboardSseReadySink>);
static_assert(std::has_virtual_destructor_v<Dashboard::IDashboardSseReadySink>);
static_assert(std::is_abstract_v<Dashboard::IDashboardSseSubscription>);
static_assert(
    std::has_virtual_destructor_v<Dashboard::IDashboardSseSubscription>);
static_assert(
    std::is_abstract_v<Dashboard::IDashboardConnectionApplication>);
static_assert(std::has_virtual_destructor_v<
              Dashboard::IDashboardConnectionApplication>);
static_assert(!std::is_convertible_v<
              Dashboard::DashboardPostDeliveryAction,
              int>);
static_assert(!std::is_copy_constructible_v<
              Dashboard::DashboardCompleteExchange>);
static_assert(!std::is_copy_constructible_v<Dashboard::DashboardSseExchange>);
static_assert(!std::is_copy_constructible_v<
              Dashboard::DashboardPreparedExchange>);
static_assert(!std::is_copy_assignable_v<
              Dashboard::DashboardPreparedExchange>);
static_assert(std::is_nothrow_move_constructible_v<
              Dashboard::DashboardPreparedExchange>);
static_assert(std::is_nothrow_move_assignable_v<
              Dashboard::DashboardPreparedExchange>);
static_assert(noexcept(std::declval<Dashboard::IDashboardSseReadySink&>().signal()));
static_assert(noexcept(
    std::declval<const Dashboard::IDashboardSseSubscription&>().deliveryHz()));
static_assert(noexcept(
    std::declval<Dashboard::IDashboardSseSubscription&>().attachReadySink(
        std::declval<std::weak_ptr<Dashboard::IDashboardSseReadySink>>())));
static_assert(noexcept(
    std::declval<Dashboard::IDashboardSseSubscription&>().takeLatest()));
static_assert(noexcept(
    std::declval<const Dashboard::IDashboardSseSubscription&>().pendingCount()));
static_assert(noexcept(
    std::declval<Dashboard::IDashboardSseSubscription&>().close()));
static_assert(noexcept(
    std::declval<Dashboard::IDashboardConnectionApplication&>().prepare(
        std::declval<Dashboard::DashboardHttpRequest>(),
        true,
        std::declval<Domain::OperationContext>())));
static_assert(noexcept(
    std::declval<Dashboard::IDashboardConnectionApplication&>()
        .executePostDelivery(
            Dashboard::DashboardPostDeliveryAction::None,
            std::declval<Domain::OperationContext>())));

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

[[nodiscard]] Dashboard::DashboardHttpEncodingResult completeEncoding()
{
    return Dashboard::DashboardHttpResponseEncoder::encode(
        Dashboard::DashboardHttpResponse{
            200U,
            "application/json; charset=utf-8",
            bytes("{\"ok\":true}")});
}

[[nodiscard]] Dashboard::DashboardHttpEncodingResult headEncoding()
{
    return Dashboard::DashboardHttpResponseEncoder::encodeHead(
        Dashboard::DashboardHttpHeadResponse{
            200U, "application/json; charset=utf-8", 11U});
}

[[nodiscard]] Dashboard::DashboardHttpEncodingResult failedEncoding()
{
    return Dashboard::DashboardHttpResponseEncoder::encode(
        Dashboard::DashboardHttpResponse{
            418U, "text/plain; charset=utf-8", bytes("no")});
}

struct SubscriptionCounters final {
    std::size_t closeCalls{};
    std::size_t destructions{};
};

class ReadySink final : public Dashboard::IDashboardSseReadySink {
public:
    void signal() noexcept override { ++signals_; }

    [[nodiscard]] std::size_t signals() const noexcept { return signals_; }

private:
    std::size_t signals_{};
};

class LatestSubscription final : public Dashboard::IDashboardSseSubscription {
public:
    explicit LatestSubscription(
        SubscriptionCounters& counters,
        const double deliveryHz = 2.0) noexcept
        : counters_{counters},
          deliveryHz_{deliveryHz}
    {
    }

    ~LatestSubscription() noexcept override { ++counters_.destructions; }

    [[nodiscard]] double deliveryHz() const noexcept override
    {
        return deliveryHz_;
    }

    void attachReadySink(
        std::weak_ptr<Dashboard::IDashboardSseReadySink> sink) noexcept override
    {
        sink_ = std::move(sink);
        if (!closed_ && pending_ != nullptr) {
            signalIfAttached();
        }
    }

    [[nodiscard]] Dashboard::DashboardSseFramePair::ImmutableFrame takeLatest()
        noexcept override
    {
        if (closed_) {
            return {};
        }
        return std::exchange(pending_, {});
    }

    [[nodiscard]] std::size_t pendingCount() const noexcept override
    {
        return pending_ == nullptr ? 0U : 1U;
    }

    void close() noexcept override
    {
        if (!closed_) {
            closed_ = true;
            ++counters_.closeCalls;
            pending_.reset();
            sink_.reset();
        }
    }

    void publish(
        Dashboard::DashboardSseFramePair::ImmutableFrame frame) noexcept
    {
        if (closed_ || frame == nullptr) {
            return;
        }
        const bool wasEmpty = pending_ == nullptr;
        pending_ = std::move(frame);
        if (wasEmpty) {
            signalIfAttached();
        }
    }

private:
    void signalIfAttached() noexcept
    {
        if (auto sink = sink_.lock(); sink != nullptr) {
            sink->signal();
        }
    }

    SubscriptionCounters& counters_;
    const double deliveryHz_{};
    std::weak_ptr<Dashboard::IDashboardSseReadySink> sink_;
    Dashboard::DashboardSseFramePair::ImmutableFrame pending_;
    bool closed_{};
};

[[nodiscard]] Dashboard::DashboardSseFramePair::ImmutableFrame frame(
    const std::uint64_t sequence,
    const std::string_view compact,
    const std::string_view full)
{
    auto created = Dashboard::DashboardSseFramePair::create(
        sequence, bytes(compact), bytes(full));
    REQUIRE(created.hasValue());
    return std::move(created).value();
}

void ownsExactCompleteAndHeadEncodingsAndActions()
{
    auto complete = completeEncoding();
    REQUIRE(complete.hasValue());
    const auto expectedComplete = complete.bytes();

    auto prepared = Dashboard::DashboardPreparedExchange::createComplete(
        std::move(complete),
        Dashboard::DashboardPostDeliveryAction::RequestManagerShutdown);
    REQUIRE(prepared.hasValue());
    auto firstOwner = std::move(prepared).value();
    REQUIRE(
        firstOwner.kind() ==
        Dashboard::DashboardPreparedExchange::Kind::Complete);
    REQUIRE(firstOwner.sseExchange() == nullptr);
    REQUIRE(firstOwner.completeExchange() != nullptr);
    REQUIRE(
        firstOwner.completeExchange()->encodedResponse().kind() ==
        Dashboard::DashboardHttpEncodingResult::Kind::CompleteResponse);
    REQUIRE(
        firstOwner.completeExchange()->encodedResponse().bytes() ==
        expectedComplete);
    REQUIRE(
        firstOwner.completeExchange()->takePostDeliveryAction() ==
        Dashboard::DashboardPostDeliveryAction::RequestManagerShutdown);
    REQUIRE(
        firstOwner.completeExchange()->takePostDeliveryAction() ==
        Dashboard::DashboardPostDeliveryAction::None);

    auto movedOwner = std::move(firstOwner);
    REQUIRE(
        firstOwner.kind() ==
        Dashboard::DashboardPreparedExchange::Kind::Empty);
    REQUIRE(firstOwner.completeExchange() == nullptr);
    REQUIRE(firstOwner.sseExchange() == nullptr);
    REQUIRE(
        movedOwner.kind() ==
        Dashboard::DashboardPreparedExchange::Kind::Complete);
    REQUIRE(movedOwner.completeExchange() != nullptr);
    REQUIRE(
        movedOwner.completeExchange()->encodedResponse().bytes() ==
        expectedComplete);

    auto head = headEncoding();
    REQUIRE(head.hasValue());
    const auto expectedHead = head.bytes();
    auto preparedHead = Dashboard::DashboardPreparedExchange::createComplete(
        std::move(head), Dashboard::DashboardPostDeliveryAction::None);
    REQUIRE(preparedHead.hasValue());
    const auto& headOwner = preparedHead.value();
    REQUIRE(headOwner.completeExchange() != nullptr);
    REQUIRE(
        headOwner.completeExchange()->encodedResponse().kind() ==
        Dashboard::DashboardHttpEncodingResult::Kind::HeadResponseHead);
    REQUIRE(
        headOwner.completeExchange()->encodedResponse().bytes() ==
        expectedHead);
    REQUIRE(
        preparedHead.value()
                .completeExchange()
                ->takePostDeliveryAction() ==
        Dashboard::DashboardPostDeliveryAction::None);
}

void rejectsInvalidCompleteExchangeInputs()
{
    auto failed = Dashboard::DashboardPreparedExchange::createComplete(
        failedEncoding(), Dashboard::DashboardPostDeliveryAction::None);
    REQUIRE(!failed.hasValue());
    REQUIRE(failed.error().code == Domain::ErrorCodes::IntegrityFailure);

    auto sseHead = Dashboard::DashboardHttpResponseEncoder::encodeSseBootstrap();
    REQUIRE(sseHead.hasValue());
    auto wrongKind = Dashboard::DashboardPreparedExchange::createComplete(
        std::move(sseHead), Dashboard::DashboardPostDeliveryAction::None);
    REQUIRE(!wrongKind.hasValue());
    REQUIRE(wrongKind.error().code == Domain::ErrorCodes::IntegrityFailure);

    auto invalidAction = Dashboard::DashboardPreparedExchange::createComplete(
        completeEncoding(),
        static_cast<Dashboard::DashboardPostDeliveryAction>(0xffU));
    REQUIRE(!invalidAction.hasValue());
    REQUIRE(invalidAction.error().code == Domain::ErrorCodes::IntegrityFailure);

    auto headWithAction = Dashboard::DashboardPreparedExchange::createComplete(
        headEncoding(),
        Dashboard::DashboardPostDeliveryAction::RequestManagerShutdown);
    REQUIRE(!headWithAction.hasValue());
    REQUIRE(
        headWithAction.error().code == Domain::ErrorCodes::IntegrityFailure);
}

void transfersDirectCompleteExchangeActionsExactlyOnce()
{
    auto restartResult = Dashboard::DashboardCompleteExchange::create(
        completeEncoding(),
        Dashboard::DashboardPostDeliveryAction::RequestManagerRestart);
    REQUIRE(restartResult.hasValue());
    auto restartOwner = std::move(restartResult).value();
    REQUIRE(
        restartOwner.takePostDeliveryAction() ==
        Dashboard::DashboardPostDeliveryAction::RequestManagerRestart);
    REQUIRE(
        restartOwner.takePostDeliveryAction() ==
        Dashboard::DashboardPostDeliveryAction::None);

    auto constructorResult = Dashboard::DashboardCompleteExchange::create(
        completeEncoding(),
        Dashboard::DashboardPostDeliveryAction::RequestManagerShutdown);
    REQUIRE(constructorResult.hasValue());
    auto constructorSource = std::move(constructorResult).value();
    Dashboard::DashboardCompleteExchange constructorDestination{
        std::move(constructorSource)};
    REQUIRE(
        constructorSource.takePostDeliveryAction() ==
        Dashboard::DashboardPostDeliveryAction::None);
    REQUIRE(
        constructorDestination.takePostDeliveryAction() ==
        Dashboard::DashboardPostDeliveryAction::RequestManagerShutdown);
    REQUIRE(
        constructorDestination.takePostDeliveryAction() ==
        Dashboard::DashboardPostDeliveryAction::None);

    auto assignmentSourceResult =
        Dashboard::DashboardCompleteExchange::create(
            completeEncoding(),
            Dashboard::DashboardPostDeliveryAction::RequestManagerShutdown);
    auto assignmentDestinationResult =
        Dashboard::DashboardCompleteExchange::create(
            completeEncoding(), Dashboard::DashboardPostDeliveryAction::None);
    REQUIRE(assignmentSourceResult.hasValue());
    REQUIRE(assignmentDestinationResult.hasValue());
    auto assignmentSource = std::move(assignmentSourceResult).value();
    auto assignmentDestination =
        std::move(assignmentDestinationResult).value();
    assignmentDestination = std::move(assignmentSource);
    REQUIRE(
        assignmentSource.takePostDeliveryAction() ==
        Dashboard::DashboardPostDeliveryAction::None);
    REQUIRE(
        assignmentDestination.takePostDeliveryAction() ==
        Dashboard::DashboardPostDeliveryAction::RequestManagerShutdown);
    REQUIRE(
        assignmentDestination.takePostDeliveryAction() ==
        Dashboard::DashboardPostDeliveryAction::None);
}

void validatesImmutableSharedFramePairs()
{
    const auto compact = bytes("data: {\"compact\":true}\n\n");
    const auto full = bytes("data: {\"full\":true}\n\n");
    auto created = Dashboard::DashboardSseFramePair::create(
        42U, compact, full);
    REQUIRE(created.hasValue());
    const auto pair = std::move(created).value();
    REQUIRE(pair != nullptr);
    REQUIRE(pair->sourceSequence() == 42U);
    REQUIRE(*pair->compactBytes() == compact);
    REQUIRE(*pair->fullBytes() == full);

    auto mutableCompact = bytes("data: retained-compact\n\n");
    auto mutableFull = bytes("data: retained-full\n\n");
    const auto expectedCompact = mutableCompact;
    const auto expectedFull = mutableFull;
    auto retainedAlias = Dashboard::DashboardSseFramePair::create(
        43U, mutableCompact, mutableFull);
    REQUIRE(retainedAlias.hasValue());
    mutableCompact.assign(
        Dashboard::DashboardSseFramePair::MaximumFrameBytes + 1U,
        std::byte{0x61});
    mutableFull.clear();
    REQUIRE(*retainedAlias.value()->compactBytes() == expectedCompact);
    REQUIRE(*retainedAlias.value()->fullBytes() == expectedFull);

    auto missingCompact = Dashboard::DashboardSseFramePair::create(
        1U, {}, full);
    REQUIRE(!missingCompact.hasValue());
    REQUIRE(missingCompact.error().code == Domain::ErrorCodes::IntegrityFailure);

    auto missingFull = Dashboard::DashboardSseFramePair::create(
        1U, compact, {});
    REQUIRE(!missingFull.hasValue());
    REQUIRE(missingFull.error().code == Domain::ErrorCodes::IntegrityFailure);

    const std::vector<std::byte> empty;
    auto emptyCompact = Dashboard::DashboardSseFramePair::create(
        1U, empty, full);
    REQUIRE(!emptyCompact.hasValue());
    REQUIRE(emptyCompact.error().code == Domain::ErrorCodes::IntegrityFailure);

    auto emptyFull = Dashboard::DashboardSseFramePair::create(
        1U, compact, empty);
    REQUIRE(!emptyFull.hasValue());
    REQUIRE(emptyFull.error().code == Domain::ErrorCodes::IntegrityFailure);

    const std::vector<std::byte> exactLimit(
        Dashboard::DashboardSseFramePair::MaximumFrameBytes,
        std::byte{0x41});
    auto exactCompact = Dashboard::DashboardSseFramePair::create(
        2U, exactLimit, full);
    REQUIRE(exactCompact.hasValue());
    REQUIRE(
        exactCompact.value()->compactBytes()->size() ==
        Dashboard::DashboardSseFramePair::MaximumFrameBytes);
    auto exactFull = Dashboard::DashboardSseFramePair::create(
        3U, compact, exactLimit);
    REQUIRE(exactFull.hasValue());
    REQUIRE(
        exactFull.value()->fullBytes()->size() ==
        Dashboard::DashboardSseFramePair::MaximumFrameBytes);

    const std::vector<std::byte> overLimit(
        Dashboard::DashboardSseFramePair::MaximumFrameBytes + 1U,
        std::byte{0x41});
    auto oversized = Dashboard::DashboardSseFramePair::create(
        1U, compact, overLimit);
    REQUIRE(!oversized.hasValue());
    REQUIRE(oversized.error().code == Domain::ErrorCodes::PayloadTooLarge);
    auto oversizedCompact = Dashboard::DashboardSseFramePair::create(
        1U, overLimit, full);
    REQUIRE(!oversizedCompact.hasValue());
    REQUIRE(
        oversizedCompact.error().code == Domain::ErrorCodes::PayloadTooLarge);
}

void ownsSseBootstrapCommentAndCapacityOneSubscription()
{
    SubscriptionCounters counters;
    {
        auto subscription = std::make_unique<LatestSubscription>(counters);
        auto* const concreteSubscription = subscription.get();
        auto bootstrap =
            Dashboard::DashboardHttpResponseEncoder::encodeSseBootstrap();
        REQUIRE(bootstrap.hasValue());
        const auto expectedHead = bootstrap.bytes();

        auto prepared = Dashboard::DashboardPreparedExchange::createSse(
            std::move(bootstrap), std::move(subscription));
        REQUIRE(prepared.hasValue());
        auto firstOwner = std::move(prepared).value();
        auto owner = std::move(firstOwner);
        REQUIRE(
            firstOwner.kind() ==
            Dashboard::DashboardPreparedExchange::Kind::Empty);
        REQUIRE(firstOwner.completeExchange() == nullptr);
        REQUIRE(firstOwner.sseExchange() == nullptr);
        REQUIRE(
            owner.kind() ==
            Dashboard::DashboardPreparedExchange::Kind::ServerSentEvents);
        REQUIRE(owner.completeExchange() == nullptr);
        REQUIRE(owner.sseExchange() != nullptr);
        REQUIRE(
            owner.sseExchange()->encodedHead().kind() ==
            Dashboard::DashboardHttpEncodingResult::Kind::SseBootstrapHead);
        REQUIRE(owner.sseExchange()->encodedHead().bytes() == expectedHead);
        REQUIRE(
            owner.sseExchange()->connectedCommentBytes() ==
            bytes(": connected realtime\n\n"));
        REQUIRE(
            Dashboard::DashboardSseExchange::ConnectedCommentText ==
            ": connected realtime\n\n");
        REQUIRE(owner.sseExchange()->subscription() == concreteSubscription);
        REQUIRE(owner.sseExchange()->subscription()->deliveryHz() == 2.0);

        const auto sink = std::make_shared<ReadySink>();
        owner.sseExchange()->subscription()->attachReadySink(sink);
        const auto first = frame(10U, "data: compact-10\n\n", "data: full-10\n\n");
        const auto second = frame(11U, "data: compact-11\n\n", "data: full-11\n\n");
        concreteSubscription->publish(first);
        REQUIRE(sink->signals() == 1U);
        REQUIRE(concreteSubscription->pendingCount() == 1U);
        concreteSubscription->publish(second);
        REQUIRE(sink->signals() == 1U);
        REQUIRE(concreteSubscription->pendingCount() == 1U);

        const auto latest = concreteSubscription->takeLatest();
        REQUIRE(latest == second);
        REQUIRE(latest->sourceSequence() == 11U);
        REQUIRE(concreteSubscription->pendingCount() == 0U);

        concreteSubscription->publish(first);
        REQUIRE(sink->signals() == 2U);
        REQUIRE(concreteSubscription->pendingCount() == 1U);

        owner.sseExchange()->close();
        REQUIRE(owner.sseExchange()->subscription() == nullptr);
        REQUIRE(counters.closeCalls == 1U);
        REQUIRE(counters.destructions == 1U);
    }
    REQUIRE(counters.closeCalls == 1U);
    REQUIRE(counters.destructions == 1U);
}

void rejectsInvalidSseExchangeInputsAndClosesOwnedSubscriptions()
{
    auto nullSubscription = Dashboard::DashboardPreparedExchange::createSse(
        Dashboard::DashboardHttpResponseEncoder::encodeSseBootstrap(), {});
    REQUIRE(!nullSubscription.hasValue());
    REQUIRE(
        nullSubscription.error().code == Domain::ErrorCodes::IntegrityFailure);

    SubscriptionCounters wrongKindCounters;
    auto wrongKind = Dashboard::DashboardPreparedExchange::createSse(
        completeEncoding(),
        std::make_unique<LatestSubscription>(wrongKindCounters));
    REQUIRE(!wrongKind.hasValue());
    REQUIRE(wrongKind.error().code == Domain::ErrorCodes::IntegrityFailure);
    REQUIRE(wrongKindCounters.closeCalls == 1U);
    REQUIRE(wrongKindCounters.destructions == 1U);

    SubscriptionCounters failedHeadCounters;
    auto failedHead = Dashboard::DashboardPreparedExchange::createSse(
        failedEncoding(),
        std::make_unique<LatestSubscription>(failedHeadCounters));
    REQUIRE(!failedHead.hasValue());
    REQUIRE(failedHead.error().code == Domain::ErrorCodes::IntegrityFailure);
    REQUIRE(failedHeadCounters.closeCalls == 1U);
    REQUIRE(failedHeadCounters.destructions == 1U);

    const std::vector<double> invalidDeliveryRates{
        0.0,
        0.999,
        2.001,
        std::numeric_limits<double>::infinity(),
        -std::numeric_limits<double>::infinity(),
        std::numeric_limits<double>::quiet_NaN(),
    };
    for (const auto deliveryHz : invalidDeliveryRates) {
        SubscriptionCounters invalidRateCounters;
        auto invalidRate = Dashboard::DashboardPreparedExchange::createSse(
            Dashboard::DashboardHttpResponseEncoder::encodeSseBootstrap(),
            std::make_unique<LatestSubscription>(
                invalidRateCounters,
                deliveryHz));
        REQUIRE(!invalidRate.hasValue());
        REQUIRE(
            invalidRate.error().code == Domain::ErrorCodes::IntegrityFailure);
        REQUIRE(invalidRateCounters.closeCalls == 1U);
        REQUIRE(invalidRateCounters.destructions == 1U);
    }
}

void emptiesMoveSourcesAndClosesAcrossAlternativeAssignment()
{
    SubscriptionCounters counters;
    auto sseResult = Dashboard::DashboardPreparedExchange::createSse(
        Dashboard::DashboardHttpResponseEncoder::encodeSseBootstrap(),
        std::make_unique<LatestSubscription>(counters));
    REQUIRE(sseResult.hasValue());
    auto sseSource = std::move(sseResult).value();

    auto completeResult = Dashboard::DashboardPreparedExchange::createComplete(
        completeEncoding(), Dashboard::DashboardPostDeliveryAction::None);
    REQUIRE(completeResult.hasValue());
    auto target = std::move(completeResult).value();

    target = std::move(sseSource);
    REQUIRE(
        sseSource.kind() ==
        Dashboard::DashboardPreparedExchange::Kind::Empty);
    REQUIRE(sseSource.completeExchange() == nullptr);
    REQUIRE(sseSource.sseExchange() == nullptr);
    REQUIRE(
        target.kind() ==
        Dashboard::DashboardPreparedExchange::Kind::ServerSentEvents);
    REQUIRE(target.completeExchange() == nullptr);
    REQUIRE(target.sseExchange() != nullptr);
    REQUIRE(target.sseExchange()->subscription() != nullptr);
    REQUIRE(counters.closeCalls == 0U);
    REQUIRE(counters.destructions == 0U);

    auto replacementResult =
        Dashboard::DashboardPreparedExchange::createComplete(
            completeEncoding(),
            Dashboard::DashboardPostDeliveryAction::None);
    REQUIRE(replacementResult.hasValue());
    auto completeSource = std::move(replacementResult).value();

    target = std::move(completeSource);
    REQUIRE(
        completeSource.kind() ==
        Dashboard::DashboardPreparedExchange::Kind::Empty);
    REQUIRE(completeSource.completeExchange() == nullptr);
    REQUIRE(completeSource.sseExchange() == nullptr);
    REQUIRE(
        target.kind() ==
        Dashboard::DashboardPreparedExchange::Kind::Complete);
    REQUIRE(target.completeExchange() != nullptr);
    REQUIRE(target.sseExchange() == nullptr);
    REQUIRE(counters.closeCalls == 1U);
    REQUIRE(counters.destructions == 1U);
}

[[nodiscard]] Domain::OperationContext context(
    const std::string_view operationId,
    const std::string_view correlationId)
{
    auto operation = Domain::OperationId::parse(operationId);
    auto correlation = Domain::CorrelationId::parse(correlationId);
    REQUIRE(operation.hasValue());
    REQUIRE(correlation.hasValue());
    return Domain::OperationContext{
        std::move(operation).value(),
        std::chrono::steady_clock::now() + std::chrono::seconds{5},
        std::stop_token{},
        std::move(correlation).value()};
}

class ConnectionApplication final
    : public Dashboard::IDashboardConnectionApplication {
public:
    [[nodiscard]] Domain::Result<Dashboard::DashboardPreparedExchange> prepare(
        Dashboard::DashboardHttpRequest request,
        const bool operationalServiceActive,
        Domain::OperationContext operationContext) noexcept override
    {
        try {
            preparedTarget_ = request.target();
            preparedOperationalState_ = operationalServiceActive;
            prepareOperationId_ = operationContext.operationId.value();
            return Dashboard::DashboardPreparedExchange::createComplete(
                completeEncoding(),
                Dashboard::DashboardPostDeliveryAction::None);
        } catch (...) {
            return Domain::Result<Dashboard::DashboardPreparedExchange>::failure(
                Domain::makeError(
                    Domain::ErrorCodes::InternalFailure,
                    "Connection application test fake failed."));
        }
    }

    [[nodiscard]] Domain::Result<void> executePostDelivery(
        const Dashboard::DashboardPostDeliveryAction action,
        Domain::OperationContext operationContext) noexcept override
    {
        try {
            executedAction_ = action;
            postDeliveryOperationId_ = operationContext.operationId.value();
            ++postDeliveryCalls_;
            return Domain::Result<void>::success();
        } catch (...) {
            return Domain::Result<void>::failure(Domain::makeError(
                Domain::ErrorCodes::InternalFailure,
                "Post-delivery test fake failed."));
        }
    }

    [[nodiscard]] const std::string& preparedTarget() const noexcept
    {
        return preparedTarget_;
    }
    [[nodiscard]] bool preparedOperationalState() const noexcept
    {
        return preparedOperationalState_;
    }
    [[nodiscard]] const std::string& prepareOperationId() const noexcept
    {
        return prepareOperationId_;
    }
    [[nodiscard]] const std::string& postDeliveryOperationId() const noexcept
    {
        return postDeliveryOperationId_;
    }
    [[nodiscard]] Dashboard::DashboardPostDeliveryAction executedAction()
        const noexcept
    {
        return executedAction_;
    }
    [[nodiscard]] std::size_t postDeliveryCalls() const noexcept
    {
        return postDeliveryCalls_;
    }

private:
    std::string preparedTarget_;
    std::string prepareOperationId_;
    std::string postDeliveryOperationId_;
    bool preparedOperationalState_{};
    Dashboard::DashboardPostDeliveryAction executedAction_{
        Dashboard::DashboardPostDeliveryAction::None};
    std::size_t postDeliveryCalls_{};
};

void preservesOwnedRequestAndFreshPostDeliveryContextBoundary()
{
    constexpr std::string_view PrepareOperation =
        "10000000-0000-4000-8000-000000000001";
    constexpr std::string_view PostDeliveryOperation =
        "10000000-0000-4000-8000-000000000002";

    ConnectionApplication application;
    auto prepared = application.prepare(
        Dashboard::DashboardHttpRequest{
            "POST", "/api/manager/shutdown", {}, bytes("{}")},
        false,
        context(PrepareOperation, "prepare-correlation"));
    REQUIRE(prepared.hasValue());
    REQUIRE(application.preparedTarget() == "/api/manager/shutdown");
    REQUIRE(!application.preparedOperationalState());
    REQUIRE(application.prepareOperationId() == PrepareOperation);

    auto executed = application.executePostDelivery(
        Dashboard::DashboardPostDeliveryAction::RequestManagerShutdown,
        context(PostDeliveryOperation, "post-delivery-correlation"));
    REQUIRE(executed.hasValue());
    REQUIRE(application.postDeliveryCalls() == 1U);
    REQUIRE(
        application.executedAction() ==
        Dashboard::DashboardPostDeliveryAction::RequestManagerShutdown);
    REQUIRE(application.postDeliveryOperationId() == PostDeliveryOperation);
    REQUIRE(
        application.prepareOperationId() !=
        application.postDeliveryOperationId());
}

} // namespace

int main()
{
    try {
        ownsExactCompleteAndHeadEncodingsAndActions();
        rejectsInvalidCompleteExchangeInputs();
        transfersDirectCompleteExchangeActionsExactlyOnce();
        validatesImmutableSharedFramePairs();
        ownsSseBootstrapCommentAndCapacityOneSubscription();
        rejectsInvalidSseExchangeInputsAndClosesOwnedSubscriptions();
        emptiesMoveSourcesAndClosesAcrossAlternativeAssignment();
        preservesOwnedRequestAndFreshPostDeliveryContextBoundary();
        std::cout << "Dashboard prepared exchange tests passed (" << assertions
                  << " assertions).\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Dashboard prepared exchange tests failed: "
                  << error.what() << '\n';
        return 1;
    } catch (...) {
        std::cerr << "Dashboard prepared exchange tests failed with an "
                     "unknown error.\n";
        return 1;
    }
}
