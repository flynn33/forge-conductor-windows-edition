#include "ForgeConductor/Application/ClientPresenceLifecycle.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <iostream>
#include <memory>
#include <mutex>
#include <set>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

namespace {

namespace Application = ForgeConductor::Application;
namespace Contracts = ForgeConductor::Contracts;
namespace Domain = ForgeConductor::Domain;

using namespace std::chrono_literals;

std::size_t assertions{};

void require(const bool condition, const std::string_view message)
{
    ++assertions;
    if (!condition) {
        throw std::runtime_error{std::string{message}};
    }
}

template <typename T>
[[nodiscard]] T take(Domain::Result<T> result)
{
    require(result.hasValue(), result.hasValue() ? "" : result.error().message);
    return std::move(result).value();
}

void take(Domain::Result<void> result)
{
    require(result.hasValue(), result.hasValue() ? "" : result.error().message);
}

template <typename T>
[[nodiscard]] T parse(const std::string_view value)
{
    return take(T::parse(value));
}

class AdvancingClock final : public Contracts::IClock {
public:
    [[nodiscard]] Domain::UtcTimePoint utcNow() const noexcept override
    {
        return Domain::UtcTimePoint{} + std::chrono::seconds{1'700'000'000} +
            std::chrono::milliseconds{utcTicks_.fetch_add(1)};
    }

    [[nodiscard]] Domain::MonotonicTimePoint monotonicNow() const noexcept override
    {
        return Domain::MonotonicTimePoint{} + std::chrono::seconds{100} +
            std::chrono::milliseconds{monotonicTicks_.fetch_add(1)};
    }

private:
    mutable std::atomic<std::int64_t> utcTicks_{};
    mutable std::atomic<std::int64_t> monotonicTicks_{};
};

class SequentialUuidGenerator final : public Contracts::IUuidGenerator {
public:
    [[nodiscard]] Domain::Result<Domain::Uuid> next() noexcept override
    {
        try {
            const auto value = next_.fetch_add(1);
            std::array<char, 37U> text{};
            const int written = std::snprintf(
                text.data(), text.size(),
                "00000000-0000-4000-8000-%012llu",
                static_cast<unsigned long long>(value));
            if (written != 36) {
                return Domain::Result<Domain::Uuid>::failure(Domain::makeError(
                    Domain::ErrorCodes::InternalFailure,
                    "The test UUID could not be formatted."));
            }
            return Domain::Uuid::parse(text.data());
        } catch (...) {
            return Domain::Result<Domain::Uuid>::failure(Domain::makeError(
                Domain::ErrorCodes::InternalFailure,
                "The test UUID generator failed safely."));
        }
    }

private:
    std::atomic<std::uint64_t> next_{1U};
};

class RecordingRuntimeDiagnostics final
    : public Contracts::IRuntimeDiagnostics {
public:
    [[nodiscard]] Domain::Result<Contracts::RuntimeOwnershipLease> acquire(
        const Contracts::RuntimeOwnerKind kind,
        const Domain::OperationContext& context) noexcept override
    {
        try {
            std::lock_guard lock{mutex_};
            events_.push_back("lease-acquire");
            acquireContexts_.push_back(context);
            if (shutdown_) {
                return Domain::Result<Contracts::RuntimeOwnershipLease>::failure(
                    Domain::makeError(
                        Domain::ErrorCodes::Cancelled,
                        "The runtime diagnostics fake is shut down."));
            }
            if (kind != Contracts::RuntimeOwnerKind::BackgroundThread) {
                return Domain::Result<Contracts::RuntimeOwnershipLease>::failure(
                    Domain::makeError(
                        Domain::ErrorCodes::InvalidRequest,
                        "The lifecycle acquired the wrong runtime owner kind."));
            }
            ++active_;
            return Domain::Result<Contracts::RuntimeOwnershipLease>::success(
                issueOwnershipLease([this]() noexcept {
                    try {
                        std::lock_guard releaseLock{mutex_};
                        events_.push_back("lease-release");
                        if (active_ > 0U) {
                            --active_;
                        }
                        changed_.notify_all();
                    } catch (...) {
                    }
                }));
        } catch (...) {
            return Domain::Result<Contracts::RuntimeOwnershipLease>::failure(
                Domain::makeError(
                    Domain::ErrorCodes::InternalFailure,
                    "The runtime diagnostics fake failed safely."));
        }
    }

    [[nodiscard]] Domain::Result<Domain::RuntimeDiagnosticSnapshot> snapshot(
        const Domain::OperationContext&) noexcept override
    {
        return Domain::Result<Domain::RuntimeDiagnosticSnapshot>::failure(
            Domain::makeError(
                Domain::ErrorCodes::UnsupportedVersion,
                "Snapshots are not used by this fixture."));
    }

    void shutdown() noexcept override
    {
        std::lock_guard lock{mutex_};
        shutdown_ = true;
    }

    [[nodiscard]] std::size_t active() const
    {
        std::lock_guard lock{mutex_};
        return active_;
    }

    [[nodiscard]] std::vector<std::string> events() const
    {
        std::lock_guard lock{mutex_};
        return events_;
    }

private:
    mutable std::mutex mutex_;
    std::condition_variable changed_;
    std::vector<std::string> events_;
    std::vector<Domain::OperationContext> acquireContexts_;
    std::size_t active_{};
    bool shutdown_{};
};

class RecordingPresenceRepository final
    : public Contracts::IClientPresenceRepository {
public:
    explicit RecordingPresenceRepository(
        std::shared_ptr<RecordingRuntimeDiagnostics> diagnostics)
        : diagnostics_{std::move(diagnostics)}
    {
    }

    [[nodiscard]] Domain::Result<void> upsert(
        const Domain::ClientPresenceRegistration& registration,
        const Domain::OperationContext& context) noexcept override
    {
        try {
            std::lock_guard lock{mutex_};
            events_.push_back("upsert");
            registrations_.push_back(registration);
            upsertContexts_.push_back(context);
            changed_.notify_all();
            if (upsertFailuresRemaining_ > 0U) {
                --upsertFailuresRemaining_;
                return Domain::Result<void>::failure(Domain::makeError(
                    Domain::ErrorCodes::DatabaseBusy,
                    "The scripted presence registration failed.", true));
            }
            return Domain::Result<void>::success();
        } catch (...) {
            return Domain::Result<void>::failure(internalFailure());
        }
    }

    [[nodiscard]] Domain::Result<bool> heartbeat(
        const Domain::ClientPresenceIdentity& identity,
        const Domain::UtcTimePoint observedAt,
        const Domain::OperationContext& context) noexcept override
    {
        try {
            std::unique_lock lock{mutex_};
            ++activeHeartbeats_;
            maximumConcurrentHeartbeats_ = std::max(
                maximumConcurrentHeartbeats_, activeHeartbeats_);
            events_.push_back("heartbeat-enter");
            heartbeatIdentities_.push_back(identity);
            heartbeatTimes_.push_back(observedAt);
            heartbeatContexts_.push_back(context);
            const std::size_t callIndex = heartbeatContexts_.size() - 1U;
            changed_.notify_all();

            if (blockHeartbeat_) {
                while (!context.isCancellationRequested()) {
                    static_cast<void>(changed_.wait_for(lock, 5ms));
                }
                cancellationObserved_ = context.isCancellationRequested();
                events_.push_back("heartbeat-exit");
                --activeHeartbeats_;
                changed_.notify_all();
                return Domain::Result<bool>::failure(Domain::makeError(
                    Domain::ErrorCodes::Cancelled,
                    "The scripted heartbeat observed cancellation."));
            }

            Domain::Result<bool> result = Domain::Result<bool>::success(true);
            if (callIndex < heartbeatResults_.size()) {
                const int scripted = heartbeatResults_[callIndex];
                if (scripted < 0) {
                    result = Domain::Result<bool>::failure(Domain::makeError(
                        Domain::ErrorCodes::DatabaseBusy,
                        "The scripted heartbeat failed transiently.", true));
                } else {
                    result = Domain::Result<bool>::success(scripted != 0);
                }
            }
            events_.push_back("heartbeat-exit");
            --activeHeartbeats_;
            changed_.notify_all();
            return result;
        } catch (...) {
            return Domain::Result<bool>::failure(internalFailure());
        }
    }

    [[nodiscard]] Domain::Result<bool> remove(
        const Domain::ClientPresenceIdentity& identity,
        const Domain::OperationContext& context) noexcept override
    {
        try {
            std::lock_guard lock{mutex_};
            events_.push_back("remove");
            removeIdentities_.push_back(identity);
            removeContexts_.push_back(context);
            activeLeaseAtRemove_.push_back(diagnostics_->active());
            changed_.notify_all();
            if (removeFailuresRemaining_ > 0U) {
                --removeFailuresRemaining_;
                return Domain::Result<bool>::failure(Domain::makeError(
                    Domain::ErrorCodes::DatabaseBusy,
                    "The scripted presence removal failed transiently.", true));
            }
            return Domain::Result<bool>::success(removeResult_);
        } catch (...) {
            return Domain::Result<bool>::failure(internalFailure());
        }
    }

    void close() noexcept override {}

    void setUpsertFailures(const std::size_t count)
    {
        std::lock_guard lock{mutex_};
        upsertFailuresRemaining_ = count;
    }

    void setHeartbeatResults(std::vector<int> results)
    {
        std::lock_guard lock{mutex_};
        heartbeatResults_ = std::move(results);
    }

    void setBlockHeartbeat(const bool value)
    {
        std::lock_guard lock{mutex_};
        blockHeartbeat_ = value;
    }

    void setRemoveResult(const bool value)
    {
        std::lock_guard lock{mutex_};
        removeResult_ = value;
    }

    void setRemoveFailures(const std::size_t count)
    {
        std::lock_guard lock{mutex_};
        removeFailuresRemaining_ = count;
    }

    [[nodiscard]] bool waitForHeartbeats(
        const std::size_t count,
        const std::chrono::milliseconds timeout)
    {
        std::unique_lock lock{mutex_};
        return changed_.wait_for(
            lock, timeout,
            [this, count] { return heartbeatContexts_.size() >= count; });
    }

    [[nodiscard]] bool waitForUpserts(
        const std::size_t count,
        const std::chrono::milliseconds timeout)
    {
        std::unique_lock lock{mutex_};
        return changed_.wait_for(
            lock, timeout,
            [this, count] { return upsertContexts_.size() >= count; });
    }

    [[nodiscard]] std::vector<std::string> events() const
    {
        std::lock_guard lock{mutex_};
        return events_;
    }

    [[nodiscard]] std::vector<Domain::ClientPresenceRegistration>
    registrations() const
    {
        std::lock_guard lock{mutex_};
        return registrations_;
    }

    [[nodiscard]] std::vector<Domain::OperationContext>
    heartbeatContexts() const
    {
        std::lock_guard lock{mutex_};
        return heartbeatContexts_;
    }

    [[nodiscard]] std::vector<Domain::ClientPresenceIdentity>
    heartbeatIdentities() const
    {
        std::lock_guard lock{mutex_};
        return heartbeatIdentities_;
    }

    [[nodiscard]] std::vector<Domain::ClientPresenceIdentity>
    removeIdentities() const
    {
        std::lock_guard lock{mutex_};
        return removeIdentities_;
    }

    [[nodiscard]] std::vector<std::size_t> activeLeaseAtRemove() const
    {
        std::lock_guard lock{mutex_};
        return activeLeaseAtRemove_;
    }

    [[nodiscard]] std::size_t heartbeatCount() const
    {
        std::lock_guard lock{mutex_};
        return heartbeatContexts_.size();
    }

    [[nodiscard]] std::size_t removeCount() const
    {
        std::lock_guard lock{mutex_};
        return removeContexts_.size();
    }

    [[nodiscard]] std::size_t maximumConcurrentHeartbeats() const
    {
        std::lock_guard lock{mutex_};
        return maximumConcurrentHeartbeats_;
    }

    [[nodiscard]] bool cancellationObserved() const
    {
        std::lock_guard lock{mutex_};
        return cancellationObserved_;
    }

private:
    [[nodiscard]] static Domain::Error internalFailure()
    {
        return Domain::makeError(
            Domain::ErrorCodes::InternalFailure,
            "The scripted presence repository failed safely.");
    }

    std::shared_ptr<RecordingRuntimeDiagnostics> diagnostics_;
    mutable std::mutex mutex_;
    std::condition_variable changed_;
    std::vector<std::string> events_;
    std::vector<Domain::ClientPresenceRegistration> registrations_;
    std::vector<Domain::OperationContext> upsertContexts_;
    std::vector<Domain::ClientPresenceIdentity> heartbeatIdentities_;
    std::vector<Domain::UtcTimePoint> heartbeatTimes_;
    std::vector<Domain::OperationContext> heartbeatContexts_;
    std::vector<Domain::ClientPresenceIdentity> removeIdentities_;
    std::vector<Domain::OperationContext> removeContexts_;
    std::vector<std::size_t> activeLeaseAtRemove_;
    std::vector<int> heartbeatResults_;
    std::size_t activeHeartbeats_{};
    std::size_t maximumConcurrentHeartbeats_{};
    std::size_t upsertFailuresRemaining_{};
    bool blockHeartbeat_{};
    bool removeResult_{true};
    bool cancellationObserved_{};
    std::size_t removeFailuresRemaining_{};
};

struct Fixture final {
    Fixture()
        : clock{std::make_shared<AdvancingClock>()},
          uuids{std::make_shared<SequentialUuidGenerator>()},
          diagnostics{std::make_shared<RecordingRuntimeDiagnostics>()},
          repository{
              std::make_shared<RecordingPresenceRepository>(diagnostics)},
          lifecycle{
              repository,
              clock,
              uuids,
              diagnostics,
              Application::ClientPresenceTiming{15ms, 100ms}}
    {
    }

    [[nodiscard]] Domain::OperationContext context(
        const std::string_view correlation = "presence-lifecycle-test") const
    {
        return Domain::OperationContext{
            parse<Domain::OperationId>(
                "aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa"),
            clock->monotonicNow() + 5s,
            {},
            parse<Domain::CorrelationId>(correlation)};
    }

    [[nodiscard]] Domain::ClientPresenceIdentity identity() const
    {
        return Domain::ClientPresenceIdentity{
            parse<Domain::ClientId>("presence-client-primary"),
            "primary",
            parse<Domain::DeploymentId>("deployment-one"),
            4242U};
    }

    std::shared_ptr<AdvancingClock> clock;
    std::shared_ptr<SequentialUuidGenerator> uuids;
    std::shared_ptr<RecordingRuntimeDiagnostics> diagnostics;
    std::shared_ptr<RecordingPresenceRepository> repository;
    Application::ClientPresenceLifecycle lifecycle;
};

[[nodiscard]] std::size_t eventIndex(
    const std::vector<std::string>& events,
    const std::string_view name)
{
    const auto found = std::find(events.begin(), events.end(), name);
    require(found != events.end(), "the expected lifecycle event was not recorded");
    return static_cast<std::size_t>(std::distance(events.begin(), found));
}

void registrationPrecedesRepeatedBoundedHeartbeats()
{
    static_assert(std::is_final_v<Application::ClientPresenceLifecycle>);
    static_assert(!std::is_copy_constructible_v<
                  Application::ClientPresenceLifecycle>);
    static_assert(!std::is_move_constructible_v<
                  Application::ClientPresenceLifecycle>);
    static_assert(
        Application::ClientPresenceLifecycle::DefaultHeartbeatInterval == 10s);
    static_assert(
        Application::ClientPresenceLifecycle::DefaultOperationTimeout == 2s);

    Fixture fixture;
    const auto identity = fixture.identity();
    take(fixture.lifecycle.start(identity, fixture.context("presence-start")));
    require(fixture.diagnostics->active() == 1U,
            "the heartbeat worker did not retain its runtime ownership lease");
    require(fixture.repository->waitForHeartbeats(3U, 2s),
            "the idle lifecycle did not emit repeated heartbeats");

    const auto events = fixture.repository->events();
    require(eventIndex(events, "upsert") < eventIndex(events, "heartbeat-enter"),
            "a heartbeat became active before registration completed");
    const auto registrations = fixture.repository->registrations();
    require(registrations.size() == 1U, "registration count was not exact");
    require(registrations.front().identity == identity,
            "registration did not retain the complete identity");
    require(registrations.front().firstSeenAt == registrations.front().lastSeenAt,
            "registration did not use one initial observation time");

    const auto contexts = fixture.repository->heartbeatContexts();
    require(contexts.size() >= 3U, "heartbeat contexts were not retained");
    std::set<std::string> operationIds;
    for (const auto& context : contexts) {
        operationIds.insert(context.operationId.value());
        require(context.cancellation.stop_possible(),
                "a heartbeat context did not carry worker cancellation");
        require(context.correlationId.value() == "mcp-presence-heartbeat",
                "heartbeat correlation ownership was not stable");
    }
    require(operationIds.size() == contexts.size(),
            "heartbeat operation identities were reused");
    require(contexts[0].deadline < contexts[1].deadline &&
                contexts[1].deadline < contexts[2].deadline,
            "heartbeat deadlines were not freshly bounded");
    require(fixture.repository->maximumConcurrentHeartbeats() == 1U,
            "heartbeat calls overlapped");

    take(fixture.lifecycle.stop(fixture.context("presence-stop")));
    require(fixture.diagnostics->active() == 0U,
            "the heartbeat worker ownership lease was not released");
    require(fixture.repository->removeCount() == 1U,
            "normal stop did not remove exactly once");
    require(fixture.repository->removeIdentities().front() == identity,
            "normal stop did not compare-and-delete the exact identity");
    require(fixture.repository->activeLeaseAtRemove().front() == 0U,
            "presence removal occurred before the worker lease released");
}

void transientFailureRetriesWithoutOverlap()
{
    Fixture fixture;
    fixture.repository->setHeartbeatResults({-1, 1});
    take(fixture.lifecycle.start(
        fixture.identity(), fixture.context("presence-retry-start")));
    require(fixture.repository->waitForHeartbeats(2U, 2s),
            "a transient heartbeat failure was not retried");
    require(fixture.repository->maximumConcurrentHeartbeats() == 1U,
            "the transient retry overlapped its predecessor");
    take(fixture.lifecycle.stop(fixture.context("presence-retry-stop")));
}

void supersededOwnerRetiresWithoutReclaim()
{
    Fixture fixture;
    fixture.repository->setHeartbeatResults({0});
    fixture.repository->setRemoveResult(false);
    take(fixture.lifecycle.start(
        fixture.identity(), fixture.context("presence-superseded-start")));
    require(fixture.repository->waitForHeartbeats(1U, 2s),
            "the supersession heartbeat did not execute");
    std::this_thread::sleep_for(80ms);
    require(fixture.repository->heartbeatCount() == 1U,
            "a superseded lifecycle continued heartbeating or reclaimed ownership");
    take(fixture.lifecycle.stop(
        fixture.context("presence-superseded-stop")));
    require(fixture.repository->removeCount() == 1U,
            "the superseded lifecycle did not issue one exact cleanup attempt");
}

void stopCancelsInflightHeartbeatAndJoinsBeforeRemove()
{
    Fixture fixture;
    fixture.repository->setBlockHeartbeat(true);
    take(fixture.lifecycle.start(
        fixture.identity(), fixture.context("presence-blocked-start")));
    require(fixture.repository->waitForHeartbeats(1U, 2s),
            "the blocked heartbeat did not enter");

    take(fixture.lifecycle.stop(
        fixture.context("presence-blocked-stop")));
    require(fixture.repository->cancellationObserved(),
            "stop did not cancel the in-flight heartbeat context");
    const auto events = fixture.repository->events();
    require(eventIndex(events, "heartbeat-exit") < eventIndex(events, "remove"),
            "presence removal raced the in-flight heartbeat");
    require(fixture.repository->activeLeaseAtRemove().front() == 0U,
            "stop removed presence before joining the owned worker");
}

void failedRegistrationIsBestEffortAndRetries()
{
    Fixture fixture;
    const auto identity = fixture.identity();
    fixture.repository->setUpsertFailures(1U);
    take(fixture.lifecycle.start(
        identity, fixture.context("presence-failed-start")));
    require(fixture.diagnostics->active() == 1U,
            "a transient store lock prevented worker ownership");
    require(fixture.repository->waitForUpserts(2U, 2s),
            "a failed initial registration was not retried");
    require(fixture.repository->waitForHeartbeats(1U, 2s),
            "a recovered registration did not begin heartbeating");
    const auto registrations = fixture.repository->registrations();
    require(registrations.size() >= 2U,
            "the initial and retry registrations were not observed");
    require(registrations[0].identity == identity &&
                registrations[1].identity == identity,
            "the registration retry changed the exact owner identity");
    take(fixture.lifecycle.stop(fixture.context("presence-failed-stop")));
    require(fixture.repository->removeCount() == 1U,
            "recovered registration did not receive exact cleanup");
    require(fixture.diagnostics->active() == 0U,
            "recovered registration leaked the worker ownership lease");
}

void stopAndDestructorAreIdempotent()
{
    auto diagnostics = std::make_shared<RecordingRuntimeDiagnostics>();
    auto repository =
        std::make_shared<RecordingPresenceRepository>(diagnostics);
    auto clock = std::make_shared<AdvancingClock>();
    auto uuids = std::make_shared<SequentialUuidGenerator>();
    const auto identity = Domain::ClientPresenceIdentity{
        parse<Domain::ClientId>("destructor-client-primary"),
        "primary",
        parse<Domain::DeploymentId>("destructor-deployment"),
        5252U};
    const auto context = Domain::OperationContext{
        parse<Domain::OperationId>(
            "bbbbbbbb-bbbb-4bbb-8bbb-bbbbbbbbbbbb"),
        clock->monotonicNow() + 5s,
        {},
        parse<Domain::CorrelationId>("presence-idempotence")};
    {
        Application::ClientPresenceLifecycle lifecycle{
            repository,
            clock,
            uuids,
            diagnostics,
            Application::ClientPresenceTiming{15ms, 100ms}};
        take(lifecycle.start(identity, context));
        take(lifecycle.stop(context));
        take(lifecycle.stop(context));
    }
    require(repository->removeCount() == 1U,
            "repeated stop or destruction repeated presence removal");
    require(diagnostics->active() == 0U,
            "idempotent shutdown retained a runtime ownership lease");

    auto fallbackDiagnostics =
        std::make_shared<RecordingRuntimeDiagnostics>();
    auto fallbackRepository =
        std::make_shared<RecordingPresenceRepository>(fallbackDiagnostics);
    {
        Application::ClientPresenceLifecycle lifecycle{
            fallbackRepository,
            clock,
            uuids,
            fallbackDiagnostics,
            Application::ClientPresenceTiming{15ms, 100ms}};
        take(lifecycle.start(identity, context));
    }
    require(fallbackRepository->removeCount() == 1U,
            "destruction did not perform bounded exact cleanup");
    require(fallbackDiagnostics->active() == 0U,
            "destruction leaked the runtime ownership lease");
}

void failedRemovalRetainsIdentityForRetryAndDestruction()
{
    Fixture fixture;
    const auto identity = fixture.identity();
    fixture.repository->setRemoveFailures(1U);
    take(fixture.lifecycle.start(
        identity, fixture.context("presence-remove-retry-start")));
    const auto failed = fixture.lifecycle.stop(
        fixture.context("presence-remove-retry-first"));
    require(!failed, "a transient remove failure reported success");
    require(failed.error().code == Domain::ErrorCodes::DatabaseBusy,
            "the transient remove failure lost its typed error");
    require(fixture.repository->removeCount() == 1U,
            "the first exact remove attempt was not recorded");
    take(fixture.lifecycle.stop(
        fixture.context("presence-remove-retry-second")));
    require(fixture.repository->removeCount() == 2U,
            "an explicit stop did not retry the retained exact owner");
    const auto retryIdentities = fixture.repository->removeIdentities();
    require(retryIdentities.size() == 2U &&
                retryIdentities[0] == identity &&
                retryIdentities[1] == identity,
            "the removal retry did not retain the complete owner identity");

    auto diagnostics = std::make_shared<RecordingRuntimeDiagnostics>();
    auto repository =
        std::make_shared<RecordingPresenceRepository>(diagnostics);
    auto clock = std::make_shared<AdvancingClock>();
    auto uuids = std::make_shared<SequentialUuidGenerator>();
    repository->setRemoveFailures(1U);
    {
        Application::ClientPresenceLifecycle lifecycle{
            repository,
            clock,
            uuids,
            diagnostics,
            Application::ClientPresenceTiming{15ms, 100ms}};
        const auto context = Domain::OperationContext{
            parse<Domain::OperationId>(
                "cccccccc-cccc-4ccc-8ccc-cccccccccccc"),
            clock->monotonicNow() + 5s,
            {},
            parse<Domain::CorrelationId>("presence-remove-destructor")};
        take(lifecycle.start(identity, context));
        const auto failedStop = lifecycle.stop(context);
        require(!failedStop,
                "the destructor-retry setup remove did not fail");
    }
    require(repository->removeCount() == 2U,
            "destruction did not retry a retained exact owner");
    require(diagnostics->active() == 0U,
            "remove retry retained a runtime ownership lease");
}

struct TestCase final {
    const char* name;
    void (*run)();
};

} // namespace

int main()
{
    const std::array<TestCase, 7U> tests{{
        {"registration precedes repeated bounded heartbeats",
         registrationPrecedesRepeatedBoundedHeartbeats},
        {"transient failure retries without overlap",
         transientFailureRetriesWithoutOverlap},
        {"superseded owner retires without reclaim",
         supersededOwnerRetiresWithoutReclaim},
        {"stop cancels in-flight heartbeat and joins before remove",
         stopCancelsInflightHeartbeatAndJoinsBeforeRemove},
        {"failed registration is best effort and retries",
         failedRegistrationIsBestEffortAndRetries},
        {"stop and destructor are idempotent",
         stopAndDestructorAreIdempotent},
        {"failed removal retains identity for retry and destruction",
         failedRemovalRetainsIdentityForRetryAndDestruction},
    }};

    std::size_t passed{};
    for (const auto& test : tests) {
        try {
            test.run();
            ++passed;
            std::cout << "[PASS] " << test.name << '\n';
        } catch (const std::exception& error) {
            std::cerr << "[FAIL] " << test.name << ": " << error.what()
                      << '\n';
            return 1;
        } catch (...) {
            std::cerr << "[FAIL] " << test.name << ": unknown exception\n";
            return 1;
        }
    }
    std::cout << passed << '/' << tests.size()
              << " client presence lifecycle tests passed: " << assertions
              << " assertions.\n";
    return 0;
}
