#include "ForgeConductor/Dashboard/DashboardSseBroadcaster.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <limits>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
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

static_assert(std::is_final_v<Dashboard::DashboardSseBroadcaster>);
static_assert(std::is_final_v<Dashboard::DashboardSseSubscription>);
static_assert(std::is_base_of_v<
              Dashboard::IDashboardSseSubscription,
              Dashboard::DashboardSseSubscription>);
static_assert(!std::is_copy_constructible_v<
              Dashboard::DashboardSseBroadcaster>);
static_assert(!std::is_copy_assignable_v<
              Dashboard::DashboardSseBroadcaster>);
static_assert(std::is_nothrow_move_constructible_v<
              Dashboard::DashboardSseBroadcaster>);
static_assert(std::is_nothrow_move_assignable_v<
              Dashboard::DashboardSseBroadcaster>);
static_assert(!std::is_copy_constructible_v<
              Dashboard::DashboardSseSubscription>);
static_assert(!std::is_move_constructible_v<
              Dashboard::DashboardSseSubscription>);
static_assert(
    Dashboard::DashboardSseBroadcaster::HardMaximumSubscriptions == 32U);
static_assert(noexcept(Dashboard::DashboardSseBroadcaster::create()));
static_assert(noexcept(
    std::declval<Dashboard::DashboardSseBroadcaster&>().subscribe(
        std::declval<Dashboard::DashboardSseFramePair::ImmutableFrame>(),
        2.0)));
static_assert(noexcept(
    std::declval<Dashboard::DashboardSseBroadcaster&>().publish(
        std::declval<Dashboard::DashboardSseFramePair::ImmutableFrame>())));
static_assert(noexcept(
    std::declval<Dashboard::DashboardSseBroadcaster&>().shutdown()));
static_assert(noexcept(
    std::declval<Dashboard::DashboardSseDeliveryCursor&>().select(
        std::declval<const Dashboard::DashboardSseFramePair::ImmutableFrame&>())));

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

[[nodiscard]] Dashboard::DashboardSseFramePair::ImmutableFrame frame(
    const std::uint64_t sequence)
{
    const auto compactText =
        "data: compact-" + std::to_string(sequence) + "\n\n";
    const auto fullText =
        "data: full-" + std::to_string(sequence) + "\n\n";
    auto created = Dashboard::DashboardSseFramePair::create(
        sequence,
        bytes(compactText),
        bytes(fullText));
    REQUIRE(created.hasValue());
    return std::move(created).value();
}

[[nodiscard]] Dashboard::DashboardSseBroadcaster broadcaster(
    const std::size_t maximum =
        Dashboard::DashboardSseBroadcaster::HardMaximumSubscriptions)
{
    auto created = Dashboard::DashboardSseBroadcaster::create(maximum);
    REQUIRE(created.hasValue());
    return std::move(created).value();
}

[[nodiscard]] std::unique_ptr<Dashboard::DashboardSseSubscription> subscribe(
    Dashboard::DashboardSseBroadcaster& owner,
    const double deliveryHz = 2.0,
    const std::uint64_t initialSequence = 0U)
{
    auto result = owner.subscribe(frame(initialSequence), deliveryHz);
    REQUIRE(result.hasValue());
    auto subscription = std::move(result).value();
    REQUIRE(subscription != nullptr);
    return subscription;
}

class CountingSink final : public Dashboard::IDashboardSseReadySink {
public:
    void signal() noexcept override
    {
        signals_.fetch_add(1U, std::memory_order_relaxed);
    }

    [[nodiscard]] std::size_t signals() const noexcept
    {
        return signals_.load(std::memory_order_relaxed);
    }

private:
    std::atomic<std::size_t> signals_{};
};

class BlockingSink final : public Dashboard::IDashboardSseReadySink {
public:
    void signal() noexcept override
    {
        try {
            std::unique_lock lock{mutex_};
            entered_ = true;
            enteredCondition_.notify_all();
            releaseCondition_.wait(lock, [this] { return released_; });
        } catch (...) {
            failed_.store(true, std::memory_order_relaxed);
        }
    }

    [[nodiscard]] bool waitUntilEntered(
        const std::chrono::milliseconds timeout) noexcept
    {
        try {
            std::unique_lock lock{mutex_};
            return enteredCondition_.wait_for(
                lock, timeout, [this] { return entered_; });
        } catch (...) {
            return false;
        }
    }

    void release() noexcept
    {
        try {
            std::lock_guard lock{mutex_};
            released_ = true;
            releaseCondition_.notify_all();
        } catch (...) {
            failed_.store(true, std::memory_order_relaxed);
        }
    }

    [[nodiscard]] bool failed() const noexcept
    {
        return failed_.load(std::memory_order_relaxed);
    }

private:
    mutable std::mutex mutex_;
    std::condition_variable enteredCondition_;
    std::condition_variable releaseCondition_;
    std::atomic<bool> failed_{};
    bool entered_{};
    bool released_{};
};

void validatesConfiguredHardCapacity()
{
    auto zero = Dashboard::DashboardSseBroadcaster::create(0U);
    REQUIRE(!zero.hasValue());
    REQUIRE(zero.error().code == Domain::ErrorCodes::InvalidRequest);

    auto over = Dashboard::DashboardSseBroadcaster::create(
        Dashboard::DashboardSseBroadcaster::HardMaximumSubscriptions + 1U);
    REQUIRE(!over.hasValue());
    REQUIRE(over.error().code == Domain::ErrorCodes::InvalidRequest);

    auto downward = broadcaster(2U);
    REQUIRE(downward.maximumSubscriptionCount() == 2U);
    auto first = subscribe(downward);
    auto second = subscribe(downward);
    REQUIRE(downward.liveSubscriptionCount() == 2U);
    auto third = downward.subscribe(frame(3U), 2.0);
    REQUIRE(!third.hasValue());
    REQUIRE(third.error().code == Domain::ErrorCodes::LimitExceeded);
    REQUIRE(third.error().retryable);
    first->close();
    REQUIRE(downward.liveSubscriptionCount() == 1U);
    auto replacement = subscribe(downward);
    REQUIRE(downward.liveSubscriptionCount() == 2U);

    auto maximum = broadcaster();
    std::vector<std::unique_ptr<Dashboard::DashboardSseSubscription>> streams;
    streams.reserve(Dashboard::DashboardSseBroadcaster::HardMaximumSubscriptions);
    for (std::size_t index{};
         index < Dashboard::DashboardSseBroadcaster::HardMaximumSubscriptions;
         ++index) {
        streams.push_back(subscribe(maximum));
    }
    REQUIRE(
        maximum.liveSubscriptionCount() ==
        Dashboard::DashboardSseBroadcaster::HardMaximumSubscriptions);
    auto thirtyThird = maximum.subscribe(frame(33U), 2.0);
    REQUIRE(!thirtyThird.hasValue());
    REQUIRE(
        thirtyThird.error().code == Domain::ErrorCodes::LimitExceeded);
    streams.back().reset();
    REQUIRE(
        maximum.liveSubscriptionCount() ==
        Dashboard::DashboardSseBroadcaster::HardMaximumSubscriptions - 1U);
    streams.back() = subscribe(maximum);
    REQUIRE(
        maximum.liveSubscriptionCount() ==
        Dashboard::DashboardSseBroadcaster::HardMaximumSubscriptions);
}

void validatesInitialFrameAndDeliveryRate()
{
    auto owner = broadcaster(4U);
    auto nullInitial = owner.subscribe({}, 1.0);
    REQUIRE(!nullInitial.hasValue());
    REQUIRE(nullInitial.error().code == Domain::ErrorCodes::IntegrityFailure);

    const std::vector<double> invalidRates{
        0.0,
        0.999,
        2.001,
        3.0,
        std::numeric_limits<double>::infinity(),
        -std::numeric_limits<double>::infinity(),
        std::numeric_limits<double>::quiet_NaN(),
    };
    for (const auto rate : invalidRates) {
        auto invalid = owner.subscribe(frame(100U), rate);
        REQUIRE(!invalid.hasValue());
        REQUIRE(invalid.error().code == Domain::ErrorCodes::InvalidRequest);
    }
    REQUIRE(owner.liveSubscriptionCount() == 0U);

    auto oneHz = subscribe(owner, 1.0, 101U);
    auto twoHz = subscribe(owner, 2.0, 102U);
    auto fractional = subscribe(owner, 1.375, 103U);
    REQUIRE(oneHz->deliveryHz() == 1.0);
    REQUIRE(twoHz->deliveryHz() == 2.0);
    REQUIRE(fractional->deliveryHz() == 1.375);
    REQUIRE(
        static_cast<const Dashboard::IDashboardSseSubscription&>(*fractional)
            .deliveryHz() == 1.375);
    REQUIRE(oneHz->pendingCount() == 1U);
    REQUIRE(twoHz->pendingCount() == 1U);
    REQUIRE(fractional->pendingCount() == 1U);
    REQUIRE(oneHz->takeLatest()->sourceSequence() == 101U);
    REQUIRE(twoHz->takeLatest()->sourceSequence() == 102U);
    REQUIRE(fractional->takeLatest()->sourceSequence() == 103U);
}

void replacesPendingFramesAndSignalsOnlyReadyEdges()
{
    auto owner = broadcaster(1U);
    auto subscription = subscribe(owner);
    const auto sink = std::make_shared<CountingSink>();
    subscription->attachReadySink(sink);
    REQUIRE(sink->signals() == 1U);
    subscription->attachReadySink(sink);
    REQUIRE(sink->signals() == 1U);
    REQUIRE(subscription->pendingCount() == 1U);

    const auto first = frame(1U);
    const auto second = frame(2U);
    REQUIRE(owner.publish(first).hasValue());
    REQUIRE(sink->signals() == 1U);
    REQUIRE(subscription->pendingCount() == 1U);
    REQUIRE(owner.publish(second).hasValue());
    REQUIRE(sink->signals() == 1U);
    REQUIRE(subscription->pendingCount() == 1U);

    Dashboard::DashboardSseDeliveryCursor cursor;
    const auto deliveredFrame = subscription->takeLatest();
    REQUIRE(deliveredFrame == second);
    REQUIRE(deliveredFrame->sourceSequence() == 2U);
    REQUIRE(cursor.select(deliveredFrame) == second->compactBytes());
    REQUIRE(subscription->pendingCount() == 0U);
    REQUIRE(subscription->takeLatest() == nullptr);

    REQUIRE(owner.publish(first).hasValue());
    REQUIRE(sink->signals() == 2U);
    REQUIRE(subscription->pendingCount() == 1U);
    const auto viaInterface =
        static_cast<Dashboard::IDashboardSseSubscription&>(*subscription)
            .takeLatest();
    REQUIRE(viaInterface == first);
    REQUIRE(subscription->pendingCount() == 0U);

    subscription->close();
    subscription->close();
    REQUIRE(subscription->isClosed());
    REQUIRE(subscription->pendingCount() == 0U);
    REQUIRE(subscription->takeLatest() == nullptr);
    REQUIRE(owner.liveSubscriptionCount() == 0U);
    REQUIRE(owner.publish(second).hasValue());
    REQUIRE(sink->signals() == 2U);
}

void fansOutTheSameImmutableFrameToEveryLiveSubscription()
{
    auto owner = broadcaster(3U);
    auto first = subscribe(owner);
    auto second = subscribe(owner);
    auto third = subscribe(owner);
    const auto sharedFrame = frame(5U);
    REQUIRE(owner.publish(sharedFrame).hasValue());
    REQUIRE(first->takeLatest() == sharedFrame);
    REQUIRE(second->takeLatest() == sharedFrame);
    REQUIRE(third->takeLatest() == sharedFrame);

    second->close();
    const auto nextFrame = frame(6U);
    REQUIRE(owner.publish(nextFrame).hasValue());
    REQUIRE(first->takeLatest() == nextFrame);
    REQUIRE(second->takeLatest() == nullptr);
    REQUIRE(third->takeLatest() == nextFrame);
    REQUIRE(owner.liveSubscriptionCount() == 2U);
}

void toleratesExpiredAndReplacedWeakReadySinks()
{
    auto owner = broadcaster(1U);
    auto subscription = subscribe(owner);

    std::weak_ptr<CountingSink> expired;
    {
        auto sink = std::make_shared<CountingSink>();
        expired = sink;
        subscription->attachReadySink(sink);
    }
    REQUIRE(expired.expired());
    REQUIRE(owner.publish(frame(10U)).hasValue());
    REQUIRE(subscription->pendingCount() == 1U);

    const auto replacementSink = std::make_shared<CountingSink>();
    subscription->attachReadySink(replacementSink);
    REQUIRE(replacementSink->signals() == 1U);
    REQUIRE(owner.publish(frame(11U)).hasValue());
    REQUIRE(replacementSink->signals() == 1U);
    const auto latest = subscription->takeLatest();
    REQUIRE(latest != nullptr);
    REQUIRE(latest->sourceSequence() == 11U);

    REQUIRE(owner.publish(frame(12U)).hasValue());
    REQUIRE(replacementSink->signals() == 2U);
    subscription->attachReadySink({});
    REQUIRE(subscription->takeLatest() != nullptr);
    REQUIRE(owner.publish(frame(13U)).hasValue());
    REQUIRE(replacementSink->signals() == 2U);
}

void selectsFullBytesEveryTenthActualDelivery()
{
    auto owner = broadcaster(1U);
    const auto initial = frame(1U);
    auto subscriptionResult = owner.subscribe(initial, 1.5);
    REQUIRE(subscriptionResult.hasValue());
    auto subscription = std::move(subscriptionResult).value();
    Dashboard::DashboardSseDeliveryCursor cursor;
    const auto initialDelivery = subscription->takeLatest();
    REQUIRE(initialDelivery == initial);
    REQUIRE(cursor.select(initialDelivery) == initial->compactBytes());
    REQUIRE(subscription->takeLatest() == nullptr);
    REQUIRE(cursor.select({}) == nullptr);

    for (std::uint64_t delivered = 2U; delivered <= 25U; ++delivered) {
        auto expected = frame(delivered);
        REQUIRE(owner.publish(expected).hasValue());

        if (delivered == 20U) {
            expected = frame(2'000U);
            REQUIRE(owner.publish(expected).hasValue());
        }

        const auto viaInterface =
            static_cast<Dashboard::IDashboardSseSubscription&>(*subscription)
                .takeLatest();
        REQUIRE(viaInterface == expected);
        const bool shouldUseFull = delivered % 10U == 0U;
        REQUIRE(
            cursor.select(viaInterface) ==
            (shouldUseFull ? expected->fullBytes() : expected->compactBytes()));
    }
    REQUIRE(subscription->pendingCount() == 0U);

    Dashboard::DashboardSseDeliveryCursor firstConnection;
    Dashboard::DashboardSseDeliveryCursor secondConnection;
    for (std::size_t delivery{1U}; delivery <= 9U; ++delivery) {
        const auto selectedFrame = frame(3'000U + delivery);
        REQUIRE(
            firstConnection.select(selectedFrame) ==
            selectedFrame->compactBytes());
    }
    for (std::size_t delivery{1U}; delivery <= 3U; ++delivery) {
        const auto selectedFrame = frame(4'000U + delivery);
        REQUIRE(
            secondConnection.select(selectedFrame) ==
            selectedFrame->compactBytes());
    }
    REQUIRE(firstConnection.select({}) == nullptr);
    const auto firstTenth = frame(3'010U);
    REQUIRE(firstConnection.select(firstTenth) == firstTenth->fullBytes());
    const auto secondFourth = frame(4'004U);
    REQUIRE(
        secondConnection.select(secondFourth) ==
        secondFourth->compactBytes());
    for (std::size_t delivery{5U}; delivery <= 9U; ++delivery) {
        const auto selectedFrame = frame(4'000U + delivery);
        REQUIRE(
            secondConnection.select(selectedFrame) ==
            selectedFrame->compactBytes());
    }
    const auto secondTenth = frame(4'010U);
    REQUIRE(secondConnection.select(secondTenth) == secondTenth->fullBytes());
}

void preservesSubscriptionStateAcrossBroadcasterMovesAndDestruction()
{
    auto source = broadcaster(2U);
    auto subscription = subscribe(source, 1.625, 29U);
    auto movedSubscription = std::move(subscription);
    REQUIRE(subscription == nullptr);
    REQUIRE(movedSubscription != nullptr);

    Dashboard::DashboardSseBroadcaster moved{std::move(source)};
    REQUIRE(source.isShutdown());
    REQUIRE(source.maximumSubscriptionCount() == 0U);
    REQUIRE(moved.liveSubscriptionCount() == 1U);
    REQUIRE(movedSubscription->deliveryHz() == 1.625);
    REQUIRE(moved.publish(frame(30U)).hasValue());
    REQUIRE(movedSubscription->takeLatest()->sourceSequence() == 30U);

    auto target = broadcaster(1U);
    auto displacedSubscription = subscribe(target);
    target = std::move(moved);
    REQUIRE(moved.isShutdown());
    REQUIRE(displacedSubscription->isClosed());
    REQUIRE(target.liveSubscriptionCount() == 1U);
    REQUIRE(movedSubscription->deliveryHz() == 1.625);
    REQUIRE(target.publish(frame(31U)).hasValue());
    REQUIRE(movedSubscription->takeLatest()->sourceSequence() == 31U);

    std::unique_ptr<Dashboard::DashboardSseSubscription> survivor;
    {
        auto scopedOwner = broadcaster(1U);
        survivor = subscribe(scopedOwner);
        REQUIRE(scopedOwner.publish(frame(32U)).hasValue());
        REQUIRE(survivor->pendingCount() == 1U);
    }
    REQUIRE(survivor->isClosed());
    REQUIRE(survivor->deliveryHz() == 2.0);
    REQUIRE(survivor->pendingCount() == 0U);
    REQUIRE(survivor->takeLatest() == nullptr);
    survivor->close();
}

void shutdownDoesNotWaitForAnInFlightReadySignal()
{
    auto owner = broadcaster(1U);
    auto subscription = subscribe(owner);
    REQUIRE(subscription->takeLatest() != nullptr);
    const auto sink = std::make_shared<BlockingSink>();
    subscription->attachReadySink(sink);
    const auto publishedFrame = frame(40U);
    std::atomic<bool> publishReturned{};
    std::atomic<bool> publishSucceeded{};

    std::thread publisher{[&] {
        const auto result = owner.publish(publishedFrame);
        publishSucceeded.store(result.hasValue(), std::memory_order_release);
        publishReturned.store(true, std::memory_order_release);
    }};

    const bool entered = sink->waitUntilEntered(std::chrono::seconds{2});
    if (!entered) {
        sink->release();
        publisher.join();
    }
    REQUIRE(entered);

    const auto started = std::chrono::steady_clock::now();
    owner.shutdown();
    const auto elapsed = std::chrono::steady_clock::now() - started;
    REQUIRE(elapsed < std::chrono::seconds{1});
    REQUIRE(owner.isShutdown());
    REQUIRE(owner.liveSubscriptionCount() == 0U);
    REQUIRE(subscription->isClosed());
    REQUIRE(subscription->pendingCount() == 0U);
    REQUIRE(!publishReturned.load(std::memory_order_acquire));

    sink->release();
    publisher.join();
    REQUIRE(publishReturned.load(std::memory_order_acquire));
    REQUIRE(publishSucceeded.load(std::memory_order_acquire));
    REQUIRE(!sink->failed());

    auto afterShutdown = owner.publish(frame(41U));
    REQUIRE(!afterShutdown.hasValue());
    REQUIRE(afterShutdown.error().code == Domain::ErrorCodes::TransportClosed);
    auto subscribeAfterShutdown = owner.subscribe(frame(42U), 2.0);
    REQUIRE(!subscribeAfterShutdown.hasValue());
    REQUIRE(
        subscribeAfterShutdown.error().code ==
        Domain::ErrorCodes::TransportClosed);
    owner.shutdown();
}

void survivesConcurrentPublishTakeCloseAndShutdownRaces()
{
    constexpr std::size_t SubscriptionCount = 8U;
    constexpr std::size_t PublisherCount = 4U;
    constexpr std::size_t Iterations = 2'000U;

    auto owner = broadcaster(SubscriptionCount);
    std::vector<std::unique_ptr<Dashboard::DashboardSseSubscription>> streams;
    streams.reserve(SubscriptionCount);
    std::vector<std::shared_ptr<CountingSink>> sinks;
    sinks.reserve(SubscriptionCount);
    for (std::size_t index{}; index < SubscriptionCount; ++index) {
        auto stream = subscribe(owner);
        auto sink = std::make_shared<CountingSink>();
        stream->attachReadySink(sink);
        streams.push_back(std::move(stream));
        sinks.push_back(std::move(sink));
    }

    std::vector<Dashboard::DashboardSseFramePair::ImmutableFrame> frames;
    frames.reserve(32U);
    for (std::uint64_t sequence{1U}; sequence <= 32U; ++sequence) {
        frames.push_back(frame(1'000U + sequence));
    }

    std::atomic<bool> start{};
    std::atomic<bool> invalidPendingCount{};
    std::atomic<std::size_t> publishFailures{};
    std::vector<std::thread> workers;
    workers.reserve(PublisherCount + SubscriptionCount + 1U);
    for (std::size_t publisherIndex{};
         publisherIndex < PublisherCount;
         ++publisherIndex) {
        workers.emplace_back([&, publisherIndex] {
            while (!start.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
            for (std::size_t iteration{}; iteration < Iterations; ++iteration) {
                const auto& selected =
                    frames[(publisherIndex + iteration) % frames.size()];
                if (!owner.publish(selected).hasValue()) {
                    publishFailures.fetch_add(1U, std::memory_order_relaxed);
                }
            }
        });
    }
    for (std::size_t streamIndex{};
         streamIndex < SubscriptionCount;
         ++streamIndex) {
        workers.emplace_back([&, streamIndex] {
            while (!start.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
            for (std::size_t iteration{}; iteration < Iterations; ++iteration) {
                if (streams[streamIndex]->pendingCount() > 1U) {
                    invalidPendingCount.store(true, std::memory_order_relaxed);
                }
                (void)streams[streamIndex]->takeLatest();
            }
        });
    }
    workers.emplace_back([&] {
        while (!start.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
        for (std::size_t pass{}; pass < 64U; ++pass) {
            for (std::size_t index{}; index < SubscriptionCount; index += 2U) {
                streams[index]->close();
            }
            std::this_thread::yield();
        }
    });

    start.store(true, std::memory_order_release);
    for (auto& worker : workers) {
        worker.join();
    }
    REQUIRE(!invalidPendingCount.load(std::memory_order_relaxed));
    REQUIRE(publishFailures.load(std::memory_order_relaxed) == 0U);
    for (const auto& stream : streams) {
        REQUIRE(stream->pendingCount() <= 1U);
        stream->close();
    }
    REQUIRE(owner.liveSubscriptionCount() == 0U);

    auto invalidFrame = owner.publish({});
    REQUIRE(!invalidFrame.hasValue());
    REQUIRE(invalidFrame.error().code == Domain::ErrorCodes::IntegrityFailure);
    owner.shutdown();
    REQUIRE(owner.isShutdown());
}

} // namespace

int main()
{
    try {
        validatesConfiguredHardCapacity();
        validatesInitialFrameAndDeliveryRate();
        replacesPendingFramesAndSignalsOnlyReadyEdges();
        fansOutTheSameImmutableFrameToEveryLiveSubscription();
        toleratesExpiredAndReplacedWeakReadySinks();
        selectsFullBytesEveryTenthActualDelivery();
        preservesSubscriptionStateAcrossBroadcasterMovesAndDestruction();
        shutdownDoesNotWaitForAnInFlightReadySignal();
        survivesConcurrentPublishTakeCloseAndShutdownRaces();
        std::cout << "Dashboard SSE broadcaster tests passed (" << assertions
                  << " assertions).\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Dashboard SSE broadcaster tests failed: " << error.what()
                  << '\n';
        return 1;
    } catch (...) {
        std::cerr << "Dashboard SSE broadcaster tests failed with an unknown "
                     "error.\n";
        return 1;
    }
}
