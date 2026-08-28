#include "Infrastructure/Windows/Detail/DashboardIocpCompletionRouter.h"

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <iostream>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <utility>

namespace {

namespace Detail = ForgeConductor::Infrastructure::Windows::Detail;
namespace Domain = ForgeConductor::Domain;

using FixedTarget = Detail::IDashboardFixedIocpCompletionTarget;
using Key = Detail::DashboardIoCompletionKey;
using Packet = Detail::DashboardIoCompletionPacket;
using Router = Detail::DashboardIocpCompletionRouter;
using Sink = Detail::IDashboardIocpCompletionSink;

constexpr Key FallbackOwnedKey{900U};

static_assert(std::is_abstract_v<FixedTarget>);
static_assert(std::is_final_v<Router>);
static_assert(std::is_base_of_v<Sink, Router>);
static_assert(Router::MaximumFixedTargetCount == 3U);

std::size_t assertions{};

void require(const bool condition, const std::string_view message)
{
    ++assertions;
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

class RecordingSink final : public Sink {
public:
    void consume(
        const Packet packet,
        const DWORD nativeError) noexcept override
    {
        const std::scoped_lock lock{mutex_};
        packet_ = packet;
        nativeError_ = nativeError;
        ++consumeCalls_;
    }

    void fatal(const DWORD nativeError) noexcept override
    {
        const std::scoped_lock lock{mutex_};
        fatalError_ = nativeError;
        ++fatalCalls_;
    }

    [[nodiscard]] std::size_t consumeCalls() const noexcept
    {
        const std::scoped_lock lock{mutex_};
        return consumeCalls_;
    }

    [[nodiscard]] std::size_t fatalCalls() const noexcept
    {
        const std::scoped_lock lock{mutex_};
        return fatalCalls_;
    }

    [[nodiscard]] Packet packet() const noexcept
    {
        const std::scoped_lock lock{mutex_};
        return packet_;
    }

    [[nodiscard]] DWORD nativeError() const noexcept
    {
        const std::scoped_lock lock{mutex_};
        return nativeError_;
    }

    [[nodiscard]] DWORD fatalError() const noexcept
    {
        const std::scoped_lock lock{mutex_};
        return fatalError_;
    }

private:
    mutable std::mutex mutex_;
    Packet packet_{};
    DWORD nativeError_{};
    DWORD fatalError_{};
    std::size_t consumeCalls_{};
    std::size_t fatalCalls_{};
};

struct TargetState final {
    mutable std::mutex mutex;
    std::condition_variable changed;
    Packet packet{};
    DWORD nativeError{};
    DWORD fatalError{};
    std::size_t consumeCalls{};
    std::size_t fatalCalls{};
    std::size_t shutdownCalls{};
    bool blockConsume{};
    bool consumeEntered{};
    bool releaseConsume{};
    bool destroyed{};
};

class RecordingTarget final : public FixedTarget {
public:
    RecordingTarget(
        const Key key,
        std::shared_ptr<TargetState> state) noexcept
        : key_{key}, state_{std::move(state)}
    {
    }

    ~RecordingTarget() noexcept override
    {
        const std::scoped_lock lock{state_->mutex};
        state_->destroyed = true;
        state_->changed.notify_all();
    }

    [[nodiscard]] Key completionKey() const noexcept override
    {
        return key_;
    }

    void consume(
        const Packet packet,
        const DWORD nativeError) noexcept override
    {
        std::unique_lock lock{state_->mutex};
        state_->packet = packet;
        state_->nativeError = nativeError;
        ++state_->consumeCalls;
        state_->consumeEntered = true;
        state_->changed.notify_all();
        if (state_->blockConsume) {
            state_->changed.wait(
                lock, [this] { return state_->releaseConsume; });
        }
    }

    void fatal(const DWORD nativeError) noexcept override
    {
        const std::scoped_lock lock{state_->mutex};
        state_->fatalError = nativeError;
        ++state_->fatalCalls;
    }

    void beginShutdown() noexcept override
    {
        const std::scoped_lock lock{state_->mutex};
        ++state_->shutdownCalls;
    }

private:
    const Key key_;
    const std::shared_ptr<TargetState> state_;
};

[[nodiscard]] std::shared_ptr<RecordingTarget> target(
    const std::uintptr_t key,
    const std::shared_ptr<TargetState>& state =
        std::make_shared<TargetState>())
{
    return std::make_shared<RecordingTarget>(Key{key}, state);
}

void rejectsFallbackOwnedKeyBeforeFixedTableMutation()
{
    auto fallback = std::make_shared<RecordingSink>();
    const auto zero = Router::create(Key{0U}, fallback);
    require(!zero &&
                zero.error().code == Domain::ErrorCodes::InvalidRequest,
            "zero fallback-owned key was accepted");
    const auto shutdown = Router::create(
        Key{Detail::DashboardIocpWorkerKernel::ShutdownKeyValue},
        fallback);
    require(!shutdown &&
                shutdown.error().code ==
                    Domain::ErrorCodes::InvalidRequest,
            "kernel shutdown key was accepted as fallback-owned");

    auto router = take(Router::create(FallbackOwnedKey, fallback));
    require(router->fallbackOwnedCompletionKey() == FallbackOwnedKey,
            "router did not retain its immutable fallback-owned key");
    auto state = std::make_shared<TargetState>();
    auto collision = target(FallbackOwnedKey.value(), state);
    const auto rejected = router->registerFixedTarget(collision);
    require(!rejected &&
                rejected.error().code ==
                    Domain::ErrorCodes::InvalidRequest,
            "fixed target claimed the fallback-owned key");
    require(router->snapshot().fixedTargetCount() == 0U &&
                fallback->consumeCalls() == 0U &&
                fallback->fatalCalls() == 0U,
            "fallback-key rejection mutated router or fallback state");
    {
        const std::scoped_lock lock{state->mutex};
        require(state->consumeCalls == 0U && state->fatalCalls == 0U &&
                    state->shutdownCalls == 0U,
                "fallback-key rejection invoked the fixed target");
    }
    require(router->registerFixedTarget(target(11U)).hasValue() &&
                router->snapshot().fixedTargetCount() == 1U,
            "fallback-key rejection consumed a fixed-table slot");
}

void routesFixedAndFallbackKeysWithoutCollapsingPacketFields()
{
    auto fallback = std::make_shared<RecordingSink>();
    auto router = take(Router::create(FallbackOwnedKey, fallback));
    auto state = std::make_shared<TargetState>();
    auto fixed = target(11U, state);
    require(router->registerFixedTarget(fixed).hasValue(),
            "fixed target registration failed");

    OVERLAPPED fixedOperation{};
    const Packet fixedPacket{23U, Key{11U}, &fixedOperation};
    router->consume(fixedPacket, ERROR_OPERATION_ABORTED);
    {
        const std::scoped_lock lock{state->mutex};
        require(state->consumeCalls == 1U,
                "fixed packet was not delivered exactly once");
        require(state->packet == fixedPacket &&
                    state->nativeError == ERROR_OPERATION_ABORTED,
                "fixed packet fields changed during routing");
    }
    require(fallback->consumeCalls() == 0U,
            "fixed packet escaped to the fallback registry");

    OVERLAPPED dynamicOperation{};
    const Packet dynamicPacket{71U, Key{900U}, &dynamicOperation};
    router->consume(dynamicPacket, ERROR_SUCCESS);
    require(fallback->consumeCalls() == 1U,
            "dynamic packet did not reach the fallback registry");
    require(fallback->packet() == dynamicPacket &&
                fallback->nativeError() == ERROR_SUCCESS,
            "fallback packet fields changed during routing");

    const auto snapshot = router->snapshot();
    require(snapshot.fixedTargetCount() == 1U &&
                snapshot.fixedDispatchCount() == 1U &&
                snapshot.fallbackDispatchCount() == 1U,
            "router snapshot did not retain exact dispatch counts");
}

void enforcesTheActiveRetiringAndOverloadFixedTable()
{
    auto router = take(Router::create(
        FallbackOwnedKey, std::make_shared<RecordingSink>()));
    auto first = target(11U);
    auto second = target(12U);
    auto third = target(13U);
    require(router->registerFixedTarget(first).hasValue(),
            "active target registration failed");
    require(router->registerFixedTarget(second).hasValue(),
            "retiring target registration failed");
    require(router->registerFixedTarget(third).hasValue(),
            "overload target registration failed");

    const auto duplicate = router->registerFixedTarget(target(12U));
    require(!duplicate &&
                duplicate.error().code == Domain::ErrorCodes::Conflict,
            "duplicate fixed key was not rejected as a conflict");
    const auto overflow = router->registerFixedTarget(target(14U));
    require(!overflow &&
                overflow.error().code ==
                    Domain::ErrorCodes::LimitExceeded &&
                overflow.error().retryable,
            "fourth fixed target was not a retryable bounded limit");

    auto imposter = target(11U);
    require(!router->unregisterFixedTarget(imposter),
            "same-key imposter removed the active target");
    require(router->unregisterFixedTarget(second),
            "exact retiring owner did not unregister");
    require(router->registerFixedTarget(target(14U)).hasValue(),
            "drained fixed slot was not reusable");

    const auto invalidZero = router->registerFixedTarget(target(0U));
    require(!invalidZero && invalidZero.error().code ==
                Domain::ErrorCodes::InvalidRequest,
            "zero fixed key was accepted");
    const auto invalidShutdown = router->registerFixedTarget(target(
        Detail::DashboardIocpWorkerKernel::ShutdownKeyValue));
    require(!invalidShutdown && invalidShutdown.error().code ==
                Domain::ErrorCodes::InvalidRequest,
            "kernel shutdown key was accepted as a fixed target");
}

void unregistrationCannotDestroyADequeuedCallbackOwner()
{
    auto router = take(Router::create(
        FallbackOwnedKey, std::make_shared<RecordingSink>()));
    auto state = std::make_shared<TargetState>();
    state->blockConsume = true;
    auto fixed = target(21U, state);
    require(router->registerFixedTarget(fixed).hasValue(),
            "blocking fixed target registration failed");

    OVERLAPPED operation{};
    std::jthread dispatch{[&router, &operation] {
        router->consume(Packet{0U, Key{21U}, &operation}, ERROR_SUCCESS);
    }};
    {
        std::unique_lock lock{state->mutex};
        state->changed.wait(lock, [&state] { return state->consumeEntered; });
    }
    require(router->unregisterFixedTarget(fixed),
            "dequeued target could not unregister");
    fixed.reset();
    {
        const std::scoped_lock lock{state->mutex};
        require(!state->destroyed,
                "unregistration destroyed a callback owner still in use");
        state->releaseConsume = true;
        state->changed.notify_all();
    }
    dispatch.join();
    {
        const std::scoped_lock lock{state->mutex};
        require(state->destroyed,
                "dequeued callback pin outlived completion dispatch");
    }
}

void gracefulAndFatalShutdownRemainNonblockingAndIdempotent()
{
    auto fallback = std::make_shared<RecordingSink>();
    auto router = take(Router::create(FallbackOwnedKey, fallback));
    auto firstState = std::make_shared<TargetState>();
    auto secondState = std::make_shared<TargetState>();
    auto first = target(31U, firstState);
    auto second = target(32U, secondState);
    require(router->registerFixedTarget(first).hasValue(),
            "first shutdown target registration failed");
    require(router->registerFixedTarget(second).hasValue(),
            "second shutdown target registration failed");

    router->beginShutdown();
    router->beginShutdown();
    {
        const std::scoped_lock firstLock{firstState->mutex};
        const std::scoped_lock secondLock{secondState->mutex};
        require(firstState->shutdownCalls == 1U &&
                    secondState->shutdownCalls == 1U,
                "graceful shutdown was not delivered exactly once");
    }
    const auto closedRegistration =
        router->registerFixedTarget(target(33U));
    require(!closedRegistration &&
                closedRegistration.error().code ==
                    Domain::ErrorCodes::Conflict,
            "graceful shutdown did not close fixed registration");

    OVERLAPPED lateOperation{};
    router->consume(
        Packet{1U, Key{31U}, &lateOperation}, ERROR_SUCCESS);
    {
        const std::scoped_lock lock{firstState->mutex};
        require(firstState->consumeCalls == 1U,
                "graceful shutdown stopped exact drain routing");
    }

    router->fatal(ERROR_NETNAME_DELETED);
    router->fatal(ERROR_GEN_FAILURE);
    {
        const std::scoped_lock firstLock{firstState->mutex};
        const std::scoped_lock secondLock{secondState->mutex};
        require(firstState->fatalCalls == 1U &&
                    secondState->fatalCalls == 1U &&
                    firstState->fatalError == ERROR_NETNAME_DELETED &&
                    secondState->fatalError == ERROR_NETNAME_DELETED,
                "first fatal IOCP error was not delivered exactly once");
    }
    require(fallback->fatalCalls() == 1U &&
                fallback->fatalError() == ERROR_NETNAME_DELETED,
            "fatal IOCP error did not reach the fallback registry once");
    const auto snapshot = router->snapshot();
    require(!snapshot.registrationOpen() &&
                snapshot.shutdownRequested() &&
                snapshot.fatalNotificationCount() == 2U &&
                snapshot.fatalNativeError() ==
                    static_cast<DWORD>(ERROR_NETNAME_DELETED),
            "fatal router snapshot did not retain its first terminal error");
}

} // namespace

int main()
{
    try {
        rejectsFallbackOwnedKeyBeforeFixedTableMutation();
        routesFixedAndFallbackKeysWithoutCollapsingPacketFields();
        enforcesTheActiveRetiringAndOverloadFixedTable();
        unregistrationCannotDestroyADequeuedCallbackOwner();
        gracefulAndFatalShutdownRemainNonblockingAndIdempotent();
        std::cout << "Dashboard IOCP completion router tests passed ("
                  << assertions << " assertions).\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Dashboard IOCP completion router tests failed: "
                  << error.what() << '\n';
        return 1;
    } catch (...) {
        std::cerr << "Dashboard IOCP completion router tests failed with an "
                     "unknown error.\n";
        return 1;
    }
}
