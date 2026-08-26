#include "ForgeConductor/Application/AgentSessionService.h"
#include "ForgeConductor/Domain/Utf8.h"
#include "ForgeConductor/Infrastructure/Windows/InfrastructureWindows.h"
#include "Fakes/DeterministicWorkspaceAuthority.h"
#include "Fakes/FoundationFakes.h"
#include "Fakes/ToolServiceFakes.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <future>
#include <iostream>
#include <map>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

namespace {

using namespace std::chrono_literals;
namespace Application = ForgeConductor::Application;
namespace Contracts = ForgeConductor::Contracts;
namespace Domain = ForgeConductor::Domain;
namespace Fakes = ForgeConductor::Tests::Fakes;

static_assert(std::is_final_v<Application::AgentSessionService>);
static_assert(!std::is_copy_constructible_v<Application::AgentSessionService>);
static_assert(!std::is_move_constructible_v<Application::AgentSessionService>);

#define REQUIRE(condition)                                                       \
    do {                                                                         \
        if (!(condition)) {                                                      \
            throw std::runtime_error{std::string{"Requirement failed: "} +     \
                                     #condition};                                \
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

template <typename T>
[[nodiscard]] T parse(const std::string_view value)
{
    return take(T::parse(value));
}

[[nodiscard]] Domain::AgentId agentId()
{
    return parse<Domain::AgentId>("implement");
}

[[nodiscard]] Domain::ProjectId projectId()
{
    return parse<Domain::ProjectId>(
        "11111111-1111-4111-8111-111111111111");
}

[[nodiscard]] Domain::AuthorityId authorityId()
{
    return parse<Domain::AuthorityId>(
        "22222222-2222-4222-8222-222222222222");
}

[[nodiscard]] Domain::SessionId sessionId(const std::string_view value)
{
    return parse<Domain::SessionId>(value);
}

[[nodiscard]] Domain::ClientId clientId(const std::string_view value)
{
    return parse<Domain::ClientId>(value);
}

[[nodiscard]] Domain::PathText path(const std::string_view value)
{
    return take(Domain::PathText::create(value));
}

[[nodiscard]] Domain::AgentSpec makeSpec(
    std::vector<std::string> schema = {"result"})
{
    return Domain::AgentSpec{
        agentId(),
        "Implement",
        "Implements a bounded change.",
        {"read", "edit"},
        {"shell"},
        {"Implementation is requested."},
        {"Inspect", "Implement"},
        {"Tests pass"},
        std::move(schema),
        {"Hand off evidence"},
        {"No regressions"},
        "# Implement",
        "builtin"};
}

[[nodiscard]] Domain::ActiveBinding bindingFor(
    const Domain::AgentRunRecord& run,
    std::string goal = "goal")
{
    return Domain::ActiveBinding{
        run.session.id,
        run.session.agentId,
        std::move(goal),
        {"read", "edit"},
        {"shell"},
        run.outputSchema,
        {"Tests pass"},
        run.workingDirectory};
}

[[nodiscard]] Domain::AgentRunRecord makeRun(
    const Domain::SessionId& id,
    const Domain::ClientId& owner,
    const Domain::UtcTimePoint updatedAt,
    std::optional<std::string> goal = std::string{"goal"},
    std::vector<std::string> schema = {"result"})
{
    return Domain::AgentRunRecord{
        Domain::AgentSession{
            id,
            agentId(),
            owner,
            Domain::SessionStatus::Active,
            std::nullopt,
            updatedAt - 1s,
            updatedAt},
        projectId(),
        std::move(goal),
        path("C:/workspace"),
        std::move(schema),
        {"Inspect"},
        std::nullopt};
}

template <typename T>
[[nodiscard]] Domain::Result<T> repositoryFailure(
    const std::string_view message)
{
    return Domain::Result<T>::failure(Domain::makeError(
        Domain::ErrorCodes::InternalFailure, std::string{message}));
}

class MutableAgentCatalog final : public Contracts::IAgentCatalog {
public:
    explicit MutableAgentCatalog(Domain::AgentSpec spec)
        : spec_{std::move(spec)}
    {
    }

    void setSpec(Domain::AgentSpec spec)
    {
        std::lock_guard lock{mutex_};
        spec_ = std::move(spec);
    }

    [[nodiscard]] std::size_t getCalls() const noexcept
    {
        std::lock_guard lock{mutex_};
        return getCalls_;
    }

    [[nodiscard]] Domain::Result<std::vector<Domain::AgentSpec>> all(
        const Domain::OperationContext& context) noexcept override
    {
        try {
            (void)context;
            std::lock_guard lock{mutex_};
            return Domain::Result<std::vector<Domain::AgentSpec>>::success(
                {spec_});
        } catch (...) {
            return repositoryFailure<std::vector<Domain::AgentSpec>>(
                "The test catalog could not enumerate agents.");
        }
    }

    [[nodiscard]] Domain::Result<std::optional<Domain::AgentSpec>> get(
        const Domain::AgentId& requested,
        const Domain::OperationContext& context) noexcept override
    {
        try {
            (void)context;
            std::lock_guard lock{mutex_};
            ++getCalls_;
            return Domain::Result<std::optional<Domain::AgentSpec>>::success(
                requested == spec_.id
                    ? std::optional<Domain::AgentSpec>{spec_}
                    : std::nullopt);
        } catch (...) {
            return repositoryFailure<std::optional<Domain::AgentSpec>>(
                "The test catalog could not read an agent.");
        }
    }

    [[nodiscard]] Domain::Result<Domain::AgentSpec> recommend(
        const std::string_view task,
        const Domain::OperationContext& context) noexcept override
    {
        try {
            (void)task;
            (void)context;
            std::lock_guard lock{mutex_};
            return Domain::Result<Domain::AgentSpec>::success(spec_);
        } catch (...) {
            return repositoryFailure<Domain::AgentSpec>(
                "The test catalog could not recommend an agent.");
        }
    }

private:
    mutable std::mutex mutex_;
    Domain::AgentSpec spec_;
    std::size_t getCalls_{};
};

class CounterUuidGenerator final : public Contracts::IUuidGenerator {
public:
    [[nodiscard]] Domain::Result<Domain::Uuid> next() noexcept override
    {
        try {
            std::lock_guard lock{mutex_};
            ++counter_;
            std::array<char, 37> buffer{};
            const auto written = std::snprintf(
                buffer.data(),
                buffer.size(),
                "%08x-0000-4000-8000-%012llx",
                static_cast<unsigned int>(counter_),
                static_cast<unsigned long long>(counter_));
            if (written != 36) {
                return repositoryFailure<Domain::Uuid>(
                    "The test UUID generator could not format a UUID.");
            }
            return Domain::Uuid::parse(buffer.data());
        } catch (...) {
            return repositoryFailure<Domain::Uuid>(
                "The test UUID generator failed.");
        }
    }

private:
    std::mutex mutex_;
    std::size_t counter_{};
};

class InMemoryAgentSessionRepository final
    : public Contracts::IAgentSessionRepository {
public:
    void seed(
        Domain::AgentRunRecord run,
        std::optional<Domain::ActiveBinding> binding = std::nullopt)
    {
        std::lock_guard lock{mutex_};
        const auto owner = run.session.clientId;
        const auto id = run.session.id.value();
        runs_.insert_or_assign(id, std::move(run));
        if (owner && binding) {
            active_.insert_or_assign(owner->value(), std::move(*binding));
        }
    }

    void setProjection(
        const Domain::ClientId& client,
        Domain::ActiveBinding binding)
    {
        std::lock_guard lock{mutex_};
        active_.insert_or_assign(client.value(), std::move(binding));
    }

    void markProjectionNeedsRepair(const Domain::ClientId& client)
    {
        std::lock_guard lock{mutex_};
        projectionRepairNeeded_.insert_or_assign(client.value(), true);
    }

    [[nodiscard]] std::optional<Domain::AgentRunRecord> snapshot(
        const Domain::SessionId& id) const
    {
        std::lock_guard lock{mutex_};
        const auto found = runs_.find(id.value());
        return found == runs_.end()
            ? std::nullopt
            : std::optional<Domain::AgentRunRecord>{found->second};
    }

    [[nodiscard]] std::vector<std::string> callOrder() const
    {
        std::lock_guard lock{mutex_};
        return callOrder_;
    }

    [[nodiscard]] std::size_t startCalls() const noexcept
    {
        std::lock_guard lock{mutex_};
        return startCalls_;
    }

    [[nodiscard]] std::size_t repairCalls() const noexcept
    {
        std::lock_guard lock{mutex_};
        return repairCalls_;
    }

    [[nodiscard]] std::size_t closeStaleCalls() const noexcept
    {
        std::lock_guard lock{mutex_};
        return closeStaleCalls_;
    }

    [[nodiscard]] bool closeCalled() const noexcept
    {
        std::lock_guard lock{mutex_};
        return closeCalled_;
    }

    void blockFirstStartAfterCommit() noexcept
    {
        std::lock_guard lock{mutex_};
        blockFirstStart_ = true;
    }

    [[nodiscard]] bool waitForFirstStartCommit(
        const std::chrono::milliseconds timeout)
    {
        std::unique_lock lock{mutex_};
        return changed_.wait_for(
            lock, timeout, [&]() { return firstStartCommitted_; });
    }

    [[nodiscard]] bool waitForStartCalls(
        const std::size_t count,
        const std::chrono::milliseconds timeout)
    {
        std::unique_lock lock{mutex_};
        return changed_.wait_for(
            lock, timeout, [&]() { return startCalls_ >= count; });
    }

    void releaseFirstStart() noexcept
    {
        std::lock_guard lock{mutex_};
        releaseFirstStart_ = true;
        changed_.notify_all();
    }

    void blockNextGetRun() noexcept
    {
        std::lock_guard lock{mutex_};
        blockNextGetRun_ = true;
        getRunEntered_ = false;
        releaseGetRun_ = false;
    }

    [[nodiscard]] bool waitForGetRun(
        const std::chrono::milliseconds timeout)
    {
        std::unique_lock lock{mutex_};
        return changed_.wait_for(
            lock, timeout, [&]() { return getRunEntered_; });
    }

    void releaseGetRun() noexcept
    {
        std::lock_guard lock{mutex_};
        releaseGetRun_ = true;
        changed_.notify_all();
    }

    void blockNextCompletionAfterCommit() noexcept
    {
        std::lock_guard lock{mutex_};
        blockNextCompletion_ = true;
        completionCommitted_ = false;
        releaseCompletion_ = false;
    }

    [[nodiscard]] bool waitForCompletionCommit(
        const std::chrono::milliseconds timeout)
    {
        std::unique_lock lock{mutex_};
        return changed_.wait_for(
            lock, timeout, [&]() { return completionCommitted_; });
    }

    void releaseCompletion() noexcept
    {
        std::lock_guard lock{mutex_};
        releaseCompletion_ = true;
        changed_.notify_all();
    }

    void closeBeforeNextTouch() noexcept
    {
        std::lock_guard lock{mutex_};
        closeBeforeNextTouch_ = true;
    }

    void failNextTouch() noexcept
    {
        std::lock_guard lock{mutex_};
        failNextTouch_ = true;
    }

    [[nodiscard]] Domain::Result<void> save(
        const Domain::AgentSession& session,
        const Domain::OperationContext& context) noexcept override
    {
        try {
            (void)context;
            std::lock_guard lock{mutex_};
            runs_.insert_or_assign(
                session.id.value(),
                Domain::AgentRunRecord{
                    session,
                    std::nullopt,
                    std::nullopt,
                    std::nullopt,
                    {},
                    {},
                    std::nullopt});
            return Domain::Result<void>::success();
        } catch (...) {
            return Domain::Result<void>::failure(Domain::makeError(
                Domain::ErrorCodes::InternalFailure,
                "The in-memory legacy session save failed."));
        }
    }

    [[nodiscard]] Domain::Result<std::optional<Domain::AgentSession>> get(
        const Domain::SessionId& id,
        const Domain::OperationContext& context) noexcept override
    {
        try {
            (void)context;
            std::lock_guard lock{mutex_};
            const auto found = runs_.find(id.value());
            return Domain::Result<std::optional<Domain::AgentSession>>::success(
                found == runs_.end()
                    ? std::nullopt
                    : std::optional<Domain::AgentSession>{
                          found->second.session});
        } catch (...) {
            return repositoryFailure<std::optional<Domain::AgentSession>>(
                "The in-memory legacy session lookup failed.");
        }
    }

    [[nodiscard]] Domain::Result<std::vector<Domain::AgentSession>> list(
        const std::optional<Domain::AgentId>& requestedAgent,
        const std::optional<Domain::SessionStatus>& requestedStatus,
        const std::size_t maximumCount,
        const Domain::OperationContext& context) noexcept override
    {
        try {
            (void)context;
            std::lock_guard lock{mutex_};
            std::vector<Domain::AgentSession> result;
            for (const auto& [id, run] : runs_) {
                (void)id;
                if (result.size() >= maximumCount) {
                    break;
                }
                if (requestedAgent && run.session.agentId != requestedAgent) {
                    continue;
                }
                if (requestedStatus && run.session.status != requestedStatus) {
                    continue;
                }
                result.push_back(run.session);
            }
            return Domain::Result<std::vector<Domain::AgentSession>>::success(
                std::move(result));
        } catch (...) {
            return repositoryFailure<std::vector<Domain::AgentSession>>(
                "The in-memory legacy session list failed.");
        }
    }

    [[nodiscard]] Domain::Result<Domain::AgentRunStartPersistenceOutcome>
    startRun(
        const Domain::AgentRunStartMutation& mutation,
        const Domain::OperationContext& context) noexcept override
    {
        try {
            (void)context;
            std::unique_lock lock{mutex_};
            std::size_t superseded{};
            if (mutation.run.session.clientId) {
                const auto& client = *mutation.run.session.clientId;
                for (auto& [id, existing] : runs_) {
                    (void)id;
                    if (existing.session.id == mutation.run.session.id ||
                        existing.session.clientId != client ||
                        !Domain::isOpen(existing.session.status)) {
                        continue;
                    }
                    existing.session.status = Domain::SessionStatus::Closed;
                    existing.session.summary = mutation.supersedeSummary;
                    existing.session.updatedAt = mutation.run.session.createdAt;
                    ++superseded;
                }
                active_.erase(client.value());
            }
            runs_.insert_or_assign(
                mutation.run.session.id.value(), mutation.run);
            if (mutation.run.session.clientId && mutation.activeBinding) {
                active_.insert_or_assign(
                    mutation.run.session.clientId->value(),
                    *mutation.activeBinding);
            }
            ++startCalls_;
            callOrder_.push_back("start_run");
            changed_.notify_all();
            const Domain::AgentRunStartPersistenceOutcome outcome{
                mutation.run, mutation.activeBinding, superseded};
            if (blockFirstStart_ && startCalls_ == 1U) {
                firstStartCommitted_ = true;
                changed_.notify_all();
                changed_.wait(lock, [&]() { return releaseFirstStart_; });
            }
            return Domain::Result<
                Domain::AgentRunStartPersistenceOutcome>::success(outcome);
        } catch (...) {
            return repositoryFailure<Domain::AgentRunStartPersistenceOutcome>(
                "The in-memory start transaction failed.");
        }
    }

    [[nodiscard]] Domain::Result<std::optional<Domain::AgentRunRecord>> getRun(
        const Domain::SessionId& id,
        const Domain::OperationContext& context) noexcept override
    {
        try {
            (void)context;
            std::unique_lock lock{mutex_};
            if (blockNextGetRun_) {
                blockNextGetRun_ = false;
                getRunEntered_ = true;
                changed_.notify_all();
                changed_.wait(lock, [&]() { return releaseGetRun_; });
            }
            const auto found = runs_.find(id.value());
            return Domain::Result<
                std::optional<Domain::AgentRunRecord>>::success(
                    found == runs_.end()
                        ? std::nullopt
                        : std::optional<Domain::AgentRunRecord>{found->second});
        } catch (...) {
            return repositoryFailure<std::optional<Domain::AgentRunRecord>>(
                "The in-memory run lookup failed.");
        }
    }

    [[nodiscard]] Domain::Result<Domain::AgentRunReattachOutcome> reattachRun(
        const Domain::AgentRunReattachMutation& mutation,
        const Domain::OperationContext& context) noexcept override
    {
        try {
            (void)context;
            std::lock_guard lock{mutex_};
            const auto found = runs_.find(mutation.sessionId.value());
            if (found == runs_.end()) {
                return Domain::Result<Domain::AgentRunReattachOutcome>::failure(
                    Domain::makeError(
                        Domain::ErrorCodes::SessionNotFound,
                        "The session does not exist."));
            }
            auto& run = found->second;
            if (!Domain::isOpen(run.session.status) ||
                run.session.clientId != mutation.expectedClientId) {
                return Domain::Result<Domain::AgentRunReattachOutcome>::failure(
                    Domain::makeError(
                        Domain::ErrorCodes::OwnershipConflict,
                        "The session ownership changed."));
            }
            const auto previous = run.session.clientId;
            std::size_t superseded{};
            for (auto& [id, existing] : runs_) {
                (void)id;
                if (existing.session.id == mutation.sessionId ||
                    existing.session.clientId != mutation.clientId ||
                    !Domain::isOpen(existing.session.status)) {
                    continue;
                }
                existing.session.status = Domain::SessionStatus::Closed;
                existing.session.summary = mutation.supersedeSummary;
                existing.session.updatedAt = mutation.attachedAt;
                ++superseded;
            }
            if (previous) {
                const auto active = active_.find(previous->value());
                if (active != active_.end() &&
                    active->second.sessionId == mutation.sessionId) {
                    active_.erase(active);
                }
            }
            run.session.clientId = mutation.clientId;
            run.session.status = Domain::SessionStatus::Active;
            run.session.updatedAt = mutation.attachedAt;
            active_.insert_or_assign(
                mutation.clientId.value(), mutation.binding);
            return Domain::Result<Domain::AgentRunReattachOutcome>::success(
                Domain::AgentRunReattachOutcome{
                    run,
                    mutation.binding,
                    previous,
                    superseded,
                    previous != mutation.clientId});
        } catch (...) {
            return repositoryFailure<Domain::AgentRunReattachOutcome>(
                "The in-memory reattach transaction failed.");
        }
    }

    [[nodiscard]] Domain::Result<Domain::AgentRunCompletePersistenceOutcome>
    completeRun(
        const Domain::AgentRunCompleteMutation& mutation,
        const Domain::OperationContext& context) noexcept override
    {
        try {
            (void)context;
            std::unique_lock lock{mutex_};
            const auto found = runs_.find(mutation.sessionId.value());
            if (found == runs_.end()) {
                return Domain::Result<
                    Domain::AgentRunCompletePersistenceOutcome>::failure(
                        Domain::makeError(
                            Domain::ErrorCodes::SessionNotFound,
                            "The session does not exist."));
            }
            auto& run = found->second;
            if (!Domain::isOpen(run.session.status) ||
                (mutation.expectedClientId &&
                 run.session.clientId != mutation.expectedClientId)) {
                return Domain::Result<
                    Domain::AgentRunCompletePersistenceOutcome>::failure(
                        Domain::makeError(
                            Domain::ErrorCodes::OwnershipConflict,
                            "The completion owner does not match."));
            }
            run.session.status = Domain::SessionStatus::Closed;
            run.session.summary = mutation.summary;
            run.session.updatedAt = mutation.completedAt;
            run.reportJson = mutation.reportJson;
            bool cleared{};
            if (run.session.clientId) {
                const auto active = active_.find(run.session.clientId->value());
                if (active != active_.end() &&
                    active->second.sessionId == mutation.sessionId) {
                    active_.erase(active);
                    cleared = true;
                }
            }
            if (blockNextCompletion_) {
                blockNextCompletion_ = false;
                completionCommitted_ = true;
                changed_.notify_all();
                changed_.wait(lock, [&]() { return releaseCompletion_; });
            }
            return Domain::Result<
                Domain::AgentRunCompletePersistenceOutcome>::success(
                    Domain::AgentRunCompletePersistenceOutcome{run, cleared});
        } catch (...) {
            return repositoryFailure<
                Domain::AgentRunCompletePersistenceOutcome>(
                    "The in-memory completion transaction failed.");
        }
    }

    [[nodiscard]] Domain::Result<bool> touchRun(
        const Domain::SessionId& id,
        const Domain::UtcTimePoint touchedAt,
        const Domain::OperationContext& context) noexcept override
    {
        try {
            (void)context;
            std::lock_guard lock{mutex_};
            const auto found = runs_.find(id.value());
            if (failNextTouch_) {
                failNextTouch_ = false;
                return repositoryFailure<bool>(
                    "The scripted in-memory touch failed.");
            }
            if (closeBeforeNextTouch_ && found != runs_.end() &&
                Domain::isOpen(found->second.session.status)) {
                closeBeforeNextTouch_ = false;
                found->second.session.status = Domain::SessionStatus::Closed;
                found->second.session.summary = "completed concurrently";
                found->second.session.updatedAt = touchedAt;
                if (found->second.session.clientId) {
                    const auto active = active_.find(
                        found->second.session.clientId->value());
                    if (active != active_.end() &&
                        active->second.sessionId == id) {
                        active_.erase(active);
                    }
                }
                return Domain::Result<bool>::success(false);
            }
            if (found == runs_.end() ||
                !Domain::isOpen(found->second.session.status)) {
                return Domain::Result<bool>::success(false);
            }
            found->second.session.updatedAt = touchedAt;
            return Domain::Result<bool>::success(true);
        } catch (...) {
            return repositoryFailure<bool>("The in-memory touch failed.");
        }
    }

    [[nodiscard]] Domain::Result<std::optional<Domain::AgentRunRecord>>
    latestOpenRun(
        const Domain::ClientId& client,
        const Domain::OperationContext& context) noexcept override
    {
        try {
            (void)context;
            std::lock_guard lock{mutex_};
            std::optional<Domain::AgentRunRecord> latest;
            for (const auto& [id, run] : runs_) {
                (void)id;
                if (run.session.clientId != client ||
                    !Domain::isOpen(run.session.status)) {
                    continue;
                }
                if (!latest || run.session.updatedAt > latest->session.updatedAt ||
                    (run.session.updatedAt == latest->session.updatedAt &&
                     run.session.id.value() > latest->session.id.value())) {
                    latest = run;
                }
            }
            return Domain::Result<
                std::optional<Domain::AgentRunRecord>>::success(
                    std::move(latest));
        } catch (...) {
            return repositoryFailure<std::optional<Domain::AgentRunRecord>>(
                "The in-memory latest-run lookup failed.");
        }
    }

    [[nodiscard]] Domain::Result<Domain::AgentRunRecoveryOutcome> recoverRun(
        const Domain::AgentRunRecoveryRequest& request,
        const Domain::OperationContext& context) noexcept override
    {
        try {
            (void)context;
            std::lock_guard lock{mutex_};
            const auto active = active_.find(request.clientId.value());
            if (active == active_.end()) {
                return Domain::Result<Domain::AgentRunRecoveryOutcome>::success(
                    Domain::AgentRunRecoveryOutcome{});
            }
            const auto run = runs_.find(active->second.sessionId.value());
            if (run == runs_.end() ||
                !Domain::isOpen(run->second.session.status) ||
                run->second.session.clientId != request.clientId) {
                return Domain::Result<Domain::AgentRunRecoveryOutcome>::success(
                    Domain::AgentRunRecoveryOutcome{
                        std::nullopt, std::nullopt, true, true});
            }
            const auto repair = projectionRepairNeeded_.contains(
                request.clientId.value());
            return Domain::Result<Domain::AgentRunRecoveryOutcome>::success(
                Domain::AgentRunRecoveryOutcome{
                    run->second, active->second, true, repair});
        } catch (...) {
            return repositoryFailure<Domain::AgentRunRecoveryOutcome>(
                "The in-memory recovery lookup failed.");
        }
    }

    [[nodiscard]] Domain::Result<Domain::AgentProjectionRepairOutcome>
    repairProjection(
        const Domain::AgentProjectionRepairRequest& request,
        const Domain::OperationContext& context) noexcept override
    {
        try {
            (void)context;
            std::lock_guard lock{mutex_};
            const auto run = runs_.find(request.run.session.id.value());
            if (run == runs_.end() ||
                !Domain::isOpen(run->second.session.status) ||
                run->second.session.clientId != request.clientId) {
                return Domain::Result<
                    Domain::AgentProjectionRepairOutcome>::failure(
                        Domain::makeError(
                            Domain::ErrorCodes::OwnershipConflict,
                            "The repair owner does not match."));
            }
            ++repairCalls_;
            active_.insert_or_assign(
                request.clientId.value(), request.binding);
            projectionRepairNeeded_.erase(request.clientId.value());
            return Domain::Result<
                Domain::AgentProjectionRepairOutcome>::success(
                    Domain::AgentProjectionRepairOutcome{
                        request.binding, true});
        } catch (...) {
            return repositoryFailure<Domain::AgentProjectionRepairOutcome>(
                "The in-memory projection repair failed.");
        }
    }

    [[nodiscard]] Domain::Result<Domain::AgentStaleCloseOutcome> closeStale(
        const Domain::AgentStaleCloseRequest& request,
        const Domain::OperationContext& context) noexcept override
    {
        try {
            (void)context;
            std::lock_guard lock{mutex_};
            ++closeStaleCalls_;
            callOrder_.push_back("close_stale");
            Domain::AgentStaleCloseOutcome outcome;
            for (auto& [id, run] : runs_) {
                (void)id;
                if (outcome.closedRuns.size() >= request.maximumCount) {
                    break;
                }
                if (!Domain::isOpen(run.session.status) ||
                    run.session.updatedAt >= request.cutoff) {
                    continue;
                }
                const auto age = std::chrono::duration_cast<std::chrono::seconds>(
                    request.now - run.session.updatedAt);
                run.session.status = Domain::SessionStatus::Closed;
                run.session.summary = Domain::makeAgentStaleSummary(age);
                run.session.updatedAt = request.now;
                if (run.session.clientId) {
                    const auto active = active_.find(run.session.clientId->value());
                    if (active != active_.end() &&
                        active->second.sessionId == run.session.id) {
                        active_.erase(active);
                    }
                }
                outcome.closedRuns.push_back(run);
            }
            return Domain::Result<Domain::AgentStaleCloseOutcome>::success(
                std::move(outcome));
        } catch (...) {
            return repositoryFailure<Domain::AgentStaleCloseOutcome>(
                "The in-memory stale close failed.");
        }
    }

    [[nodiscard]] Domain::Result<void> quickCheck(
        const Domain::OperationContext& context) noexcept override
    {
        (void)context;
        return Domain::Result<void>::success();
    }

    void close() noexcept override
    {
        std::lock_guard lock{mutex_};
        closeCalled_ = true;
    }

private:
    mutable std::mutex mutex_;
    std::condition_variable changed_;
    std::map<std::string, Domain::AgentRunRecord> runs_;
    std::map<std::string, Domain::ActiveBinding> active_;
    std::map<std::string, bool> projectionRepairNeeded_;
    std::vector<std::string> callOrder_;
    std::size_t startCalls_{};
    std::size_t repairCalls_{};
    std::size_t closeStaleCalls_{};
    bool closeCalled_{};
    bool blockFirstStart_{};
    bool firstStartCommitted_{};
    bool releaseFirstStart_{};
    bool blockNextGetRun_{};
    bool getRunEntered_{};
    bool releaseGetRun_{};
    bool blockNextCompletion_{};
    bool completionCommitted_{};
    bool releaseCompletion_{};
    bool closeBeforeNextTouch_{};
    bool failNextTouch_{};
};

class AgentWorkspaceAuthority final : public Contracts::IWorkspaceAuthority {
public:
    AgentWorkspaceAuthority()
        : delegate_{
              authorityId(),
              clientId("agent-path-authority"),
              {path("C:/workspace")},
              Domain::FileAccess::Write,
              {Domain::FileAccess::Read, Domain::FileAccess::Write},
              {Domain::FileAccess::Delete},
              false,
              1U}
    {
        delegate_.setNow(Domain::MonotonicTimePoint{10h});
    }

    [[nodiscard]] Domain::Result<Contracts::WorkspaceAuthority> authorityFor(
        const Domain::ProjectId& project,
        const Domain::OperationContext& context) noexcept override
    {
        return delegate_.authorityFor(project, context);
    }

    [[nodiscard]] Domain::Result<Contracts::WorkspaceAuthority> narrow(
        const Contracts::WorkspaceAuthority& authority,
        const std::vector<Domain::PathText>& trustedRoots,
        const std::vector<Domain::FileAccess>& grants,
        const bool shellEnabled,
        const std::uint64_t generation,
        const Domain::OperationContext& context) noexcept override
    {
        return delegate_.narrow(
            authority,
            trustedRoots,
            grants,
            shellEnabled,
            generation,
            context);
    }

    [[nodiscard]] Domain::Result<Contracts::AuthorizedPath> authorize(
        const Contracts::WorkspaceAuthority& authority,
        const Domain::PathAuthorizationRequest& request,
        const Domain::OperationContext& context) noexcept override
    {
        ++authorizeCalls_;
        if (rejectNextResolvedPath_) {
            rejectNextResolvedPath_ = false;
            return Domain::Result<Contracts::AuthorizedPath>::failure(
                Domain::makeError(
                    Domain::ErrorCodes::PathOutsideAuthority,
                    "The resolved test path escaped through a reparse point."));
        }
        return delegate_.authorize(authority, request, context);
    }

    void rejectNextResolvedPath() noexcept { rejectNextResolvedPath_ = true; }

    [[nodiscard]] std::size_t authorizeCalls() const noexcept
    {
        return authorizeCalls_;
    }

private:
    Fakes::DeterministicWorkspaceAuthority delegate_;
    std::size_t authorizeCalls_{};
    bool rejectNextResolvedPath_{};
};

struct Fixture final {
    Fakes::FakeClock clock{
        Domain::UtcTimePoint{100h}, Domain::MonotonicTimePoint{10h}};
    MutableAgentCatalog catalog{makeSpec()};
    InMemoryAgentSessionRepository repository;
    CounterUuidGenerator uuids;
    std::unique_ptr<Contracts::IAgentCompletionReportInspector> reportInspector{
        ForgeConductor::Infrastructure::Windows::
            createWindowsAgentCompletionReportInspector(clock)};
    AgentWorkspaceAuthority workspaceAuthority;
    Application::AgentSessionService service{
        catalog,
        repository,
        *reportInspector,
        workspaceAuthority,
        clock,
        uuids,
        4h};

    [[nodiscard]] Domain::OperationContext context(
        const std::string_view suffix = "default") const
    {
        return Domain::OperationContext{
            parse<Domain::OperationId>(
                "33333333-3333-4333-8333-333333333333"),
            clock.monotonicNow() + 1h,
            std::stop_token{},
            parse<Domain::CorrelationId>(
                std::string{"agent-session-service-"} + std::string{suffix})};
    }
};

[[nodiscard]] Contracts::WorkspaceAuthority authorityFor(
    const Domain::ClientId& client,
    const Domain::OperationContext& context)
{
    Fakes::DeterministicWorkspaceAuthority issuer{
        authorityId(),
        client,
        {path("C:/workspace")},
        Domain::FileAccess::Write,
        {Domain::FileAccess::Read, Domain::FileAccess::Write},
        {Domain::FileAccess::Delete},
        false,
        1U};
    issuer.setNow(Domain::MonotonicTimePoint{10h});
    return take(issuer.authorityFor(projectId(), context));
}

[[nodiscard]] Contracts::AuthorizedToolCall authorizationFor(
    const Contracts::WorkspaceAuthority& authority,
    const Domain::OperationContext& context,
    const Domain::SessionId& session,
    const std::string_view request,
    const std::optional<Domain::SessionId>& alsoSession = std::nullopt)
{
    std::string canonical =
        std::string{"{\"session_id\":\""} + session.value() + "\"";
    if (alsoSession) {
        canonical += ",\"also_session_id\":\"" + alsoSession->value() + "\"";
    }
    canonical += "}";
    Fakes::DeterministicToolAuthorizerFake authorizer{
        "agent_run_status", Domain::ToolEffect::Write,
        Domain::MonotonicTimePoint{10h}};
    return take(authorizer.authorize(
        Domain::ToolAuthorizationRequest{
            Domain::ToolCallRequest{
                Domain::McpRequestMetadata{
                    parse<Domain::RequestId>(request),
                    context.correlationId,
                    authority.callerId(),
                    authority.projectId(),
                    "1.0"},
                "agent_run_status",
                std::move(canonical)},
            Domain::ToolEffect::Write,
            Domain::AuthorityReference{
                authority.authorityId(), authority.generation()}},
        authority,
        context));
}

[[nodiscard]] Domain::AgentRunStartRequest startRequest(
    const Domain::ClientId& client,
    std::string goal = "goal")
{
    return Domain::AgentRunStartRequest{
        agentId(),
        client,
        projectId(),
        std::move(goal),
        path("C:/workspace")};
}

[[nodiscard]] std::size_t utf8CodePoints(const std::string_view value)
{
    return static_cast<std::size_t>(std::count_if(
        value.begin(), value.end(), [](const unsigned char character) {
            return (character & 0xc0U) != 0x80U;
        }));
}

void startPrunesBeforeAtomicCommitAndUsesStableErrors()
{
    Fixture fixture;
    const auto staleOwner = clientId("stale-client");
    const auto stale = makeRun(
        sessionId("aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa"),
        staleOwner,
        fixture.clock.utcNow() - 5h);
    fixture.repository.seed(stale, bindingFor(stale));

    const auto result = fixture.service.startRun(
        startRequest(clientId("start-client")), fixture.context("start"));
    REQUIRE(result);
    REQUIRE(result.value().run.projectId == projectId());
    REQUIRE(result.value().run.goal == std::optional<std::string>{"goal"});
    REQUIRE(result.value().activeBinding);
    REQUIRE(result.value().mustComplete);
    REQUIRE(fixture.repository.closeStaleCalls() == 1U);
    const auto order = fixture.repository.callOrder();
    REQUIRE(order.size() >= 2U);
    REQUIRE(order[order.size() - 2U] == "close_stale");
    REQUIRE(order.back() == "start_run");
    REQUIRE(!Domain::isOpen(
        fixture.repository.snapshot(stale.session.id)->session.status));

    const auto missing = fixture.service.startRun(
        Domain::AgentRunStartRequest{
            parse<Domain::AgentId>("missing"),
            clientId("start-client"),
            projectId(),
            "goal",
            std::nullopt},
        fixture.context("missing-agent"));
    REQUIRE(!missing);
    REQUIRE(missing.error().code == Domain::ErrorCodes::AgentNotFound);
}

void statusUsesObservedIdleAndAuthorizationIsGloballyOneUse()
{
    Fixture fixture;
    const auto oldOwner = clientId("old-status-client");
    const auto newOwner = clientId("new-status-client");
    const auto id = sessionId("bbbbbbbb-bbbb-4bbb-8bbb-bbbbbbbbbbbb");
    const auto run = makeRun(id, oldOwner, fixture.clock.utcNow() - 301s);
    fixture.repository.seed(run);
    const auto context = fixture.context("status-idle");
    const auto authority = authorityFor(newOwner, context);
    const auto authorization = authorizationFor(
        authority, context, id, "status-idle-request");
    const auto status = fixture.service.runStatus(
        {id, newOwner}, authority, authorization, context);
    REQUIRE(status);
    REQUIRE(status.value().run);
    REQUIRE(status.value().run->session.clientId == newOwner);
    REQUIRE(status.value().idleSeconds == std::optional<std::int64_t>{0});
    REQUIRE(!status.value().abandonRisk);
    REQUIRE(status.value().reattached);

    const auto exactId =
        sessionId("cccccccc-cccc-4ccc-8ccc-cccccccccccc");
    const auto exactRun = makeRun(
        exactId, oldOwner, fixture.clock.utcNow() - 300s);
    fixture.repository.seed(exactRun);
    const auto exactAuthorization = authorizationFor(
        authority, context, exactId, "status-exact-request");
    const auto exact = fixture.service.runStatus(
        {exactId, newOwner}, authority, exactAuthorization, context);
    REQUIRE(exact);
    REQUIRE(exact.value().idleSeconds == std::optional<std::int64_t>{0});
    REQUIRE(!exact.value().abandonRisk);

    fixture.clock.advance(301s);
    const auto ordinaryAuthorization = authorizationFor(
        authority, context, exactId, "status-ordinary-idle");
    const auto ordinary = fixture.service.runStatus(
        {exactId, newOwner}, authority, ordinaryAuthorization, context);
    REQUIRE(ordinary);
    REQUIRE(ordinary.value().idleSeconds ==
            std::optional<std::int64_t>{301});
    REQUIRE(ordinary.value().abandonRisk);
    REQUIRE(!ordinary.value().reattached);

    auto foreignProjectRun = makeRun(
        sessionId("abababab-abab-4bab-8bab-abababababab"),
        oldOwner,
        fixture.clock.utcNow());
    foreignProjectRun.projectId = parse<Domain::ProjectId>(
        "99999999-9999-4999-8999-999999999999");
    fixture.repository.seed(foreignProjectRun);
    const auto foreignAuthorization = authorizationFor(
        authority,
        context,
        foreignProjectRun.session.id,
        "status-foreign-project");
    const auto foreignAttach = fixture.service.attach(
        {foreignProjectRun.session.id, newOwner},
        authority,
        foreignAuthorization,
        context);
    REQUIRE(!foreignAttach);
    REQUIRE(foreignAttach.error().code == Domain::ErrorCodes::Unauthorized);
    REQUIRE(fixture.repository.snapshot(foreignProjectRun.session.id)
                ->session.clientId == oldOwner);

    auto escapedRun = makeRun(
        sessionId("acacacac-acac-4cac-8cac-acacacacacac"),
        oldOwner,
        fixture.clock.utcNow());
    escapedRun.workingDirectory = path("C:/workspace-escape");
    fixture.repository.seed(escapedRun);
    const auto escapedAuthorization = authorizationFor(
        authority,
        context,
        escapedRun.session.id,
        "status-escaped-path");
    const auto escaped = fixture.service.runStatus(
        {escapedRun.session.id, newOwner},
        authority,
        escapedAuthorization,
        context);
    REQUIRE(!escaped);
    REQUIRE(escaped.error().code == Domain::ErrorCodes::PathOutsideAuthority);
    REQUIRE(fixture.repository.snapshot(escapedRun.session.id)
                ->session.clientId == oldOwner);

    auto reparseRun = makeRun(
        sessionId("afafafaf-afaf-4faf-8faf-afafafafafaf"),
        oldOwner,
        fixture.clock.utcNow());
    reparseRun.workingDirectory = path("C:/workspace/junction/elsewhere");
    fixture.repository.seed(reparseRun);
    const auto reparseAuthorization = authorizationFor(
        authority,
        context,
        reparseRun.session.id,
        "status-reparse-escape");
    const auto authorizationCalls = fixture.workspaceAuthority.authorizeCalls();
    fixture.workspaceAuthority.rejectNextResolvedPath();
    const auto reparseEscape = fixture.service.attach(
        {reparseRun.session.id, newOwner},
        authority,
        reparseAuthorization,
        context);
    REQUIRE(!reparseEscape);
    REQUIRE(reparseEscape.error().code ==
            Domain::ErrorCodes::PathOutsideAuthority);
    REQUIRE(fixture.workspaceAuthority.authorizeCalls() ==
            authorizationCalls + 1U);
    REQUIRE(fixture.repository.snapshot(reparseRun.session.id)
                ->session.clientId == oldOwner);

    const auto canonicalTarget = makeRun(
        sessionId("adadadad-adad-4dad-8dad-adadadadadad"),
        oldOwner,
        fixture.clock.utcNow());
    fixture.repository.seed(canonicalTarget);
    const auto canonicalDecoy =
        sessionId("aeaeaeae-aeae-4eae-8eae-aeaeaeaeaeae");
    const auto nestedOnlyAuthorization = authorizationFor(
        authority,
        context,
        canonicalDecoy,
        "status-canonical-session-binding",
        canonicalTarget.session.id);
    const auto nestedOnly = fixture.service.runStatus(
        {canonicalTarget.session.id, newOwner},
        authority,
        nestedOnlyAuthorization,
        context);
    REQUIRE(!nestedOnly);
    REQUIRE(nestedOnly.error().code == Domain::ErrorCodes::Unauthorized);
    REQUIRE(fixture.repository.snapshot(canonicalTarget.session.id)
                ->session.clientId == oldOwner);

    const auto missingA =
        sessionId("dddddddd-dddd-4ddd-8ddd-dddddddddddd");
    const auto missingB =
        sessionId("eeeeeeee-eeee-4eee-8eee-eeeeeeeeeeee");
    const auto reusableAuthorization = authorizationFor(
        authority,
        context,
        missingA,
        "status-global-one-use");
    const auto richMissing = fixture.service.runStatus(
        {missingA, newOwner}, authority, reusableAuthorization, context);
    REQUIRE(richMissing);
    REQUIRE(!richMissing.value().run);
    REQUIRE(richMissing.value().activeBinding);
    REQUIRE(richMissing.value().activeBinding->sessionId == exactId);
    const auto repeated = fixture.service.runStatus(
        {missingA, newOwner}, authority, reusableAuthorization, context);
    REQUIRE(!repeated);
    REQUIRE(repeated.error().code == Domain::ErrorCodes::Unauthorized);
    const auto crossed = fixture.service.runStatus(
        {missingB, newOwner}, authority, reusableAuthorization, context);
    REQUIRE(!crossed);
    REQUIRE(crossed.error().code == Domain::ErrorCodes::Unauthorized);

    const auto legacyAuthorization = authorizationFor(
        authority, context, missingB, "status-legacy-missing");
    const auto legacy = fixture.service.status(
        missingB, authority, legacyAuthorization, context);
    REQUIRE(!legacy);
    REQUIRE(legacy.error().code == Domain::ErrorCodes::SessionNotFound);

    const auto touchRace = take(fixture.service.startRun(
        startRequest(newOwner, "touch race"),
        fixture.context("touch-race-start")));
    fixture.repository.closeBeforeNextTouch();
    const auto touchRaceAuthorization = authorizationFor(
        authority,
        context,
        touchRace.run.session.id,
        "status-touch-race");
    const auto refreshedClosed = fixture.service.runStatus(
        {touchRace.run.session.id, newOwner},
        authority,
        touchRaceAuthorization,
        context);
    REQUIRE(refreshedClosed);
    REQUIRE(refreshedClosed.value().run);
    REQUIRE(!Domain::isOpen(refreshedClosed.value().run->session.status));
    REQUIRE(!refreshedClosed.value().mustComplete);
    REQUIRE(!refreshedClosed.value().idleSeconds);
    REQUIRE(!take(fixture.service.binding(
        newOwner, fixture.context("touch-race-binding"))));

    const auto touchFailure = take(fixture.service.startRun(
        startRequest(newOwner, "touch failure"),
        fixture.context("touch-failure-start")));
    fixture.repository.failNextTouch();
    const auto touchFailureAuthorization = authorizationFor(
        authority,
        context,
        touchFailure.run.session.id,
        "status-touch-failure");
    const auto failedTouch = fixture.service.runStatus(
        {touchFailure.run.session.id, newOwner},
        authority,
        touchFailureAuthorization,
        context);
    REQUIRE(!failedTouch);
    REQUIRE(failedTouch.error().code == Domain::ErrorCodes::InternalFailure);
    REQUIRE(Domain::isOpen(fixture.repository.snapshot(
        touchFailure.run.session.id)->session.status));

    const auto legacyStarted = take(fixture.service.start(
        agentId(),
        std::optional<Domain::ClientId>{oldOwner},
        "retained P05 projectless start",
        path("C:/workspace/legacy"),
        fixture.context("legacy-projectless-start")));
    const auto legacyContext = fixture.context("legacy-projectless-status");
    const auto legacyAuthority = authorityFor(oldOwner, legacyContext);
    const auto legacyProjectlessAuthorization = authorizationFor(
        legacyAuthority,
        legacyContext,
        legacyStarted.id,
        "legacy-projectless-status-request");
    const auto legacyProjectless = fixture.service.status(
        legacyStarted.id,
        legacyAuthority,
        legacyProjectlessAuthorization,
        legacyContext);
    REQUIRE(legacyProjectless);
    REQUIRE(legacyProjectless.value().id == legacyStarted.id);
    REQUIRE(legacyProjectless.value().clientId == oldOwner);

    const auto unscopedLegacy = take(fixture.service.start(
        agentId(),
        std::optional<Domain::ClientId>{oldOwner},
        "unscoped retained P05 start",
        std::nullopt,
        fixture.context("legacy-unscoped-start")));
    const auto unscopedContext = fixture.context("legacy-unscoped-status");
    const auto unscopedAuthority = authorityFor(oldOwner, unscopedContext);
    const auto unscopedAuthorization = authorizationFor(
        unscopedAuthority,
        unscopedContext,
        unscopedLegacy.id,
        "legacy-unscoped-status-request");
    const auto unscopedStatus = fixture.service.status(
        unscopedLegacy.id,
        unscopedAuthority,
        unscopedAuthorization,
        unscopedContext);
    REQUIRE(!unscopedStatus);
    REQUIRE(unscopedStatus.error().code == Domain::ErrorCodes::Unauthorized);
    REQUIRE(fixture.repository.snapshot(unscopedLegacy.id)
                ->session.clientId == oldOwner);
}

void completionUsesDurableSchemaAndEvictsPersistedOwner()
{
    Fixture fixture;
    fixture.catalog.setSpec(makeSpec(
        {"empty_string", "empty_array", "empty_object", "null_value", "number"}));
    const auto owner = clientId("completion-owner");
    const auto started = take(fixture.service.startRun(
        startRequest(owner, "long report"), fixture.context("complete-start")));
    std::string emoji;
    for (std::size_t index{}; index < 4'100U; ++index) {
        emoji += "\xF0\x9F\xA7\xAD";
    }
    const std::string json =
        std::string{"{\"details\":\""} + emoji +
        "\",\"empty_array\":[],\"empty_object\":{},\"empty_string\":\"\","
        "\"null_value\":null,\"number\":1}";
    const Domain::AgentCompletionReport report{
        json,
        {
            {"empty_string", Domain::AgentReportValueKind::String, 0U},
            {"empty_array", Domain::AgentReportValueKind::Array, 0U},
            {"empty_object", Domain::AgentReportValueKind::Object, 0U},
            {"null_value", Domain::AgentReportValueKind::Null, 0U},
            {"number", Domain::AgentReportValueKind::Number, 0U},
            {"details", Domain::AgentReportValueKind::String, emoji.size()},
        }};
    const auto unauthenticated = fixture.service.completeRun(
        {started.run.session.id, std::nullopt, report},
        fixture.context("complete-without-owner"));
    REQUIRE(!unauthenticated);
    REQUIRE(unauthenticated.error().code ==
            Domain::ErrorCodes::OwnershipConflict);
    REQUIRE(Domain::isOpen(fixture.repository.snapshot(
        started.run.session.id)->session.status));
    REQUIRE(take(fixture.service.binding(
        owner, fixture.context("binding-after-rejected-complete"))));

    const auto completed = fixture.service.completeRun(
        {started.run.session.id, owner, report},
        fixture.context("complete"));
    REQUIRE(completed);
    REQUIRE(!completed.value().schemaComplete);
    REQUIRE(completed.value().missingSchemaKeys ==
            std::vector<std::string>({
                "empty_string", "empty_array", "empty_object"}));
    REQUIRE(completed.value().report.fields.size() == 6U);
    REQUIRE(completed.value().report.fields[0].key == "details");
    REQUIRE(completed.value().report.fields[0].kind ==
            Domain::AgentReportValueKind::String);
    REQUIRE(completed.value().report.fields[0].logicalSize == emoji.size());
    REQUIRE(completed.value().report.fields[1].key == "empty_array");
    REQUIRE(completed.value().report.fields[1].kind ==
            Domain::AgentReportValueKind::Array);
    REQUIRE(completed.value().report.fields[2].key == "empty_object");
    REQUIRE(completed.value().report.fields[2].kind ==
            Domain::AgentReportValueKind::Object);
    REQUIRE(completed.value().report.fields[3].key == "empty_string");
    REQUIRE(completed.value().report.fields[3].kind ==
            Domain::AgentReportValueKind::String);
    REQUIRE(completed.value().report.fields[4].key == "null_value");
    REQUIRE(completed.value().report.fields[4].kind ==
            Domain::AgentReportValueKind::Null);
    REQUIRE(completed.value().report.fields[5].key == "number");
    REQUIRE(completed.value().report.fields[5].kind ==
            Domain::AgentReportValueKind::Number);
    REQUIRE(completed.value().run.session.summary);
    REQUIRE(Domain::isValidUtf8(*completed.value().run.session.summary));
    REQUIRE(utf8CodePoints(*completed.value().run.session.summary) <=
            Domain::AgentSessionLimits::MaximumSummaryUnits);
    REQUIRE(!take(fixture.service.binding(
        owner, fixture.context("binding-after-complete"))));

    const auto ownerless = take(fixture.service.startRun(
        Domain::AgentRunStartRequest{
            agentId(),
            std::nullopt,
            projectId(),
            "ownerless compatibility",
            path("C:/workspace")},
        fixture.context("ownerless-start")));
    const auto ownerlessLegacy = fixture.service.complete(
        ownerless.run.session.id,
        "ownerless completion",
        true,
        fixture.context("ownerless-legacy-complete"));
    REQUIRE(!ownerlessLegacy);
    REQUIRE(ownerlessLegacy.error().code ==
            Domain::ErrorCodes::OwnershipConflict);
    REQUIRE(Domain::isOpen(fixture.repository.snapshot(
        ownerless.run.session.id)->session.status));

    fixture.catalog.setSpec(makeSpec({"required"}));
    const auto inspected = take(fixture.service.startRun(
        startRequest(owner, "inspected report"),
        fixture.context("inspected-report-start")));
    const auto assertRejectedReport =
        [&](Domain::AgentCompletionReport candidate,
            const std::string_view suffix) {
            const auto rejected = fixture.service.completeRun(
                {inspected.run.session.id, owner, std::move(candidate)},
                fixture.context(suffix));
            REQUIRE(!rejected);
            REQUIRE(rejected.error().code ==
                    Domain::ErrorCodes::InvalidRequest);
            const auto durableRun = fixture.repository.snapshot(
                inspected.run.session.id);
            REQUIRE(durableRun);
            REQUIRE(Domain::isOpen(durableRun->session.status));
            REQUIRE(!durableRun->reportJson);
        };
    assertRejectedReport(
        Domain::AgentCompletionReport{
            "{\"required\":\"\"}",
            {{"required", Domain::AgentReportValueKind::String, 8U}}},
        "forged-report-metadata");
    assertRejectedReport(
        Domain::AgentCompletionReport{
            "{\"required\":}",
            {{"required", Domain::AgentReportValueKind::String, 0U}}},
        "malformed-report-json");
    assertRejectedReport(
        Domain::AgentCompletionReport{
            "{\"required\":\"\",\"required\":\"ok\"}",
            {{"required", Domain::AgentReportValueKind::String, 0U}}},
        "duplicate-report-key");
    assertRejectedReport(
        Domain::AgentCompletionReport{
            "{\"required\":{\"nested\":1,\"nested\":2}}",
            {{"required", Domain::AgentReportValueKind::Object, 1U}}},
        "nested-duplicate-report-key");
    std::string overNestedReport{"{\"required\":"};
    overNestedReport.append(66U, '[');
    overNestedReport += "null";
    overNestedReport.append(66U, ']');
    overNestedReport.push_back('}');
    assertRejectedReport(
        Domain::AgentCompletionReport{
            std::move(overNestedReport),
            {{"required", Domain::AgentReportValueKind::Array, 1U}}},
        "report-nesting-over-64");
    assertRejectedReport(
        Domain::AgentCompletionReport{
            "{\"required\": \"ok\"}",
            {{"required", Domain::AgentReportValueKind::String, 2U}}},
        "noncanonical-report-json");

    const auto unknown = fixture.service.completeRun(
        {sessionId("ffffffff-ffff-4fff-8fff-ffffffffffff"),
         std::nullopt,
         Domain::AgentCompletionReport{"{}", {}}},
        fixture.context("complete-missing"));
    REQUIRE(!unknown);
    REQUIRE(unknown.error().code == Domain::ErrorCodes::SessionNotFound);

    fixture.catalog.setSpec(makeSpec({"original"}));
    const auto durable = take(fixture.service.startRun(
        startRequest(owner, "durable schema"),
        fixture.context("durable-start")));
    fixture.catalog.setSpec(makeSpec({"replacement"}));
    const auto durableCompletion = fixture.service.completeRun(
        {durable.run.session.id,
         owner,
         Domain::AgentCompletionReport{
             "{\"original\":\"ok\"}",
             {{"original", Domain::AgentReportValueKind::String, 2U}}}},
        fixture.context("durable-complete"));
    REQUIRE(durableCompletion);
    REQUIRE(durableCompletion.value().schemaComplete);

    const auto ownership = take(fixture.service.startRun(
        startRequest(owner, "ownership"), fixture.context("owner-start")));
    const auto wrongOwner = fixture.service.completeRun(
        {ownership.run.session.id,
         clientId("different-owner"),
         Domain::AgentCompletionReport{
             "{\"replacement\":\"ok\"}",
             {{"replacement", Domain::AgentReportValueKind::String, 2U}}}},
        fixture.context("owner-conflict"));
    REQUIRE(!wrongOwner);
    REQUIRE(wrongOwner.error().code == Domain::ErrorCodes::OwnershipConflict);
}

void rehydrateRepairsOnlyWhenDurableGoalExists()
{
    Fixture fixture;
    const auto migratedClient = clientId("migrated-client");
    const auto migrated = makeRun(
        sessionId("12121212-1212-4212-8212-121212121212"),
        migratedClient,
        fixture.clock.utcNow(),
        std::nullopt);
    fixture.repository.seed(migrated);
    const auto noGoal = fixture.service.rehydrate(
        {migratedClient}, fixture.context("migrated-null-goal"));
    REQUIRE(noGoal);
    REQUIRE(noGoal.value().run);
    REQUIRE(!noGoal.value().run->goal);
    REQUIRE(!noGoal.value().binding);
    REQUIRE(fixture.repository.repairCalls() == 0U);

    const auto trusted = bindingFor(migrated, "trusted imported goal");
    fixture.repository.setProjection(migratedClient, trusted);
    const auto projected = fixture.service.rehydrate(
        {migratedClient}, fixture.context("migrated-projection"));
    REQUIRE(projected);
    REQUIRE(projected.value().binding);
    REQUIRE(projected.value().binding->goal == "trusted imported goal");
    REQUIRE(fixture.repository.repairCalls() == 0U);

    const auto repairClient = clientId("repair-client");
    const auto repairRun = makeRun(
        sessionId("13131313-1313-4313-8313-131313131313"),
        repairClient,
        fixture.clock.utcNow(),
        std::string{"repairable goal"});
    fixture.repository.seed(repairRun);
    const auto repaired = fixture.service.rehydrate(
        {repairClient}, fixture.context("repair-projection"));
    REQUIRE(repaired);
    REQUIRE(repaired.value().binding);
    REQUIRE(repaired.value().binding->goal == "repairable goal");
    REQUIRE(!repaired.value().projectionNeedsRepair);
    REQUIRE(fixture.repository.repairCalls() == 1U);

    const auto staleClient = clientId("stale-projection-client");
    const auto staleRun = makeRun(
        sessionId("14141414-1212-4212-8212-141414141414"),
        staleClient,
        fixture.clock.utcNow(),
        std::string{"durable catalog goal"});
    auto staleBinding = bindingFor(staleRun, "stale projected goal");
    staleBinding.toolsPrimary = {"stale-tool"};
    staleBinding.toolsForbidden.clear();
    fixture.repository.seed(staleRun, std::move(staleBinding));
    fixture.repository.markProjectionNeedsRepair(staleClient);
    const auto rebuilt = fixture.service.rehydrate(
        {staleClient}, fixture.context("stale-projection-rebuild"));
    REQUIRE(rebuilt);
    REQUIRE(rebuilt.value().binding);
    REQUIRE(rebuilt.value().binding->goal == "durable catalog goal");
    REQUIRE(rebuilt.value().binding->toolsPrimary ==
            std::vector<std::string>({"read", "edit"}));
    REQUIRE(rebuilt.value().binding->toolsForbidden ==
            std::vector<std::string>({"shell"}));
    REQUIRE(!rebuilt.value().projectionNeedsRepair);
    REQUIRE(fixture.repository.repairCalls() == 2U);
}

void pruneUsesStrictCutoffAndEvictsOnlyClosedBindings()
{
    Fixture fixture;
    const auto cutoff = fixture.clock.utcNow() - 4h;
    const auto exactClient = clientId("cutoff-exact-client");
    const auto oldClient = clientId("cutoff-old-client");
    const auto exact = makeRun(
        sessionId("14141414-1414-4414-8414-141414141414"),
        exactClient,
        cutoff);
    const auto old = makeRun(
        sessionId("15151515-1515-4515-8515-151515151515"),
        oldClient,
        cutoff - 1s);
    fixture.repository.seed(exact, bindingFor(exact));
    fixture.repository.seed(old, bindingFor(old));
    REQUIRE(fixture.service.rehydrate(
        {exactClient}, fixture.context("cache-exact")));
    REQUIRE(fixture.service.rehydrate(
        {oldClient}, fixture.context("cache-old")));
    REQUIRE(take(fixture.service.pruneStale(
                fixture.context("strict-prune"))) == 1U);
    REQUIRE(Domain::isOpen(
        fixture.repository.snapshot(exact.session.id)->session.status));
    REQUIRE(!Domain::isOpen(
        fixture.repository.snapshot(old.session.id)->session.status));
    REQUIRE(take(fixture.service.binding(
        exactClient, fixture.context("exact-binding"))));
    REQUIRE(!take(fixture.service.binding(
        oldClient, fixture.context("old-binding"))));
}

void bindingCacheIsBoundedAndDeterministicallyEvicted()
{
    Fixture fixture;
    std::vector<Domain::ClientId> clients;
    clients.reserve(Domain::AgentSessionLimits::MaximumMemoryBindings + 1U);
    for (std::size_t index{};
         index <= Domain::AgentSessionLimits::MaximumMemoryBindings;
         ++index) {
        std::array<char, 32> text{};
        const auto written = std::snprintf(
            text.data(), text.size(), "cache-client-%03zu", index);
        REQUIRE(written > 0);
        clients.push_back(clientId(text.data()));
        REQUIRE(fixture.service.startRun(
            startRequest(clients.back()), fixture.context("cache-start")));
    }
    REQUIRE(!take(fixture.service.binding(
        clients.front(), fixture.context("cache-first"))));
    REQUIRE(take(fixture.service.binding(
        clients.back(), fixture.context("cache-last"))));
}

void concurrentStartsCannotReverseCommittedCacheOrder()
{
    Fixture fixture;
    const auto client = clientId("race-client");
    fixture.repository.blockFirstStartAfterCommit();
    std::optional<Domain::Result<Domain::AgentRunStartOutcome>> first;
    std::optional<Domain::Result<Domain::AgentRunStartOutcome>> second;
    std::promise<void> secondAttempted;
    auto secondAttempt = secondAttempted.get_future();
    std::thread firstThread{[&]() {
        first.emplace(fixture.service.startRun(
            startRequest(client, "first"), fixture.context("race-first")));
    }};
    REQUIRE(fixture.repository.waitForFirstStartCommit(2s));
    std::thread secondThread{[&]() {
        secondAttempted.set_value();
        second.emplace(fixture.service.startRun(
            startRequest(client, "second"), fixture.context("race-second")));
    }};
    secondAttempt.wait();
    const auto secondEnteredEarly =
        fixture.repository.waitForStartCalls(2U, 100ms);
    fixture.repository.releaseFirstStart();
    firstThread.join();
    secondThread.join();
    REQUIRE(!secondEnteredEarly);
    REQUIRE(first && *first);
    REQUIRE(second && *second);
    const auto cached = take(fixture.service.binding(
        client, fixture.context("race-binding")));
    REQUIRE(cached);
    REQUIRE(cached->sessionId == second->value().run.session.id);
    REQUIRE(!Domain::isOpen(fixture.repository.snapshot(
        first->value().run.session.id)->session.status));
    REQUIRE(Domain::isOpen(fixture.repository.snapshot(
        second->value().run.session.id)->session.status));

    const auto serialized = take(fixture.service.startRun(
        startRequest(client, "serialized completion"),
        fixture.context("serialized-start")));
    fixture.repository.blockNextCompletionAfterCommit();
    std::optional<Domain::Result<Domain::AgentRunCompleteOutcome>> completion;
    std::thread completionThread{[&]() {
        completion.emplace(fixture.service.completeRun(
            {serialized.run.session.id,
             client,
             Domain::AgentCompletionReport{
                 "{\"result\":\"done\"}",
                 {{"result", Domain::AgentReportValueKind::String, 4U}}}},
            fixture.context("serialized-complete")));
    }};
    const auto completionCommitted =
        fixture.repository.waitForCompletionCommit(2s);
    const auto statusContext = fixture.context("serialized-status");
    const auto statusAuthority = authorityFor(client, statusContext);
    const auto statusAuthorization = authorizationFor(
        statusAuthority,
        statusContext,
        serialized.run.session.id,
        "serialized-status-request");
    auto statusFuture = std::async(std::launch::async, [&]() {
        return fixture.service.runStatus(
            {serialized.run.session.id, client},
            statusAuthority,
            statusAuthorization,
            statusContext);
    });
    const auto statusBlocked =
        statusFuture.wait_for(100ms) == std::future_status::timeout;
    fixture.repository.releaseCompletion();
    completionThread.join();
    const auto serializedStatus = statusFuture.get();

    REQUIRE(completionCommitted);
    REQUIRE(statusBlocked);
    REQUIRE(completion && *completion);
    REQUIRE(serializedStatus);
    REQUIRE(serializedStatus.value().run);
    REQUIRE(!Domain::isOpen(serializedStatus.value().run->session.status));
    REQUIRE(!serializedStatus.value().mustComplete);
}

void admittedLegacyCompletionDrainsThroughShutdown()
{
    Fixture fixture;
    const auto owner = clientId("legacy-shutdown-owner");
    const auto started = take(fixture.service.startRun(
        startRequest(owner, "legacy completion"),
        fixture.context("legacy-shutdown-start")));
    fixture.repository.blockNextGetRun();
    std::optional<Domain::Result<Domain::AgentSession>> completion;
    std::thread completionThread{[&]() {
        completion.emplace(fixture.service.complete(
            started.run.session.id,
            "completed while shutdown waited",
            true,
            fixture.context("legacy-shutdown-complete")));
    }};
    const auto entered = fixture.repository.waitForGetRun(2s);

    std::promise<void> shutdownAttempted;
    auto attempted = shutdownAttempted.get_future();
    std::thread shutdownThread{[&]() {
        shutdownAttempted.set_value();
        fixture.service.shutdown();
    }};
    attempted.wait();
    bool shutdownOwnsAdmission{};
    const auto probeDeadline = std::chrono::steady_clock::now() + 2s;
    while (std::chrono::steady_clock::now() < probeDeadline) {
        const auto probe = fixture.service.binding(
            owner, fixture.context("legacy-shutdown-probe"));
        if (!probe && probe.error().code == Domain::ErrorCodes::Cancelled) {
            shutdownOwnsAdmission = true;
            break;
        }
        std::this_thread::yield();
    }
    fixture.repository.releaseGetRun();
    completionThread.join();
    shutdownThread.join();

    REQUIRE(entered);
    REQUIRE(shutdownOwnsAdmission);
    REQUIRE(completion && *completion);
    REQUIRE(completion->value().status == Domain::SessionStatus::Closed);
    REQUIRE(fixture.repository.closeCalled());
}

void cancellationDeadlineAndShutdownOwnTheirBoundaries()
{
    Fixture fixture;
    const auto callsBefore = fixture.repository.startCalls();
    std::stop_source cancellation;
    cancellation.request_stop();
    auto cancelledContext = fixture.context("cancelled");
    cancelledContext.cancellation = cancellation.get_token();
    const auto cancelled = fixture.service.startRun(
        startRequest(clientId("cancelled-client")), cancelledContext);
    REQUIRE(!cancelled);
    REQUIRE(cancelled.error().code == Domain::ErrorCodes::Cancelled);
    REQUIRE(fixture.repository.startCalls() == callsBefore);

    auto expiredContext = fixture.context("expired");
    expiredContext.deadline = fixture.clock.monotonicNow();
    const auto expired = fixture.service.startRun(
        startRequest(clientId("expired-client")), expiredContext);
    REQUIRE(!expired);
    REQUIRE(expired.error().code == Domain::ErrorCodes::DeadlineExceeded);
    REQUIRE(fixture.repository.startCalls() == callsBefore);

    fixture.service.shutdown();
    REQUIRE(fixture.repository.closeCalled());
    const auto stopped = fixture.service.binding(
        clientId("stopped-client"), fixture.context("stopped"));
    REQUIRE(!stopped);
    REQUIRE(stopped.error().code == Domain::ErrorCodes::Cancelled);
}

} // namespace

int main()
{
    const std::vector<std::pair<std::string_view, void (*)()>> tests{
        {"start_prunes_before_atomic_commit_and_uses_stable_errors",
         startPrunesBeforeAtomicCommitAndUsesStableErrors},
        {"status_uses_observed_idle_and_authorization_is_globally_one_use",
         statusUsesObservedIdleAndAuthorizationIsGloballyOneUse},
        {"completion_uses_durable_schema_and_evicts_persisted_owner",
         completionUsesDurableSchemaAndEvictsPersistedOwner},
        {"rehydrate_repairs_only_when_durable_goal_exists",
         rehydrateRepairsOnlyWhenDurableGoalExists},
        {"prune_uses_strict_cutoff_and_evicts_only_closed_bindings",
         pruneUsesStrictCutoffAndEvictsOnlyClosedBindings},
        {"binding_cache_is_bounded_and_deterministically_evicted",
         bindingCacheIsBoundedAndDeterministicallyEvicted},
        {"concurrent_starts_cannot_reverse_committed_cache_order",
         concurrentStartsCannotReverseCommittedCacheOrder},
        {"admitted_legacy_completion_drains_through_shutdown",
         admittedLegacyCompletionDrainsThroughShutdown},
        {"cancellation_deadline_and_shutdown_own_their_boundaries",
         cancellationDeadlineAndShutdownOwnTheirBoundaries}};

    std::size_t failures{};
    for (const auto& [name, test] : tests) {
        try {
            test();
            std::cout << "PASS " << name << '\n';
        } catch (const std::exception& error) {
            ++failures;
            std::cerr << "FAIL " << name << ": " << error.what() << '\n';
        }
    }
    std::cout << "SUMMARY passed=" << (tests.size() - failures)
              << " failed=" << failures << '\n';
    return failures == 0U ? EXIT_SUCCESS : EXIT_FAILURE;
}
