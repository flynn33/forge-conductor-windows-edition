#include "ManagerProcessRestartSignal.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <thread>
#include <type_traits>

namespace {

namespace Host = ForgeConductor::Hosts::Manager;

using namespace std::chrono_literals;

static_assert(std::is_final_v<Host::ManagerProcessRestartSignal>);

class TestFailure final : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

void require(const bool condition, const std::string& message)
{
    if (!condition) {
        throw TestFailure{message};
    }
}

void publishedRestartIsClaimedCompletedAndReusable()
{
    Host::ManagerProcessRestartSignal signal;
    require(!signal.pending(), "new signal was pending");
    require(!signal.inFlight(), "new signal was in flight");
    require(!signal.closed(), "new signal was closed");

    require(
        signal.requestRestart() ==
            Host::ManagerProcessRestartRequestResult::Published,
        "first restart was not published");
    require(signal.pending(), "published restart was not pending");
    require(
        signal.requestRestart() ==
            Host::ManagerProcessRestartRequestResult::Coalesced,
        "pending restart was not coalesced");

    require(
        signal.waitAndBegin({}) ==
            Host::ManagerProcessRestartWaitResult::RestartRequested,
        "pending restart was not claimed");
    require(!signal.pending(), "claimed restart remained pending");
    require(signal.inFlight(), "claimed restart was not in flight");
    require(
        signal.requestRestart() ==
            Host::ManagerProcessRestartRequestResult::Coalesced,
        "in-flight restart was not coalesced");

    require(signal.completeRestart(), "in-flight restart was not completed");
    require(!signal.completeRestart(), "restart was completed twice");
    require(!signal.pending(), "completed restart remained pending");
    require(!signal.inFlight(), "completed restart remained in flight");

    std::optional<Host::ManagerProcessRestartWaitResult> observed;
    std::jthread worker{[&signal, &observed](const std::stop_token stop) {
        observed = signal.waitAndBegin(stop);
    }};
    require(
        signal.requestRestart() ==
            Host::ManagerProcessRestartRequestResult::Published,
        "reused restart was not published");
    worker.join();

    require(
        observed == Host::ManagerProcessRestartWaitResult::RestartRequested,
        "worker did not claim the reused restart");
    require(signal.inFlight(), "reused restart was not in flight");
    require(signal.completeRestart(), "reused restart was not completed");
}

void cancellationDoesNotMutateSignalState()
{
    Host::ManagerProcessRestartSignal signal;
    std::atomic_bool entered{};
    std::optional<Host::ManagerProcessRestartWaitResult> observed;
    std::jthread worker{
        [&signal, &entered, &observed](const std::stop_token stop) {
            entered.store(true, std::memory_order_release);
            observed = signal.waitAndBegin(stop);
        }};

    const auto deadline = std::chrono::steady_clock::now() + 2s;
    while (!entered.load(std::memory_order_acquire) &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::yield();
    }
    require(entered.load(std::memory_order_acquire), "worker did not start");
    worker.request_stop();
    worker.join();

    require(
        observed == Host::ManagerProcessRestartWaitResult::Cancelled,
        "worker cancellation was not distinguished");
    require(!signal.pending(), "cancellation published a restart");
    require(!signal.inFlight(), "cancellation began a restart");
    require(!signal.closed(), "cancellation closed the signal");
}

void closeIsPermanentIdempotentAndWakesWorker()
{
    Host::ManagerProcessRestartSignal signal;
    std::optional<Host::ManagerProcessRestartWaitResult> observed;
    std::jthread worker{[&signal, &observed](const std::stop_token stop) {
        observed = signal.waitAndBegin(stop);
    }};

    signal.close();
    signal.close();
    worker.join();

    require(
        observed == Host::ManagerProcessRestartWaitResult::Closed,
        "close did not wake the worker with closed state");
    require(signal.closed(), "closed state was not retained");
    require(!signal.pending(), "closed signal was pending");
    require(!signal.inFlight(), "closed signal was in flight");
    require(
        signal.requestRestart() ==
            Host::ManagerProcessRestartRequestResult::Closed,
        "closed signal accepted a restart");
    require(
        signal.waitAndBegin({}) ==
            Host::ManagerProcessRestartWaitResult::Closed,
        "closed signal did not return immediately");
    require(!signal.completeRestart(), "closed signal completed a restart");

    Host::ManagerProcessRestartSignal pending;
    require(
        pending.requestRestart() ==
            Host::ManagerProcessRestartRequestResult::Published,
        "pre-close restart was not published");
    pending.close();
    require(pending.closed(), "close did not supersede pending state");
    require(
        pending.waitAndBegin({}) ==
            Host::ManagerProcessRestartWaitResult::Closed,
        "pending close was not observed as closed");

    Host::ManagerProcessRestartSignal active;
    require(
        active.requestRestart() ==
            Host::ManagerProcessRestartRequestResult::Published,
        "active-close restart was not published");
    require(
        active.waitAndBegin({}) ==
            Host::ManagerProcessRestartWaitResult::RestartRequested,
        "active-close restart was not claimed");
    active.close();
    require(active.closed(), "active close did not reject new work");
    require(active.inFlight(), "active close erased in-flight ownership");
    require(
        active.requestRestart() ==
            Host::ManagerProcessRestartRequestResult::Closed,
        "active close accepted a new request");
    require(
        active.completeRestart(),
        "active close did not acknowledge worker completion");
    require(active.closed(), "worker completion reopened a closed signal");
    require(!active.inFlight(), "worker completion retained in-flight state");
    require(!active.completeRestart(),
            "closed worker completion was acknowledged twice");
}

void concurrentPublishHasOneOwnerAndVisibleBoundedState()
{
    constexpr std::size_t producerCount = 16U;
    Host::ManagerProcessRestartSignal signal;
    std::atomic_bool release{};
    std::atomic_size_t published{};
    std::atomic_size_t coalesced{};
    std::array<std::jthread, producerCount> producers;

    for (auto& producer : producers) {
        producer = std::jthread{[&signal, &release, &published, &coalesced] {
            while (!release.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
            const auto result = signal.requestRestart();
            if (result ==
                Host::ManagerProcessRestartRequestResult::Published) {
                published.fetch_add(1U, std::memory_order_acq_rel);
            } else if (result ==
                       Host::ManagerProcessRestartRequestResult::Coalesced) {
                coalesced.fetch_add(1U, std::memory_order_acq_rel);
            }
        }};
    }

    release.store(true, std::memory_order_release);
    for (auto& producer : producers) {
        producer.join();
    }

    require(published.load(std::memory_order_acquire) == 1U,
            "concurrent producers published more than one restart");
    require(coalesced.load(std::memory_order_acquire) == producerCount - 1U,
            "concurrent producers did not coalesce exactly");
    require(signal.pending(), "concurrent publication was not visible");

    require(
        signal.waitAndBegin({}) ==
            Host::ManagerProcessRestartWaitResult::RestartRequested,
        "concurrent publication was not claimable");
    require(signal.inFlight(), "in-flight transition was not visible");
    require(signal.completeRestart(), "concurrent restart did not complete");
}

} // namespace

int main()
{
    try {
        publishedRestartIsClaimedCompletedAndReusable();
        cancellationDoesNotMutateSignalState();
        closeIsPermanentIdempotentAndWakesWorker();
        concurrentPublishHasOneOwnerAndVisibleBoundedState();
        std::cout << "Manager process restart signal tests passed.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Manager process restart signal tests failed: "
                  << error.what() << '\n';
        return 1;
    } catch (...) {
        std::cerr << "Manager process restart signal tests failed with an "
                     "unknown error.\n";
        return 1;
    }
}
