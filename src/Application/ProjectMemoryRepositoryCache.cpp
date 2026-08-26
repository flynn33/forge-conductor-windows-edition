#include "ForgeConductor/Application/ProjectMemoryRepositoryCache.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <utility>
#include <vector>

namespace ForgeConductor::Application {
namespace {

constexpr std::size_t AbsoluteMaximumOpenRepositories = 16U;
constexpr auto CancellationObservationSlice = std::chrono::milliseconds{5};

[[nodiscard]] Domain::Error cacheError(
    const std::string_view code,
    std::string message,
    const bool retryable = false)
{
    return Domain::makeError(code, std::move(message), retryable);
}

} // namespace

class ProjectMemoryRepositoryCache::Impl final {
public:
    Impl(
        std::shared_ptr<Contracts::IProjectMemoryRepositoryOpener> opener,
        const std::size_t maximumOpenRepositories)
        : opener_{std::move(opener)}, maximumOpenRepositories_{maximumOpenRepositories}
    {
        if (!opener_) {
            throw std::invalid_argument("A project-memory repository opener is required.");
        }
        if (maximumOpenRepositories_ == 0U ||
            maximumOpenRepositories_ > AbsoluteMaximumOpenRepositories) {
            throw std::invalid_argument(
                "The project-memory repository cache limit must be within 1...16.");
        }
        entries_.reserve(maximumOpenRepositories_);
        pending_.reserve(maximumOpenRepositories_);
    }

    [[nodiscard]] Domain::Result<std::shared_ptr<Contracts::IProjectRepository>>
    openAggregate(
        const Domain::ProjectId& projectId,
        const Domain::OperationContext& context) noexcept
    {
        try {
            auto locked = acquire(context, "Open a project-memory repository");
            if (!locked) {
                return Domain::Result<
                    std::shared_ptr<Contracts::IProjectRepository>>::failure(
                    std::move(locked).error());
            }
            auto lock = std::move(locked).value();
            if (shutdownRequested_) {
                return closedRepository();
            }

            if (auto* existing = findEntry(projectId); existing != nullptr) {
                existing->lastUse = nextGeneration();
                return Domain::Result<
                    std::shared_ptr<Contracts::IProjectRepository>>::success(
                    existing->repository);
            }
            if (containsPending(projectId)) {
                return busyRepository(
                    "The selected project repository is already opening.");
            }

            std::shared_ptr<Contracts::IProjectRepository> victim;
            if (entries_.size() + pending_.size() >= maximumOpenRepositories_) {
                auto candidate = entries_.end();
                for (auto iterator = entries_.begin(); iterator != entries_.end(); ++iterator) {
                    if (iterator->repository.use_count() != 1L) {
                        continue;
                    }
                    if (candidate == entries_.end() ||
                        iterator->lastUse < candidate->lastUse ||
                        (iterator->lastUse == candidate->lastUse &&
                         iterator->projectId.value() < candidate->projectId.value())) {
                        candidate = iterator;
                    }
                }
                if (candidate == entries_.end()) {
                    return busyRepository(
                        "Every bounded project repository is pinned by an active operation.");
                }
                victim = std::move(candidate->repository);
                entries_.erase(candidate);
            }

            pending_.push_back(projectId);
            lock.unlock();
            if (victim) {
                victim->close();
                victim.reset();
            }

            auto created = opener_->openUncached(projectId, context);

            auto publishLockResult = acquire(context, "Publish a project-memory repository");
            if (!publishLockResult) {
                if (created) {
                    created.value()->close();
                }
                clearPendingNoexcept(projectId);
                return Domain::Result<
                    std::shared_ptr<Contracts::IProjectRepository>>::failure(
                    std::move(publishLockResult).error());
            }
            auto publishLock = std::move(publishLockResult).value();
            erasePending(projectId);
            condition_.notify_all();
            if (!created) {
                return Domain::Result<
                    std::shared_ptr<Contracts::IProjectRepository>>::failure(
                    std::move(created).error());
            }
            if (shutdownRequested_) {
                auto repository = std::move(created).value();
                publishLock.unlock();
                repository->close();
                return closedRepository();
            }
            if (auto* existing = findEntry(projectId); existing != nullptr) {
                auto redundant = std::move(created).value();
                existing->lastUse = nextGeneration();
                auto selected = existing->repository;
                publishLock.unlock();
                redundant->close();
                return Domain::Result<
                    std::shared_ptr<Contracts::IProjectRepository>>::success(
                    std::move(selected));
            }

            auto repository = std::move(created).value();
            if (!repository || repository->projectId() != projectId) {
                if (repository) {
                    publishLock.unlock();
                    repository->close();
                }
                return Domain::Result<
                    std::shared_ptr<Contracts::IProjectRepository>>::failure(
                    cacheError(
                        Domain::ErrorCodes::ProjectScopeMismatch,
                        "The repository opener returned a mismatched project scope."));
            }
            entries_.push_back(Entry{projectId, repository, nextGeneration()});
            return Domain::Result<
                std::shared_ptr<Contracts::IProjectRepository>>::success(
                std::move(repository));
        } catch (...) {
            clearPendingNoexcept(projectId);
            return Domain::Result<
                std::shared_ptr<Contracts::IProjectRepository>>::failure(
                cacheError(
                    Domain::ErrorCodes::InternalFailure,
                    "The bounded project-repository cache could not complete open."));
        }
    }

    [[nodiscard]] Domain::Result<void> close(
        const Domain::ProjectId& projectId,
        const Domain::OperationContext& context) noexcept
    {
        try {
            auto locked = acquire(context, "Close a project-memory repository");
            if (!locked) {
                return Domain::Result<void>::failure(std::move(locked).error());
            }
            auto lock = std::move(locked).value();
            if (containsPending(projectId)) {
                return Domain::Result<void>::failure(cacheError(
                    Domain::ErrorCodes::DatabaseBusy,
                    "The selected project repository is still opening.",
                    true));
            }
            const auto iterator = std::find_if(
                entries_.begin(), entries_.end(), [&](const Entry& entry) {
                    return entry.projectId == projectId;
                });
            if (iterator == entries_.end()) {
                return Domain::Result<void>::success();
            }
            if (iterator->repository.use_count() != 1L) {
                return Domain::Result<void>::failure(cacheError(
                    Domain::ErrorCodes::DatabaseBusy,
                    "The selected project repository is pinned by an active operation.",
                    true));
            }
            auto repository = std::move(iterator->repository);
            entries_.erase(iterator);
            lock.unlock();
            repository->close();
            return Domain::Result<void>::success();
        } catch (...) {
            return Domain::Result<void>::failure(cacheError(
                Domain::ErrorCodes::InternalFailure,
                "The bounded project-repository cache could not close the project."));
        }
    }

    [[nodiscard]] std::size_t openCount() const noexcept
    {
        try {
            std::lock_guard lock{mutex_};
            return entries_.size();
        } catch (...) {
            return 0U;
        }
    }

    void shutdown() noexcept
    {
        try {
            std::unique_lock lock{mutex_};
            if (shutdownCompleted_) {
                return;
            }
            if (shutdownInProgress_) {
                condition_.wait(lock, [this]() noexcept { return shutdownCompleted_; });
                return;
            }
            shutdownInProgress_ = true;
            shutdownRequested_ = true;
            condition_.wait(lock, [this]() noexcept { return pending_.empty(); });
            auto entries = std::move(entries_);
            entries_.clear();
            lock.unlock();
            std::sort(entries.begin(), entries.end(), [](const Entry& left, const Entry& right) {
                return left.projectId.value() < right.projectId.value();
            });
            for (auto& entry : entries) {
                entry.repository->close();
            }
            opener_->shutdown();
            lock.lock();
            shutdownCompleted_ = true;
            shutdownInProgress_ = false;
            condition_.notify_all();
        } catch (...) {
            try {
                opener_->shutdown();
            } catch (...) {
            }
            try {
                std::lock_guard lock{mutex_};
                shutdownCompleted_ = true;
                shutdownInProgress_ = false;
                condition_.notify_all();
            } catch (...) {
            }
        }
    }

private:
    struct Entry final {
        Domain::ProjectId projectId;
        std::shared_ptr<Contracts::IProjectRepository> repository;
        std::uint64_t lastUse{};
    };

    using CacheLock = std::unique_lock<std::timed_mutex>;

    [[nodiscard]] Domain::Result<CacheLock> acquire(
        const Domain::OperationContext& context,
        const std::string_view action) const noexcept
    {
        try {
            CacheLock lock{mutex_, std::defer_lock};
            for (;;) {
                if (context.isCancellationRequested()) {
                    return Domain::Result<CacheLock>::failure(cacheError(
                        Domain::ErrorCodes::Cancelled,
                        std::string{action} + " was cancelled before cache admission."));
                }
                const auto now = std::chrono::steady_clock::now();
                if (context.isExpired(now)) {
                    return Domain::Result<CacheLock>::failure(cacheError(
                        Domain::ErrorCodes::DeadlineExceeded,
                        std::string{action} + " exceeded its cache-admission deadline."));
                }
                const auto remaining = context.deadline - now;
                const auto slice = (std::min)(
                    std::chrono::duration_cast<std::chrono::milliseconds>(remaining),
                    CancellationObservationSlice);
                if (lock.try_lock_for(slice > std::chrono::milliseconds::zero()
                                          ? slice
                                          : std::chrono::milliseconds{1})) {
                    return Domain::Result<CacheLock>::success(std::move(lock));
                }
            }
        } catch (...) {
            return Domain::Result<CacheLock>::failure(cacheError(
                Domain::ErrorCodes::InternalFailure,
                std::string{action} + " could not acquire cache ownership."));
        }
    }

    [[nodiscard]] Entry* findEntry(const Domain::ProjectId& projectId) noexcept
    {
        const auto iterator = std::find_if(
            entries_.begin(), entries_.end(), [&](const Entry& entry) {
                return entry.projectId == projectId;
            });
        return iterator == entries_.end() ? nullptr : &*iterator;
    }

    [[nodiscard]] bool containsPending(const Domain::ProjectId& projectId) const noexcept
    {
        return std::find(pending_.begin(), pending_.end(), projectId) != pending_.end();
    }

    void erasePending(const Domain::ProjectId& projectId) noexcept
    {
        pending_.erase(
            std::remove(pending_.begin(), pending_.end(), projectId),
            pending_.end());
    }

    void clearPendingNoexcept(const Domain::ProjectId& projectId) noexcept
    {
        try {
            std::lock_guard lock{mutex_};
            erasePending(projectId);
            condition_.notify_all();
        } catch (...) {
        }
    }

    [[nodiscard]] std::uint64_t nextGeneration() noexcept
    {
        if (generation_ != (std::numeric_limits<std::uint64_t>::max)()) {
            return ++generation_;
        }
        for (auto& entry : entries_) {
            entry.lastUse = 0U;
        }
        generation_ = 1U;
        return generation_;
    }

    [[nodiscard]] static Domain::Result<
        std::shared_ptr<Contracts::IProjectRepository>>
    busyRepository(std::string message)
    {
        return Domain::Result<
            std::shared_ptr<Contracts::IProjectRepository>>::failure(
            cacheError(Domain::ErrorCodes::DatabaseBusy, std::move(message), true));
    }

    [[nodiscard]] static Domain::Result<
        std::shared_ptr<Contracts::IProjectRepository>>
    closedRepository()
    {
        return Domain::Result<
            std::shared_ptr<Contracts::IProjectRepository>>::failure(
            cacheError(
                Domain::ErrorCodes::TransportClosed,
                "The project-repository cache is shutting down."));
    }

    std::shared_ptr<Contracts::IProjectMemoryRepositoryOpener> opener_;
    const std::size_t maximumOpenRepositories_;
    mutable std::timed_mutex mutex_;
    mutable std::condition_variable_any condition_;
    std::vector<Entry> entries_;
    std::vector<Domain::ProjectId> pending_;
    std::uint64_t generation_{};
    bool shutdownRequested_{};
    bool shutdownInProgress_{};
    bool shutdownCompleted_{};
};

ProjectMemoryRepositoryCache::ProjectMemoryRepositoryCache(
    std::shared_ptr<Contracts::IProjectMemoryRepositoryOpener> opener,
    const std::size_t maximumOpenRepositories)
    : implementation_{std::make_unique<Impl>(
          std::move(opener), maximumOpenRepositories)}
{
}

ProjectMemoryRepositoryCache::~ProjectMemoryRepositoryCache()
{
    shutdown();
}

Domain::Result<std::shared_ptr<Contracts::IProjectMemoryRepository>>
ProjectMemoryRepositoryCache::open(
    const Domain::ProjectId& projectId,
    const Domain::OperationContext& context) noexcept
{
    if (!implementation_) {
        return Domain::Result<
            std::shared_ptr<Contracts::IProjectMemoryRepository>>::failure(
            cacheError(
                Domain::ErrorCodes::TransportClosed,
                "The project-repository cache has no implementation."));
    }
    auto opened = implementation_->openAggregate(projectId, context);
    if (!opened) {
        return Domain::Result<
            std::shared_ptr<Contracts::IProjectMemoryRepository>>::failure(
            std::move(opened).error());
    }
    std::shared_ptr<Contracts::IProjectMemoryRepository> repository =
        std::move(opened).value();
    return Domain::Result<
        std::shared_ptr<Contracts::IProjectMemoryRepository>>::success(
        std::move(repository));
}

Domain::Result<std::shared_ptr<Contracts::IContinuityRepository>>
ProjectMemoryRepositoryCache::openContinuity(
    const Domain::ProjectId& projectId,
    const Domain::OperationContext& context) noexcept
{
    if (!implementation_) {
        return Domain::Result<
            std::shared_ptr<Contracts::IContinuityRepository>>::failure(
            cacheError(
                Domain::ErrorCodes::TransportClosed,
                "The project-repository cache has no implementation."));
    }
    auto opened = implementation_->openAggregate(projectId, context);
    if (!opened) {
        return Domain::Result<
            std::shared_ptr<Contracts::IContinuityRepository>>::failure(
            std::move(opened).error());
    }
    std::shared_ptr<Contracts::IContinuityRepository> repository =
        std::move(opened).value();
    return Domain::Result<
        std::shared_ptr<Contracts::IContinuityRepository>>::success(
        std::move(repository));
}

Domain::Result<void> ProjectMemoryRepositoryCache::close(
    const Domain::ProjectId& projectId,
    const Domain::OperationContext& context) noexcept
{
    if (!implementation_) {
        return Domain::Result<void>::success();
    }
    return implementation_->close(projectId, context);
}

std::size_t ProjectMemoryRepositoryCache::openCount() const noexcept
{
    return implementation_ ? implementation_->openCount() : 0U;
}

void ProjectMemoryRepositoryCache::shutdown() noexcept
{
    if (implementation_) {
        implementation_->shutdown();
    }
}

} // namespace ForgeConductor::Application
