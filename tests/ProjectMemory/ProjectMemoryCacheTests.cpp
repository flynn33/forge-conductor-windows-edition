#include "ForgeConductor/Application/ProjectMemoryRepositoryCache.h"
#include "Fakes/ProjectRepositoryFakes.h"

#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace {

using namespace std::chrono_literals;
namespace Contracts = ForgeConductor::Contracts;
namespace Domain = ForgeConductor::Domain;
namespace Application = ForgeConductor::Application;
namespace Fakes = ForgeConductor::Tests::Fakes;

#define REQUIRE(condition)                                                       \
    do {                                                                         \
        if (!(condition)) {                                                      \
            throw std::runtime_error{std::string{"Requirement failed: "} + #condition}; \
        }                                                                        \
    } while (false)

template <typename T>
[[nodiscard]] T take(Domain::Result<T> result)
{
    if (!result) {
        throw std::runtime_error{result.error().message};
    }
    return std::move(result).value();
}

[[nodiscard]] Domain::ProjectId project(const char* value)
{
    return take(Domain::ProjectId::parse(value));
}

[[nodiscard]] Domain::OperationContext context()
{
    return Domain::OperationContext{
        take(Domain::OperationId::parse("aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa")),
        std::chrono::steady_clock::now() + 5s,
        std::stop_token{},
        take(Domain::CorrelationId::parse("project-memory-cache"))};
}

template <typename Left, typename Right>
[[nodiscard]] bool sharesOwner(
    const std::shared_ptr<Left>& left,
    const std::shared_ptr<Right>& right) noexcept
{
    return !left.owner_before(right) && !right.owner_before(left);
}

class RecordingOpener final : public Contracts::IProjectMemoryRepositoryOpener {
public:
    [[nodiscard]] Domain::Result<std::shared_ptr<Contracts::IProjectRepository>>
    openUncached(
        const Domain::ProjectId& projectId,
        const Domain::OperationContext& operation) noexcept override
    {
        try {
            if (shutdown_) {
                return Domain::Result<
                    std::shared_ptr<Contracts::IProjectRepository>>::failure(
                    Domain::makeError(
                        Domain::ErrorCodes::TransportClosed,
                        "The recording repository opener is shut down."));
            }
            if (operation.isCancellationRequested()) {
                return Domain::Result<
                    std::shared_ptr<Contracts::IProjectRepository>>::failure(
                    Domain::makeError(
                        Domain::ErrorCodes::Cancelled,
                        "The recording repository open was cancelled."));
            }
            ++opens_[projectId.value()];
            auto repository = std::make_shared<Fakes::ProjectMemoryRepositoryFake>(projectId);
            opened_.push_back(repository);
            return Domain::Result<
                std::shared_ptr<Contracts::IProjectRepository>>::success(
                std::move(repository));
        } catch (...) {
            return Domain::Result<
                std::shared_ptr<Contracts::IProjectRepository>>::failure(
                Domain::makeError(
                    Domain::ErrorCodes::InternalFailure,
                    "The recording repository opener failed."));
        }
    }

    void shutdown() noexcept override
    {
        shutdown_ = true;
        ++shutdownCount_;
    }

    [[nodiscard]] std::size_t opens(const Domain::ProjectId& projectId) const noexcept
    {
        const auto iterator = opens_.find(projectId.value());
        return iterator == opens_.end() ? 0U : iterator->second;
    }

    [[nodiscard]] std::size_t shutdownCount() const noexcept { return shutdownCount_; }

private:
    std::unordered_map<std::string, std::size_t> opens_;
    std::vector<std::weak_ptr<Contracts::IProjectRepository>> opened_;
    std::size_t shutdownCount_{};
    bool shutdown_{};
};

class BlockingOpener final : public Contracts::IProjectMemoryRepositoryOpener {
public:
    [[nodiscard]] Domain::Result<std::shared_ptr<Contracts::IProjectRepository>>
    openUncached(
        const Domain::ProjectId& projectId,
        const Domain::OperationContext&) noexcept override
    {
        try {
            std::unique_lock lock{mutex_};
            if (shutdown_) {
                return Domain::Result<
                    std::shared_ptr<Contracts::IProjectRepository>>::failure(
                    Domain::makeError(
                        Domain::ErrorCodes::TransportClosed,
                        "The blocking repository opener is shut down."));
            }
            ++entered_;
            enteredProjects_.push_back(projectId.value());
            condition_.notify_all();
            condition_.wait(lock, [this]() noexcept { return released_ || shutdown_; });
            if (shutdown_) {
                return Domain::Result<
                    std::shared_ptr<Contracts::IProjectRepository>>::failure(
                    Domain::makeError(
                        Domain::ErrorCodes::TransportClosed,
                        "The blocking repository opener shut down during open."));
            }
            lock.unlock();
            return Domain::Result<
                std::shared_ptr<Contracts::IProjectRepository>>::success(
                std::make_shared<Fakes::ProjectMemoryRepositoryFake>(projectId));
        } catch (...) {
            return Domain::Result<
                std::shared_ptr<Contracts::IProjectRepository>>::failure(
                Domain::makeError(
                    Domain::ErrorCodes::InternalFailure,
                    "The blocking repository opener failed."));
        }
    }

    void shutdown() noexcept override
    {
        try {
            std::lock_guard lock{mutex_};
            shutdown_ = true;
            condition_.notify_all();
        } catch (...) {
        }
    }

    [[nodiscard]] bool waitUntilEntered(
        const std::size_t count,
        const std::chrono::milliseconds timeout) noexcept
    {
        try {
            std::unique_lock lock{mutex_};
            return condition_.wait_for(
                lock, timeout, [this, count]() noexcept { return entered_ >= count; });
        } catch (...) {
            return false;
        }
    }

    void release() noexcept
    {
        try {
            std::lock_guard lock{mutex_};
            released_ = true;
            condition_.notify_all();
        } catch (...) {
        }
    }

    [[nodiscard]] std::size_t entered() const noexcept
    {
        try {
            std::lock_guard lock{mutex_};
            return entered_;
        } catch (...) {
            return 0U;
        }
    }

    [[nodiscard]] std::string firstEnteredProjectId() const
    {
        std::lock_guard lock{mutex_};
        return enteredProjects_.empty() ? std::string{} : enteredProjects_.front();
    }

private:
    mutable std::mutex mutex_;
    std::condition_variable condition_;
    std::size_t entered_{};
    std::vector<std::string> enteredProjects_;
    bool released_{};
    bool shutdown_{};
};

void deterministicLruEviction()
{
    const auto first = project("11111111-1111-4111-8111-111111111111");
    const auto second = project("22222222-2222-4222-8222-222222222222");
    const auto third = project("33333333-3333-4333-8333-333333333333");
    auto opener = std::make_shared<RecordingOpener>();
    Application::ProjectMemoryRepositoryCache cache{opener, 2U};

    auto firstPin = take(cache.open(first, context()));
    firstPin.reset();
    auto secondPin = take(cache.open(second, context()));
    secondPin.reset();
    firstPin = take(cache.open(first, context()));
    firstPin.reset();
    auto thirdPin = take(cache.open(third, context()));
    thirdPin.reset();

    REQUIRE(cache.openCount() == 2U);
    REQUIRE(opener->opens(first) == 1U);
    REQUIRE(opener->opens(second) == 1U);
    REQUIRE(opener->opens(third) == 1U);

    secondPin = take(cache.open(second, context()));
    secondPin.reset();
    REQUIRE(opener->opens(second) == 2U);
    REQUIRE(cache.openCount() == 2U);
}

void activePinCannotBeEvictedOrClosed()
{
    const auto first = project("44444444-4444-4444-8444-444444444444");
    const auto second = project("55555555-5555-4555-8555-555555555555");
    auto opener = std::make_shared<RecordingOpener>();
    Application::ProjectMemoryRepositoryCache cache{opener, 1U};

    auto pin = take(cache.open(first, context()));
    const auto blockedOpen = cache.open(second, context());
    REQUIRE(!blockedOpen);
    REQUIRE(blockedOpen.error().code == Domain::ErrorCodes::DatabaseBusy);
    REQUIRE(blockedOpen.error().retryable);
    const auto blockedClose = cache.close(first, context());
    REQUIRE(!blockedClose);
    REQUIRE(blockedClose.error().code == Domain::ErrorCodes::DatabaseBusy);
    REQUIRE(cache.openCount() == 1U);

    pin.reset();
    REQUIRE(cache.close(first, context()));
    REQUIRE(cache.openCount() == 0U);
    auto secondPin = take(cache.open(second, context()));
    REQUIRE(secondPin->projectId() == second);
}

void memoryAndContinuityShareAggregateOwnerAndBounds()
{
    const auto first = project("12121212-1212-4212-8212-121212121212");
    const auto second = project("34343434-3434-4434-8434-343434343434");
    auto opener = std::make_shared<RecordingOpener>();
    Application::ProjectMemoryRepositoryCache cache{opener, 1U};

    auto memory = take(cache.open(first, context()));
    auto continuity = take(cache.openContinuity(first, context()));
    REQUIRE(sharesOwner(memory, continuity));
    REQUIRE(memory->projectId() == continuity->projectId());
    REQUIRE(opener->opens(first) == 1U);
    REQUIRE(cache.openCount() == 1U);

    auto blocked = cache.open(second, context());
    REQUIRE(!blocked);
    REQUIRE(blocked.error().code == Domain::ErrorCodes::DatabaseBusy);
    memory.reset();
    blocked = cache.open(second, context());
    REQUIRE(!blocked);
    REQUIRE(blocked.error().code == Domain::ErrorCodes::DatabaseBusy);
    continuity.reset();

    auto secondMemory = take(cache.open(second, context()));
    REQUIRE(secondMemory->projectId() == second);
    REQUIRE(cache.openCount() == 1U);
    REQUIRE(opener->opens(second) == 1U);
}

void closeThroughEitherFactoryClosesOneAggregateOwner()
{
    const auto projectId = project("56565656-5656-4656-8656-565656565656");

    {
        auto opener = std::make_shared<RecordingOpener>();
        Application::ProjectMemoryRepositoryCache cache{opener, 1U};
        Contracts::IProjectMemoryRepositoryFactory& memoryFactory = cache;
        Contracts::IContinuityRepositoryFactory& continuityFactory = cache;
        auto memory = take(memoryFactory.open(projectId, context()));
        auto continuity = take(continuityFactory.openContinuity(projectId, context()));
        REQUIRE(sharesOwner(memory, continuity));
        memory.reset();
        continuity.reset();
        REQUIRE(memoryFactory.close(projectId, context()));
        REQUIRE(memoryFactory.openCount() == 0U);
        auto reopened = take(continuityFactory.openContinuity(projectId, context()));
        REQUIRE(opener->opens(projectId) == 2U);
        reopened.reset();
    }

    {
        auto opener = std::make_shared<RecordingOpener>();
        Application::ProjectMemoryRepositoryCache cache{opener, 1U};
        Contracts::IProjectMemoryRepositoryFactory& memoryFactory = cache;
        Contracts::IContinuityRepositoryFactory& continuityFactory = cache;
        auto continuity = take(continuityFactory.openContinuity(projectId, context()));
        auto memory = take(memoryFactory.open(projectId, context()));
        REQUIRE(sharesOwner(memory, continuity));
        memory.reset();
        continuity.reset();
        REQUIRE(continuityFactory.close(projectId, context()));
        REQUIRE(continuityFactory.openCount() == 0U);
        auto reopened = take(memoryFactory.open(projectId, context()));
        REQUIRE(opener->opens(projectId) == 2U);
        reopened.reset();
    }
}

void profileBoundsAndShutdown()
{
    constexpr std::size_t bounds[]{4U, 8U, 16U};
    for (const auto bound : bounds) {
        auto opener = std::make_shared<RecordingOpener>();
        Application::ProjectMemoryRepositoryCache cache{opener, bound};
        for (std::size_t index = 0; index < bound; ++index) {
            std::string id = "60000000-0000-4000-8000-000000000000";
            const char digit = static_cast<char>('0' + (index % 10U));
            id[id.size() - 1U] = digit;
            id[id.size() - 2U] = static_cast<char>('0' + ((index / 10U) % 10U));
            auto pin = take(cache.open(project(id.c_str()), context()));
            pin.reset();
        }
        REQUIRE(cache.openCount() == bound);
        cache.shutdown();
        REQUIRE(cache.openCount() == 0U);
        REQUIRE(opener->shutdownCount() == 1U);
        const auto rejected = cache.open(
            project("77777777-7777-4777-8777-777777777777"), context());
        REQUIRE(!rejected);
        REQUIRE(rejected.error().code == Domain::ErrorCodes::TransportClosed);
    }
}

void cancelledAdmissionFailsClosed()
{
    auto opener = std::make_shared<RecordingOpener>();
    Application::ProjectMemoryRepositoryCache cache{opener, 4U};
    std::stop_source cancellation;
    cancellation.request_stop();
    const Domain::OperationContext cancelled{
        take(Domain::OperationId::parse("bbbbbbbb-bbbb-4bbb-8bbb-bbbbbbbbbbbb")),
        std::chrono::steady_clock::now() + 5s,
        cancellation.get_token(),
        take(Domain::CorrelationId::parse("project-memory-cache-cancelled"))};
    const auto result = cache.open(
        project("88888888-8888-4888-8888-888888888888"), cancelled);
    REQUIRE(!result);
    REQUIRE(result.error().code == Domain::ErrorCodes::Cancelled);
    REQUIRE(cache.openCount() == 0U);
}

void concurrentReservationsRemainBounded()
{
    constexpr std::size_t RepositoryLimit = 4U;
    constexpr std::size_t RequestCount = 12U;
    auto opener = std::make_shared<BlockingOpener>();
    Application::ProjectMemoryRepositoryCache cache{opener, RepositoryLimit};

    std::vector<Domain::ProjectId> projects;
    projects.reserve(RequestCount);
    for (std::size_t index = 0U; index < RequestCount; ++index) {
        std::string id = "90000000-0000-4000-8000-000000000000";
        id[id.size() - 1U] = static_cast<char>('0' + (index % 10U));
        id[id.size() - 2U] = static_cast<char>('0' + ((index / 10U) % 10U));
        projects.push_back(project(id.c_str()));
    }

    struct Outcome final {
        bool succeeded{};
        std::string errorCode;
    };
    std::vector<Outcome> outcomes(RequestCount);
    std::mutex completionMutex;
    std::condition_variable completionCondition;
    std::size_t completed{};
    std::vector<std::thread> workers;
    workers.reserve(RequestCount);
    for (std::size_t index = 0U; index < RequestCount; ++index) {
        workers.emplace_back([&, index]() {
            auto result = cache.open(projects[index], context());
            outcomes[index].succeeded = result.hasValue();
            if (!result) {
                outcomes[index].errorCode = result.error().code;
            }
            {
                std::lock_guard lock{completionMutex};
                ++completed;
            }
            completionCondition.notify_all();
        });
    }

    const bool exactReservationsEntered =
        opener->waitUntilEntered(RepositoryLimit, 5s);
    bool pendingCloseRejected{};
    const auto pendingProjectId = opener->firstEnteredProjectId();
    if (!pendingProjectId.empty()) {
        const auto closeResult = cache.close(
            project(pendingProjectId.c_str()), context());
        pendingCloseRejected =
            !closeResult &&
            closeResult.error().code == Domain::ErrorCodes::DatabaseBusy &&
            closeResult.error().retryable;
    }
    bool excessRequestsRejected{};
    {
        std::unique_lock lock{completionMutex};
        excessRequestsRejected = completionCondition.wait_for(
            lock, 5s, [&]() noexcept {
                return completed >= RequestCount - RepositoryLimit;
            });
    }
    opener->release();
    for (auto& worker : workers) {
        worker.join();
    }

    REQUIRE(exactReservationsEntered);
    REQUIRE(pendingCloseRejected);
    REQUIRE(excessRequestsRejected);
    REQUIRE(opener->entered() == RepositoryLimit);
    std::size_t successes{};
    std::size_t busyFailures{};
    for (const auto& outcome : outcomes) {
        if (outcome.succeeded) {
            ++successes;
        } else if (outcome.errorCode == Domain::ErrorCodes::DatabaseBusy) {
            ++busyFailures;
        }
    }
    REQUIRE(successes == RepositoryLimit);
    REQUIRE(busyFailures == RequestCount - RepositoryLimit);
    REQUIRE(cache.openCount() == RepositoryLimit);
}

} // namespace

int main()
{
    try {
        deterministicLruEviction();
        std::cout << "PASS project_memory_cache.deterministic_lru\n";
        activePinCannotBeEvictedOrClosed();
        std::cout << "PASS project_memory_cache.active_pin\n";
        memoryAndContinuityShareAggregateOwnerAndBounds();
        std::cout << "PASS project_memory_cache.aggregate_owner_bounds\n";
        closeThroughEitherFactoryClosesOneAggregateOwner();
        std::cout << "PASS project_memory_cache.either_factory_close\n";
        profileBoundsAndShutdown();
        std::cout << "PASS project_memory_cache.profile_bounds_shutdown\n";
        cancelledAdmissionFailsClosed();
        std::cout << "PASS project_memory_cache.cancelled_admission\n";
        concurrentReservationsRemainBounded();
        std::cout << "PASS project_memory_cache.concurrent_reservations\n";
        std::cout << "SUMMARY passed=7 failed=0\n";
        return EXIT_SUCCESS;
    } catch (const std::exception& error) {
        std::cerr << "FAIL " << error.what() << '\n';
        return EXIT_FAILURE;
    }
}
