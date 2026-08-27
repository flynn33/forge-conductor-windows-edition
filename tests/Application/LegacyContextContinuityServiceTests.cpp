#include "ForgeConductor/Application/LegacyContextContinuityService.h"
#include "ForgeConductor/Domain/Utf8.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <future>
#include <iostream>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace {

using namespace std::chrono_literals;
namespace Application = ForgeConductor::Application;
namespace Contracts = ForgeConductor::Contracts;
namespace Domain = ForgeConductor::Domain;

#define REQUIRE(condition)                                                        \
    do {                                                                          \
        if (!(condition)) {                                                       \
            throw std::runtime_error{                                             \
                std::string{"requirement failed: "} + #condition};              \
        }                                                                         \
    } while (false)

template <typename T>
[[nodiscard]] T take(Domain::Result<T> result)
{
    if (!result) {
        throw std::runtime_error{result.error().code + ": " + result.error().message};
    }
    return std::move(result).value();
}

void take(Domain::Result<void> result)
{
    if (!result) {
        throw std::runtime_error{result.error().code + ": " + result.error().message};
    }
}

[[nodiscard]] Domain::ClientId clientId(const std::string_view value)
{
    return take(Domain::ClientId::parse(value));
}

[[nodiscard]] Domain::AgentId agentId(const std::string_view value)
{
    return take(Domain::AgentId::parse(value));
}

[[nodiscard]] Domain::SessionId sessionId(const std::string_view value)
{
    return take(Domain::SessionId::parse(value));
}

[[nodiscard]] Domain::Sha256Digest zeroDigest()
{
    return take(Domain::Sha256Digest::parse(std::string(64U, '0')));
}

class FakeClock final : public Contracts::IClock {
public:
    [[nodiscard]] Domain::UtcTimePoint utcNow() const noexcept override
    {
        return utc_;
    }

    [[nodiscard]] Domain::MonotonicTimePoint monotonicNow() const noexcept override
    {
        return monotonic_;
    }

    void advance(const std::chrono::seconds value) noexcept
    {
        utc_ += value;
        monotonic_ += value;
    }

private:
    Domain::UtcTimePoint utc_{std::chrono::seconds{1'787'650'000}};
    Domain::MonotonicTimePoint monotonic_{10h};
};

class FakeUuidGenerator final : public Contracts::IUuidGenerator {
public:
    [[nodiscard]] Domain::Result<Domain::Uuid> next() noexcept override
    {
        try {
            std::lock_guard lock{mutex_};
            char text[37]{};
            const auto written = std::snprintf(
                text,
                sizeof(text),
                "%08x-0000-4000-8000-%012llx",
                counter_,
                static_cast<unsigned long long>(counter_));
            ++counter_;
            if (written != 36) {
                return Domain::Result<Domain::Uuid>::failure(Domain::makeError(
                    Domain::ErrorCodes::InternalFailure,
                    "The fake UUID could not be formatted."));
            }
            return Domain::Uuid::parse(text);
        } catch (...) {
            return Domain::Result<Domain::Uuid>::failure(Domain::makeError(
                Domain::ErrorCodes::InternalFailure,
                "The fake UUID generator failed."));
        }
    }

private:
    std::mutex mutex_;
    unsigned int counter_{1U};
};

class FakeLegacyRepository final
    : public Contracts::ILegacyContinuityRepository {
public:
    [[nodiscard]] Domain::Result<std::optional<Domain::LegacyContinuityRecord>>
    get(
        const Domain::LegacyHandoffId& handoffId,
        const Domain::OperationContext&) noexcept override
    {
        try {
            std::lock_guard lock{mutex_};
            ++getCalls_;
            const auto found = records_.find(handoffId.value());
            return Domain::Result<
                std::optional<Domain::LegacyContinuityRecord>>::success(
                found == records_.end()
                    ? std::nullopt
                    : std::optional<Domain::LegacyContinuityRecord>{found->second});
        } catch (...) {
            return failure<std::optional<Domain::LegacyContinuityRecord>>();
        }
    }

    [[nodiscard]] Domain::Result<std::optional<Domain::LegacyContinuityRecord>>
    latest(
        const std::optional<Domain::ClientId>& clientId,
        const bool resumeReadyOnly,
        const Domain::OperationContext&) noexcept override
    {
        try {
            std::lock_guard lock{mutex_};
            auto ordered = orderedUnlocked();
            for (const auto& record : ordered) {
                if (resumeReadyOnly && !record.packet.resumeReady) continue;
                if (clientId &&
                    (!record.packet.clientId ||
                     *record.packet.clientId != *clientId)) {
                    continue;
                }
                return Domain::Result<
                    std::optional<Domain::LegacyContinuityRecord>>::success(
                    record);
            }
            return Domain::Result<
                std::optional<Domain::LegacyContinuityRecord>>::success(
                std::nullopt);
        } catch (...) {
            return failure<std::optional<Domain::LegacyContinuityRecord>>();
        }
    }

    [[nodiscard]] Domain::Result<std::vector<Domain::LegacyContinuityRecord>>
    list(
        const std::size_t maximumCount,
        const Domain::OperationContext&) noexcept override
    {
        return listBounded(maximumCount);
    }

    [[nodiscard]] Domain::Result<std::vector<Domain::LegacyContinuityRecord>>
    listAll(
        const std::size_t maximumCount,
        const Domain::OperationContext&) noexcept override
    {
        return listBounded(maximumCount);
    }

    [[nodiscard]] Domain::Result<Domain::LegacyContinuityRecord> compareExchange(
        const Domain::LegacyContinuityCompareExchange& mutation,
        const Domain::OperationContext&) noexcept override
    {
        try {
            std::unique_lock lock{mutex_};
            ++compareCalls_;
            if (blockCompare_) {
                compareEntered_ = true;
                changed_.notify_all();
                changed_.wait(lock, [&]() { return !blockCompare_; });
            }

            auto found = records_.find(mutation.packet.id.value());
            if (conflictsRemaining_ != 0U) {
                --conflictsRemaining_;
                if (found != records_.end()) {
                    ++sequence_;
                    found->second.writeSequence = sequence_;
                    if (conflictDecision_) {
                        found->second.packet.decisions = {*conflictDecision_};
                        conflictDecision_.reset();
                    }
                }
                return conflict();
            }
            if (mutation.expectedWriteSequence) {
                if (found == records_.end() ||
                    found->second.writeSequence !=
                        *mutation.expectedWriteSequence) {
                    return conflict();
                }
            } else if (found != records_.end()) {
                return conflict();
            }

            Domain::LegacyContinuityDocuments documents{
                std::string{"{}"}, std::string{"{}"}, zeroDigest()};
            if (found != records_.end() && found->second.documents.packetJson) {
                documents.packetJson = found->second.documents.packetJson;
            }
            ++sequence_;
            Domain::LegacyContinuityRecord stored{
                mutation.packet, sequence_, std::move(documents)};
            records_.insert_or_assign(mutation.packet.id.value(), stored);
            return Domain::Result<Domain::LegacyContinuityRecord>::success(
                std::move(stored));
        } catch (...) {
            return failure<Domain::LegacyContinuityRecord>();
        }
    }

    [[nodiscard]] Domain::Result<Domain::LegacyContinuityPointerRepairOutcome>
    repairPointers(const Domain::OperationContext&) noexcept override
    {
        try {
            std::lock_guard lock{mutex_};
            auto ordered = orderedUnlocked();
            std::optional<Domain::LegacyHandoffId> latest;
            std::optional<Domain::LegacyHandoffId> resume;
            if (!ordered.empty()) latest = ordered.front().packet.id;
            for (const auto& record : ordered) {
                if (record.packet.resumeReady) {
                    resume = record.packet.id;
                    break;
                }
            }
            latestPointer_ = latest;
            resumePointer_ = resume;
            return Domain::Result<
                Domain::LegacyContinuityPointerRepairOutcome>::success(
                {std::move(latest), std::move(resume), 2U});
        } catch (...) {
            return failure<Domain::LegacyContinuityPointerRepairOutcome>();
        }
    }

    [[nodiscard]] Domain::Result<Domain::LegacyContinuityResetOutcome> reset(
        const Domain::DestructiveConfirmation&,
        const Domain::OperationContext&) noexcept override
    {
        try {
            std::lock_guard lock{mutex_};
            const auto removed = records_.size();
            records_.clear();
            latestPointer_.reset();
            resumePointer_.reset();
            return Domain::Result<Domain::LegacyContinuityResetOutcome>::success(
                {removed,
                 2U,
                 0U,
                 true,
                 false,
                 true,
                 std::nullopt});
        } catch (...) {
            return failure<Domain::LegacyContinuityResetOutcome>();
        }
    }

    [[nodiscard]] Domain::Result<void> quickCheck(
        const Domain::OperationContext&) noexcept override
    {
        return Domain::Result<void>::success();
    }

    void close() noexcept override
    {
        closed_.store(true, std::memory_order_release);
    }

    void setConflicts(const std::size_t count, std::optional<std::string> decision = {})
    {
        std::lock_guard lock{mutex_};
        conflictsRemaining_ = count;
        conflictDecision_ = std::move(decision);
    }

    void markImported(const Domain::LegacyHandoffId& id, std::string packetJson)
    {
        std::lock_guard lock{mutex_};
        auto& record = records_.at(id.value());
        record.documents = {
            std::move(packetJson), std::nullopt, std::nullopt};
    }

    [[nodiscard]] std::optional<Domain::LegacyContinuityRecord> snapshot(
        const Domain::LegacyHandoffId& id) const
    {
        std::lock_guard lock{mutex_};
        const auto found = records_.find(id.value());
        return found == records_.end()
            ? std::nullopt
            : std::optional<Domain::LegacyContinuityRecord>{found->second};
    }

    [[nodiscard]] std::size_t count() const
    {
        std::lock_guard lock{mutex_};
        return records_.size();
    }

    [[nodiscard]] std::size_t compareCalls() const noexcept
    {
        return compareCalls_.load(std::memory_order_acquire);
    }

    [[nodiscard]] std::size_t getCalls() const noexcept
    {
        return getCalls_.load(std::memory_order_acquire);
    }

    void blockNextCompare()
    {
        std::lock_guard lock{mutex_};
        blockCompare_ = true;
        compareEntered_ = false;
    }

    [[nodiscard]] bool waitForCompare(const std::chrono::milliseconds timeout)
    {
        std::unique_lock lock{mutex_};
        return changed_.wait_for(lock, timeout, [&]() { return compareEntered_; });
    }

    void releaseCompare()
    {
        std::lock_guard lock{mutex_};
        blockCompare_ = false;
        changed_.notify_all();
    }

    [[nodiscard]] bool closed() const noexcept
    {
        return closed_.load(std::memory_order_acquire);
    }

    bool ordinaryMemoryPreserved{true};
    bool tagsPreserved{true};
    bool linksPreserved{true};
    bool artifactsPreserved{true};
    bool metadataPreserved{true};

private:
    template <typename T>
    [[nodiscard]] static Domain::Result<T> failure()
    {
        return Domain::Result<T>::failure(Domain::makeError(
            Domain::ErrorCodes::InternalFailure,
            "The fake legacy repository failed."));
    }

    [[nodiscard]] static Domain::Result<Domain::LegacyContinuityRecord> conflict()
    {
        return Domain::Result<Domain::LegacyContinuityRecord>::failure(
            Domain::makeError(
                Domain::ErrorCodes::Conflict,
                "Injected compare-exchange conflict.",
                true));
    }

    [[nodiscard]] std::vector<Domain::LegacyContinuityRecord> orderedUnlocked() const
    {
        std::vector<Domain::LegacyContinuityRecord> ordered;
        ordered.reserve(records_.size());
        for (const auto& [id, record] : records_) {
            (void)id;
            ordered.push_back(record);
        }
        std::sort(ordered.begin(), ordered.end(), [](const auto& left, const auto& right) {
            if (left.writeSequence != right.writeSequence) {
                return left.writeSequence > right.writeSequence;
            }
            return left.packet.id.value() < right.packet.id.value();
        });
        return ordered;
    }

    [[nodiscard]] Domain::Result<std::vector<Domain::LegacyContinuityRecord>>
    listBounded(const std::size_t maximumCount) noexcept
    {
        try {
            std::lock_guard lock{mutex_};
            auto ordered = orderedUnlocked();
            if (ordered.size() > maximumCount) {
                ordered.erase(
                    ordered.begin() + static_cast<std::ptrdiff_t>(maximumCount),
                    ordered.end());
            }
            return Domain::Result<
                std::vector<Domain::LegacyContinuityRecord>>::success(
                std::move(ordered));
        } catch (...) {
            return failure<std::vector<Domain::LegacyContinuityRecord>>();
        }
    }

    mutable std::mutex mutex_;
    std::condition_variable changed_;
    std::map<std::string, Domain::LegacyContinuityRecord> records_;
    std::uint64_t sequence_{};
    std::size_t conflictsRemaining_{};
    std::optional<std::string> conflictDecision_;
    std::optional<Domain::LegacyHandoffId> latestPointer_;
    std::optional<Domain::LegacyHandoffId> resumePointer_;
    bool blockCompare_{};
    bool compareEntered_{};
    std::atomic<std::size_t> compareCalls_{};
    std::atomic<std::size_t> getCalls_{};
    std::atomic<bool> closed_{};
};

class FakeProjectionStore final : public Contracts::IContinuityProjectionStore {
public:
    [[nodiscard]] Domain::Result<Domain::LegacyContinuityProjectionReceipt> write(
        const Domain::LegacyContinuityRecord& record,
        const Domain::OperationContext&) noexcept override
    {
        try {
            std::lock_guard lock{mutex_};
            if (failNextWrite_) {
                failNextWrite_ = false;
                return Domain::Result<
                    Domain::LegacyContinuityProjectionReceipt>::failure(
                    Domain::makeError(
                        Domain::ErrorCodes::StorageFull,
                        "Injected projection failure."));
            }
            packets_.insert_or_assign(record.packet.id.value(), record);
            if (record.writeSequence >= latestSequence_) {
                latestSequence_ = record.writeSequence;
                latestId_ = record.packet.id;
            }
            return Domain::Result<
                Domain::LegacyContinuityProjectionReceipt>::success(
                {"memory/handoffs/" + record.packet.id.value() + ".json",
                 "memory/current-task.md"});
        } catch (...) {
            return Domain::Result<
                Domain::LegacyContinuityProjectionReceipt>::failure(
                Domain::makeError(
                    Domain::ErrorCodes::InternalFailure,
                    "The fake projection store failed."));
        }
    }

    [[nodiscard]] Domain::Result<Domain::LegacyContinuityProjectionRepairOutcome>
    repair(
        const std::vector<Domain::LegacyContinuityRecord>& records,
        const Domain::LegacyContinuityPointerRepairOutcome& pointers,
        const Domain::OperationContext&) noexcept override
    {
        try {
            std::lock_guard lock{mutex_};
            packets_.clear();
            latestSequence_ = 0U;
            latestId_.reset();
            for (const auto& record : records) {
                packets_.insert_or_assign(record.packet.id.value(), record);
                if (record.writeSequence >= latestSequence_) {
                    latestSequence_ = record.writeSequence;
                }
            }
            latestId_ = pointers.latestId;
            return Domain::Result<
                Domain::LegacyContinuityProjectionRepairOutcome>::success(
                {records.size(), !records.empty(), !records.empty()});
        } catch (...) {
            return Domain::Result<
                Domain::LegacyContinuityProjectionRepairOutcome>::failure(
                Domain::makeError(
                    Domain::ErrorCodes::InternalFailure,
                    "The fake projection repair failed."));
        }
    }

    [[nodiscard]] Domain::Result<std::size_t> reset(
        const Domain::DestructiveConfirmation&,
        const Domain::OperationContext&) noexcept override
    {
        try {
            std::lock_guard lock{mutex_};
            if (failNextReset_) {
                failNextReset_ = false;
                return Domain::Result<std::size_t>::failure(Domain::makeError(
                    Domain::ErrorCodes::StorageFull,
                    "Injected projection reset failure."));
            }
            const auto count = packets_.size();
            packets_.clear();
            latestId_.reset();
            latestSequence_ = 0U;
            return Domain::Result<std::size_t>::success(count);
        } catch (...) {
            return Domain::Result<std::size_t>::failure(Domain::makeError(
                Domain::ErrorCodes::InternalFailure,
                "The fake projection reset failed."));
        }
    }

    void close() noexcept override
    {
        closed_.store(true, std::memory_order_release);
    }

    void failNextWrite()
    {
        std::lock_guard lock{mutex_};
        failNextWrite_ = true;
    }

    void failNextReset()
    {
        std::lock_guard lock{mutex_};
        failNextReset_ = true;
    }

    [[nodiscard]] std::size_t packetCount() const
    {
        std::lock_guard lock{mutex_};
        return packets_.size();
    }

    [[nodiscard]] std::optional<Domain::LegacyHandoffId> latestId() const
    {
        std::lock_guard lock{mutex_};
        return latestId_;
    }

    [[nodiscard]] bool closed() const noexcept
    {
        return closed_.load(std::memory_order_acquire);
    }

private:
    mutable std::mutex mutex_;
    std::map<std::string, Domain::LegacyContinuityRecord> packets_;
    std::optional<Domain::LegacyHandoffId> latestId_;
    std::uint64_t latestSequence_{};
    bool failNextWrite_{};
    bool failNextReset_{};
    std::atomic<bool> closed_{};
};

class FakeSessionSource final
    : public Contracts::ILegacyContinuitySessionSource {
public:
    [[nodiscard]] Domain::Result<std::size_t> countOpen(
        const std::size_t maximumCount,
        const Domain::OperationContext&) noexcept override
    {
        std::lock_guard lock{mutex_};
        if (open_.size() > maximumCount) {
            return Domain::Result<std::size_t>::failure(Domain::makeError(
                Domain::ErrorCodes::LimitExceeded,
                "The fake global open-session count exceeds its bound."));
        }
        return Domain::Result<std::size_t>::success(open_.size());
    }

    [[nodiscard]] Domain::Result<
        std::vector<Domain::LegacyAgentContinuitySnapshot>>
    listOpenForClient(
        const Domain::ClientId& clientId,
        const std::size_t,
        const Domain::OperationContext&) noexcept override
    {
        try {
            std::lock_guard lock{mutex_};
            lastQueriedClient_ = clientId.value();
            const auto found = snapshots_.find(clientId.value());
            return Domain::Result<
                std::vector<Domain::LegacyAgentContinuitySnapshot>>::success(
                found == snapshots_.end()
                    ? std::vector<Domain::LegacyAgentContinuitySnapshot>{}
                    : found->second);
        } catch (...) {
            return Domain::Result<
                std::vector<Domain::LegacyAgentContinuitySnapshot>>::failure(
                Domain::makeError(
                    Domain::ErrorCodes::InternalFailure,
                    "The fake session source failed."));
        }
    }

    [[nodiscard]] Domain::Result<bool> isOpen(
        const Domain::SessionId& sessionId,
        const Domain::OperationContext&) noexcept override
    {
        std::lock_guard lock{mutex_};
        return Domain::Result<bool>::success(
            open_.contains(sessionId.value()));
    }

    [[nodiscard]] Domain::Result<
        std::optional<Domain::LegacyActiveBindingSnapshot>>
    binding(
        const Domain::ClientId& clientId,
        const Domain::OperationContext&) noexcept override
    {
        try {
            std::lock_guard lock{mutex_};
            const auto found = bindings_.find(clientId.value());
            return Domain::Result<
                std::optional<Domain::LegacyActiveBindingSnapshot>>::success(
                found == bindings_.end()
                    ? std::nullopt
                    : std::optional<Domain::LegacyActiveBindingSnapshot>{
                          found->second});
        } catch (...) {
            return Domain::Result<
                std::optional<Domain::LegacyActiveBindingSnapshot>>::failure(
                Domain::makeError(
                    Domain::ErrorCodes::InternalFailure,
                    "The fake binding source failed."));
        }
    }

    void setSnapshots(
        const Domain::ClientId& clientId,
        std::vector<Domain::LegacyAgentContinuitySnapshot> snapshots)
    {
        std::lock_guard lock{mutex_};
        for (const auto& snapshot : snapshots) {
            open_.insert(snapshot.sessionId.value());
        }
        snapshots_.insert_or_assign(clientId.value(), std::move(snapshots));
    }

    void closeSession(const Domain::SessionId& id)
    {
        std::lock_guard lock{mutex_};
        open_.erase(id.value());
    }

    void setBinding(
        const Domain::ClientId& clientId,
        Domain::LegacyActiveBindingSnapshot binding)
    {
        std::lock_guard lock{mutex_};
        bindings_.insert_or_assign(clientId.value(), std::move(binding));
    }

    [[nodiscard]] std::string lastQueriedClient() const
    {
        std::lock_guard lock{mutex_};
        return lastQueriedClient_;
    }

private:
    mutable std::mutex mutex_;
    std::map<std::string, std::vector<Domain::LegacyAgentContinuitySnapshot>>
        snapshots_;
    std::map<std::string, Domain::LegacyActiveBindingSnapshot> bindings_;
    std::set<std::string> open_;
    std::string lastQueriedClient_;
};

[[nodiscard]] Domain::LegacyAgentContinuitySnapshot snapshot(
    const std::string_view session,
    const std::string_view agent,
    const std::string_view goal = "goal")
{
    return Domain::LegacyAgentContinuitySnapshot{
        sessionId(session),
        agentId(agent),
        std::string{goal},
        std::string{"C:/workspace"},
        "open",
        Domain::UtcTimePoint{std::chrono::seconds{1'787'650'000}},
        "agent_run_status then continue"};
}

struct Fixture final {
    Fixture()
        : service{repository, projections, sessions, clock, uuids}
    {
    }

    [[nodiscard]] Domain::OperationContext context(
        const std::string_view correlation = "legacy-continuity-test") const
    {
        return Domain::OperationContext{
            take(Domain::OperationId::parse(
                "aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa")),
            clock.monotonicNow() + 30s,
            {},
            take(Domain::CorrelationId::parse(correlation))};
    }

    FakeLegacyRepository repository;
    FakeProjectionStore projections;
    FakeSessionSource sessions;
    FakeClock clock;
    FakeUuidGenerator uuids;
    Application::LegacyContextContinuityService service;
};

void checkpointHandoffGetListAndProjectionFailure()
{
    Fixture fixture;
    const auto owner = clientId("legacy-owner");
    const auto firstSession = sessionId(
        "11111111-1111-4111-8111-111111111111");
    fixture.sessions.setSnapshots(
        owner,
        {snapshot(firstSession.value(), "debug", "Trace continuity")});
    fixture.sessions.setBinding(
        owner,
        {firstSession, "Binding goal", std::string{"C:/binding"}});

    Domain::LegacyContinuityPatch initialPatch;
    initialPatch.goal = "Port context continuity";
    initialPatch.status = "investigating";
    initialPatch.workingDirectory = "C:/workspace";
    initialPatch.nextActions = std::vector<std::string>{"Read source", "Write tests"};
    initialPatch.narrative = "Durable context";
    const auto checkpoint = take(fixture.service.checkpoint(
        {std::nullopt, std::move(initialPatch)},
        owner,
        Domain::LegacyHandoffSource::Model,
        fixture.context("checkpoint")));
    REQUIRE(!checkpoint.record.packet.resumeReady);
    REQUIRE(!checkpoint.handoffRequired);
    REQUIRE(checkpoint.projectionOk);
    REQUIRE(checkpoint.record.packet.agents.size() == 1U);
    REQUIRE(checkpoint.record.packet.clientId == owner);
    REQUIRE(checkpoint.record.documents.packetJson);
    REQUIRE(checkpoint.record.documents.payloadJson);
    REQUIRE(checkpoint.record.documents.contentSha256);

    const auto loaded = take(fixture.service.get(
        {{}, false}, fixture.context("get-latest")));
    REQUIRE(loaded.record);
    REQUIRE(loaded.record->packet.id == checkpoint.record.packet.id);

    Domain::LegacyContinuityPatch finalPatch;
    finalPatch.status = "ready_for_new_chat";
    const auto handoff = take(fixture.service.handoff(
        {std::nullopt, std::move(finalPatch)},
        owner,
        Domain::LegacyHandoffSource::Model,
        fixture.context("handoff")));
    REQUIRE(handoff.record.packet.id == checkpoint.record.packet.id);
    REQUIRE(handoff.record.packet.resumeReady);
    REQUIRE(handoff.handoffRequired);
    REQUIRE(handoff.record.packet.goal == "Port context continuity");

    fixture.projections.failNextWrite();
    Domain::LegacyContinuityPatch durablePatch;
    durablePatch.goal = "Durable despite projection failure";
    const auto durable = take(fixture.service.checkpoint(
        {handoff.record.packet.id, std::move(durablePatch)},
        owner,
        Domain::LegacyHandoffSource::Model,
        fixture.context("projection-failure")));
    REQUIRE(!durable.projectionOk);
    REQUIRE(durable.projectionRepairPending);
    REQUIRE(durable.projectionWarning);
    REQUIRE(fixture.repository.snapshot(handoff.record.packet.id)->packet.goal ==
            "Durable despite projection failure");

    const auto missingId = take(Domain::LegacyHandoffId::parse("missing-context"));
    Domain::LegacyContinuityPatch mustNotWrite;
    mustNotWrite.goal = "Must not create unknown explicit state";
    const auto rejected = fixture.service.checkpoint(
        {missingId, std::move(mustNotWrite)},
        owner,
        Domain::LegacyHandoffSource::Model,
        fixture.context("unknown-explicit"));
    REQUIRE(!rejected);
    REQUIRE(rejected.error().code == Domain::ErrorCodes::RecordNotFound);
    const auto missing = take(fixture.service.get(
        {missingId, false}, fixture.context("missing-explicit-get")));
    REQUIRE(missing.explicitIdRequested);
    REQUIRE(!missing.record);
    REQUIRE(fixture.repository.count() == 1U);

    const auto listed = take(fixture.service.list(
        {1000}, fixture.context("list")));
    REQUIRE(listed.handoffs.size() == 1U);
    REQUIRE(listed.handoffs.front().id == handoff.record.packet.id);
    REQUIRE(listed.handoffs.front().agentCount == 1U);
}

void statusSummaryMatchesLegacyObservableFields()
{
    Fixture fixture;
    const auto empty = take(fixture.service.statusSummary(
        fixture.context("status-empty")));
    REQUIRE(!empty.latestId);
    REQUIRE(!empty.latestUpdatedAt);
    REQUIRE(!empty.resumeReady);
    REQUIRE(!empty.resumeId);
    REQUIRE(empty.openAgentSessions == 0U);
    const std::vector<std::string> expectedTools{
        "session_checkpoint", "session_handoff", "context_get", "context_list"};
    REQUIRE(empty.tools == expectedTools);
    REQUIRE(empty.note ==
            "New chat bootstrap: call context_get over stdio MCP (forge-conductor).");
    REQUIRE(empty.automatic.checkpointEveryTools == 50U);
    REQUIRE(empty.automatic.handoffEveryTools == 200U);
    REQUIRE(empty.automatic.note ==
            "Forge writes checkpoints and handoffs from tool progress; the model does not have to call session_*.");

    const auto owner = clientId("status-owner");
    fixture.sessions.setSnapshots(
        owner,
        {snapshot("51111111-1111-4111-8111-111111111111", "debug"),
         snapshot("52222222-2222-4222-8222-222222222222", "review")});
    const auto checkpoint = take(fixture.service.checkpoint(
        {std::nullopt,
         Domain::LegacyContinuityPatch{std::string{"Status checkpoint"}}},
        owner,
        Domain::LegacyHandoffSource::Model,
        fixture.context("status-checkpoint")));
    fixture.clock.advance(1s);
    const auto handoff = take(fixture.service.handoff(
        {std::nullopt,
         Domain::LegacyContinuityPatch{std::string{"Status handoff"}}},
        owner,
        Domain::LegacyHandoffSource::Model,
        fixture.context("status-handoff")));

    const auto summary = take(fixture.service.statusSummary(
        fixture.context("status-populated")));
    REQUIRE(summary.latestId == handoff.record.packet.id);
    REQUIRE(summary.latestUpdatedAt == handoff.record.packet.updatedAt);
    REQUIRE(summary.resumeReady);
    REQUIRE(summary.resumeId == handoff.record.packet.id);
    REQUIRE(summary.openAgentSessions == 2U);
    REQUIRE(summary.latestId == checkpoint.record.packet.id);
    REQUIRE(summary.latestUpdatedAt != checkpoint.record.packet.updatedAt);
}

void explicitContinuationPreservesOnlyStillOpenAgentsAndCapsSnapshots()
{
    Fixture fixture;
    const auto original = clientId("original-client");
    const auto resumed = clientId("resumed-client");
    const auto originalSession = sessionId(
        "22222222-2222-4222-8222-222222222222");
    const auto resumedSession = sessionId(
        "33333333-3333-4333-8333-333333333333");
    fixture.sessions.setSnapshots(
        original,
        {snapshot(originalSession.value(), "debug", "Original specialist")});
    auto first = take(fixture.service.handoff(
        {std::nullopt,
         Domain::LegacyContinuityPatch{
             std::string{"Preserve specialists"}}},
        original,
        Domain::LegacyHandoffSource::Model,
        fixture.context("original-handoff")));

    fixture.sessions.setSnapshots(
        resumed,
        {snapshot(resumedSession.value(), "review", "Reattached specialist")});
    Domain::LegacyContinuityPatch resumePatch;
    resumePatch.status = "resuming";
    const auto continued = take(fixture.service.checkpoint(
        {first.record.packet.id, std::move(resumePatch)},
        resumed,
        Domain::LegacyHandoffSource::Model,
        fixture.context("explicit-continuation")));
    REQUIRE(fixture.sessions.lastQueriedClient() == resumed.value());
    REQUIRE(continued.record.packet.clientId == resumed);
    REQUIRE(continued.record.packet.agents.size() == 2U);
    REQUIRE(continued.record.packet.agents[0].sessionId == originalSession);
    REQUIRE(continued.record.packet.agents[1].sessionId == resumedSession);

    fixture.sessions.closeSession(originalSession);
    const auto afterClose = take(fixture.service.checkpoint(
        {first.record.packet.id, {}},
        resumed,
        Domain::LegacyHandoffSource::Model,
        fixture.context("closed-prior")));
    REQUIRE(afterClose.record.packet.agents.size() == 1U);
    REQUIRE(afterClose.record.packet.agents.front().sessionId == resumedSession);

    const auto overflowClient = clientId("overflow-client");
    std::vector<Domain::LegacyAgentContinuitySnapshot> overflow;
    for (std::size_t index{};
         index <= Domain::LegacyContinuityLimits::MaximumAgentSnapshots;
         ++index) {
        char id[37]{};
        const auto written = std::snprintf(
            id,
            sizeof(id),
            "%08x-4444-4444-8444-%012llx",
            static_cast<unsigned int>(index + 1U),
            static_cast<unsigned long long>(index + 1U));
        REQUIRE(written == 36);
        overflow.push_back(snapshot(id, "debug"));
    }
    fixture.sessions.setSnapshots(overflowClient, std::move(overflow));
    const auto rejected = fixture.service.checkpoint(
        {std::nullopt, {}},
        overflowClient,
        Domain::LegacyHandoffSource::Model,
        fixture.context("snapshot-overflow"));
    REQUIRE(!rejected);
    REQUIRE(rejected.error().code == Domain::ErrorCodes::IntegrityFailure);
}

void compareExchangeMergesDisjointFieldsAndBoundsConflicts()
{
    Fixture fixture;
    const auto owner = clientId("cas-owner");
    Domain::LegacyContinuityPatch initial;
    initial.goal = "Merge concurrent edits";
    const auto created = take(fixture.service.checkpoint(
        {std::nullopt, std::move(initial)},
        owner,
        Domain::LegacyHandoffSource::Model,
        fixture.context("cas-create")));

    fixture.repository.setConflicts(1U, "Concurrent decision");
    Domain::LegacyContinuityPatch blocker;
    blocker.blockers = std::vector<std::string>{"Caller blocker"};
    const auto merged = take(fixture.service.checkpoint(
        {created.record.packet.id, std::move(blocker)},
        owner,
        Domain::LegacyHandoffSource::Model,
        fixture.context("cas-merge")));
    REQUIRE(merged.record.packet.goal == "Merge concurrent edits");
    REQUIRE(merged.record.packet.blockers ==
            std::vector<std::string>{"Caller blocker"});
    REQUIRE(merged.record.packet.decisions ==
            std::vector<std::string>{"Concurrent decision"});

    fixture.repository.setConflicts(
        Domain::LegacyContinuityLimits::MaximumConflictRetries);
    Domain::LegacyContinuityPatch eight;
    eight.status = "after-eight-conflicts";
    const auto accepted = fixture.service.checkpoint(
        {created.record.packet.id, std::move(eight)},
        owner,
        Domain::LegacyHandoffSource::Model,
        fixture.context("eight-conflicts"));
    REQUIRE(accepted);

    fixture.repository.setConflicts(
        Domain::LegacyContinuityLimits::MaximumConflictRetries + 1U);
    Domain::LegacyContinuityPatch ninth;
    ninth.status = "must-not-commit";
    const auto exhausted = fixture.service.checkpoint(
        {created.record.packet.id, std::move(ninth)},
        owner,
        Domain::LegacyHandoffSource::Model,
        fixture.context("nine-conflicts"));
    REQUIRE(!exhausted);
    REQUIRE(exhausted.error().code == Domain::ErrorCodes::Conflict);
}

void resumeSeedsAutomationNarrativeAndImportedDocumentsRemainCompatible()
{
    Fixture fixture;
    const auto owner = clientId("seed-owner");
    Domain::LegacyContinuityPatch initial;
    initial.goal = "Old goal";
    initial.nextActions = std::vector<std::string>{"Old action"};
    auto packet = take(fixture.service.checkpoint(
        {std::nullopt, std::move(initial)},
        owner,
        Domain::LegacyHandoffSource::Model,
        fixture.context("seed-initial")));
    REQUIRE(packet.record.packet.resumeSeed.find("Old goal") != std::string::npos);

    Domain::LegacyContinuityPatch updated;
    updated.goal = "Current goal";
    updated.nextActions = std::vector<std::string>{"Current action"};
    packet = take(fixture.service.checkpoint(
        {packet.record.packet.id, std::move(updated)},
        owner,
        Domain::LegacyHandoffSource::Model,
        fixture.context("seed-regenerated")));
    REQUIRE(packet.record.packet.resumeSeed.find("Current goal") != std::string::npos);
    REQUIRE(packet.record.packet.resumeSeed.find("Old goal") == std::string::npos);

    Domain::LegacyContinuityPatch custom;
    custom.resumeSeed = "Operator recovery sequence";
    packet = take(fixture.service.checkpoint(
        {packet.record.packet.id, std::move(custom)},
        owner,
        Domain::LegacyHandoffSource::Model,
        fixture.context("custom-seed")));
    REQUIRE(packet.record.packet.resumeSeedIsCustom);
    Domain::LegacyContinuityPatch newGoal;
    newGoal.goal = "Goal after custom seed";
    packet = take(fixture.service.checkpoint(
        {packet.record.packet.id, std::move(newGoal)},
        owner,
        Domain::LegacyHandoffSource::Model,
        fixture.context("preserve-custom")));
    REQUIRE(packet.record.packet.resumeSeed == "Operator recovery sequence");

    fixture.repository.markImported(
        packet.record.packet.id,
        "{\"schema_version\":1,\"legacy\":true}");
    Domain::LegacyContinuityPatch importedUpdate;
    importedUpdate.decisions = std::vector<std::string>{"Retain imported source"};
    packet = take(fixture.service.checkpoint(
        {packet.record.packet.id, std::move(importedUpdate)},
        owner,
        Domain::LegacyHandoffSource::Model,
        fixture.context("imported-update")));
    REQUIRE(packet.record.documents.packetJson ==
            "{\"schema_version\":1,\"legacy\":true}");
    REQUIRE(packet.record.documents.payloadJson);
    REQUIRE(packet.record.documents.contentSha256);

    const std::string unicode = "\xC3\xA9";
    Domain::LegacyContinuityPatch narrative;
    narrative.narrative = std::string{};
    for (std::size_t index{}; index < 5'000U; ++index) {
        narrative.narrative->append(unicode);
    }
    packet = take(fixture.service.checkpoint(
        {packet.record.packet.id, std::move(narrative)},
        owner,
        Domain::LegacyHandoffSource::Model,
        fixture.context("narrative-cap")));
    REQUIRE(packet.record.packet.narrative.size() == 8'000U);
    REQUIRE(Domain::isValidUtf8(packet.record.packet.narrative));

    auto oversizedSeedPacket = packet.record.packet;
    oversizedSeedPacket.goal.assign(
        Domain::LegacyContinuityLimits::MaximumTextBytes, 'g');
    oversizedSeedPacket.projectSlug = std::string(
        Domain::LegacyContinuityLimits::MaximumTextBytes, 'p');
    oversizedSeedPacket.workingDirectory = std::string(
        Domain::LegacyContinuityLimits::MaximumTextBytes, 'w');
    oversizedSeedPacket.nextActions.assign(
        8U,
        std::string(Domain::LegacyContinuityLimits::MaximumItemBytes, 'a'));
    oversizedSeedPacket.resumeSeed.clear();
    oversizedSeedPacket.resumeSeedIsCustom = false;
    const auto boundedSeed = take(Domain::makeLegacyDefaultResumeSeed(
        oversizedSeedPacket));
    REQUIRE(boundedSeed.size() <=
            Domain::LegacyContinuityLimits::MaximumResumeSeedBytes);
    REQUIRE(Domain::isValidUtf8(boundedSeed));
    REQUIRE(boundedSeed.ends_with(
        "Call context_get for the full structured packet, then continue the task."));

    Domain::LegacyContinuityPatch inferred;
    inferred.goal = "Must not replace rich goal";
    inferred.narrative = "Must not replace rich narrative";
    inferred.keyFiles = std::vector<std::string>{"inferred.cpp"};
    const auto automatic = take(fixture.service.automaticPersist(
        {std::move(inferred), "tool progress", false},
        owner,
        fixture.context("automatic-fill-only")));
    REQUIRE(automatic.record.packet.goal == "Goal after custom seed");
    REQUIRE(automatic.record.packet.narrative.size() == 8'000U);
    REQUIRE(automatic.record.packet.keyFiles ==
            std::vector<std::string>{"inferred.cpp"});
    REQUIRE(automatic.record.packet.source == Domain::LegacyHandoffSource::Model);
    REQUIRE(automatic.record.packet.clientId == owner);

    Domain::LegacyContinuityPatch shortNarrative;
    shortNarrative.narrative = "Evidence before budget trigger";
    packet = take(fixture.service.checkpoint(
        {packet.record.packet.id, std::move(shortNarrative)},
        owner,
        Domain::LegacyHandoffSource::Model,
        fixture.context("short-budget-narrative")));

    const auto budget = take(fixture.service.budgetHandoff(
        owner,
        "context pressure",
        fixture.context("budget")));
    REQUIRE(budget.record.packet.id == packet.record.packet.id);
    REQUIRE(budget.record.packet.resumeReady);
    REQUIRE(budget.record.packet.resumeSeed == "Operator recovery sequence");
    REQUIRE(budget.record.packet.narrative.find("Budget trigger: context pressure") !=
            std::string::npos);
}

void repairAndScopedConvergentResetPreserveUnrelatedData()
{
    Fixture fixture;
    const auto firstClient = clientId("repair-first");
    const auto secondClient = clientId("repair-second");
    auto first = take(fixture.service.handoff(
        {std::nullopt,
         Domain::LegacyContinuityPatch{std::string{"First"}}},
        firstClient,
        Domain::LegacyHandoffSource::Model,
        fixture.context("repair-first")));
    fixture.clock.advance(1s);
    auto second = take(fixture.service.handoff(
        {std::nullopt,
         Domain::LegacyContinuityPatch{std::string{"Second"}}},
        secondClient,
        Domain::LegacyHandoffSource::Model,
        fixture.context("repair-second")));
    auto tieFirst = first.record;
    auto tieSecond = second.record;
    tieFirst.writeSequence = 42U;
    tieSecond.writeSequence = 42U;
    std::vector<Domain::LegacyContinuityRecord> tied{
        std::move(tieFirst), std::move(tieSecond)};
    std::sort(tied.begin(), tied.end(), [](const auto& left, const auto& right) {
        return left.packet.id.value() < right.packet.id.value();
    });
    take(Domain::validateLegacyContinuityList(tied, tied.size()));
    std::reverse(tied.begin(), tied.end());
    const auto invalidTie = Domain::validateLegacyContinuityList(
        tied, tied.size());
    REQUIRE(!invalidTie);
    REQUIRE(invalidTie.error().code == Domain::ErrorCodes::IntegrityFailure);
    REQUIRE(fixture.projections.packetCount() == 2U);
    const auto repaired = take(fixture.service.repairProjections(
        fixture.context("repair")));
    REQUIRE(repaired.packetFilesWritten == 2U);
    REQUIRE(repaired.latestWritten);
    REQUIRE(fixture.projections.latestId() == second.record.packet.id);

    const auto denied = fixture.service.reset(
        {"reset_legacy_continuity",
         "legacy-context-continuity",
         "wrong"},
        fixture.context("reset-denied"));
    REQUIRE(!denied);
    REQUIRE(denied.error().code == Domain::ErrorCodes::Unauthorized);
    REQUIRE(fixture.repository.count() == 2U);

    const Domain::DestructiveConfirmation confirmation{
        "reset_legacy_continuity",
        "legacy-context-continuity",
        "RESET LEGACY CONTINUITY"};
    const auto reset = take(fixture.service.reset(
        confirmation, fixture.context("reset")));
    REQUIRE(reset.handoffsRemoved == 2U);
    REQUIRE(reset.authoritativeScopeCommitted);
    REQUIRE(reset.projectionScopeCommitted);
    REQUIRE(reset.verified);
    REQUIRE(fixture.repository.count() == 0U);
    REQUIRE(fixture.projections.packetCount() == 0U);
    REQUIRE(fixture.repository.ordinaryMemoryPreserved);
    REQUIRE(fixture.repository.tagsPreserved);
    REQUIRE(fixture.repository.linksPreserved);
    REQUIRE(fixture.repository.artifactsPreserved);
    REQUIRE(fixture.repository.metadataPreserved);

    const auto repeated = take(fixture.service.reset(
        confirmation, fixture.context("reset-repeat")));
    REQUIRE(repeated.handoffsRemoved == 0U);
    REQUIRE(repeated.verified);

    auto third = take(fixture.service.handoff(
        {std::nullopt,
         Domain::LegacyContinuityPatch{std::string{"Third"}}},
        firstClient,
        Domain::LegacyHandoffSource::Model,
        fixture.context("reset-projection-failure-seed")));
    (void)third;
    fixture.projections.failNextReset();
    const auto partial = take(fixture.service.reset(
        confirmation, fixture.context("reset-projection-failure")));
    REQUIRE(partial.authoritativeScopeCommitted);
    REQUIRE(!partial.projectionScopeCommitted);
    REQUIRE(!partial.verified);
    REQUIRE(partial.projectionWarning);
    REQUIRE(fixture.repository.count() == 0U);
    const auto converged = take(fixture.service.reset(
        confirmation, fixture.context("reset-converge")));
    REQUIRE(converged.authoritativeScopeCommitted);
    REQUIRE(converged.projectionScopeCommitted);
    REQUIRE(converged.verified);
}

void cancellationDeadlineAndShutdownDrainOwnTheBoundary()
{
    Fixture fixture;
    const auto owner = clientId("lifetime-owner");
    const auto callsBefore = fixture.repository.compareCalls();

    std::stop_source cancellation;
    cancellation.request_stop();
    auto cancelledContext = fixture.context("cancelled");
    cancelledContext.cancellation = cancellation.get_token();
    const auto cancelled = fixture.service.checkpoint(
        {std::nullopt, {}},
        owner,
        Domain::LegacyHandoffSource::Model,
        cancelledContext);
    REQUIRE(!cancelled);
    REQUIRE(cancelled.error().code == Domain::ErrorCodes::Cancelled);
    REQUIRE(fixture.repository.compareCalls() == callsBefore);

    auto expiredContext = fixture.context("expired");
    expiredContext.deadline = fixture.clock.monotonicNow();
    const auto expired = fixture.service.checkpoint(
        {std::nullopt, {}},
        owner,
        Domain::LegacyHandoffSource::Model,
        expiredContext);
    REQUIRE(!expired);
    REQUIRE(expired.error().code == Domain::ErrorCodes::DeadlineExceeded);
    REQUIRE(fixture.repository.compareCalls() == callsBefore);

    fixture.repository.blockNextCompare();
    std::optional<Domain::Result<Domain::LegacyContinuityPersistOutcome>> write;
    std::thread writer{[&]() {
        write.emplace(fixture.service.checkpoint(
            {std::nullopt,
             Domain::LegacyContinuityPatch{std::string{"Drain admitted write"}}},
            owner,
            Domain::LegacyHandoffSource::Model,
            fixture.context("admitted-write")));
    }};
    REQUIRE(fixture.repository.waitForCompare(2s));

    std::promise<void> shutdownStarted;
    auto started = shutdownStarted.get_future();
    std::thread shutdown{[&]() {
        shutdownStarted.set_value();
        fixture.service.shutdown();
    }};
    started.wait();

    bool rejectedDuringDrain{};
    const auto deadline = std::chrono::steady_clock::now() + 2s;
    while (std::chrono::steady_clock::now() < deadline) {
        const auto probe = fixture.service.list(
            {1}, fixture.context("shutdown-probe"));
        if (!probe && probe.error().code == Domain::ErrorCodes::Cancelled) {
            rejectedDuringDrain = true;
            break;
        }
        std::this_thread::yield();
    }
    fixture.repository.releaseCompare();
    writer.join();
    shutdown.join();

    REQUIRE(rejectedDuringDrain);
    REQUIRE(write && *write);
    REQUIRE(fixture.repository.closed());
    REQUIRE(fixture.projections.closed());
}

} // namespace

int main()
{
    const std::vector<std::pair<std::string_view, void (*)()>> tests{
        {"checkpoint_handoff_get_list_and_projection_failure",
         checkpointHandoffGetListAndProjectionFailure},
        {"status_summary_matches_legacy_observable_fields",
         statusSummaryMatchesLegacyObservableFields},
        {"explicit_continuation_preserves_only_still_open_agents_and_caps_snapshots",
         explicitContinuationPreservesOnlyStillOpenAgentsAndCapsSnapshots},
        {"compare_exchange_merges_disjoint_fields_and_bounds_conflicts",
         compareExchangeMergesDisjointFieldsAndBoundsConflicts},
        {"resume_seeds_automation_narrative_and_imported_documents_remain_compatible",
         resumeSeedsAutomationNarrativeAndImportedDocumentsRemainCompatible},
        {"repair_and_scoped_convergent_reset_preserve_unrelated_data",
         repairAndScopedConvergentResetPreserveUnrelatedData},
        {"cancellation_deadline_and_shutdown_drain_own_the_boundary",
         cancellationDeadlineAndShutdownDrainOwnTheBoundary}};

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
