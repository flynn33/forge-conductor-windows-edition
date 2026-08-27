#include "ForgeConductor/Application/LegacyContextContinuityService.h"
#include "ForgeConductor/Domain/Utf8.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <limits>
#include <map>
#include <mutex>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace ForgeConductor::Application {
namespace {

constexpr std::string_view ResetAction = "reset_legacy_continuity";
constexpr std::string_view ResetScope = "legacy-context-continuity";
constexpr std::string_view ResetToken = "RESET LEGACY CONTINUITY";

enum class PersistMode {
    Checkpoint,
    Handoff,
    Automatic,
    Budget
};

struct PersistSpec final {
    std::optional<Domain::LegacyHandoffId> explicitId;
    Domain::LegacyContinuityPatch patch;
    Domain::LegacyHandoffSource source{Domain::LegacyHandoffSource::Model};
    PersistMode mode{PersistMode::Checkpoint};
    bool finalize{};
    bool preserveAuthorIdentity{};
    bool fillOnly{};
    std::string reason;
};

template <typename T>
[[nodiscard]] Domain::Result<T> internalFailure(const std::string_view message)
{
    return Domain::Result<T>::failure(Domain::makeError(
        Domain::ErrorCodes::InternalFailure, std::string{message}));
}

template <typename T, typename U>
[[nodiscard]] Domain::Result<T> propagateFailure(Domain::Result<U>&& source)
{
    return Domain::Result<T>::failure(std::move(source).error());
}

template <typename T>
[[nodiscard]] Domain::Result<T> dependencyIntegrityFailure(
    const std::string_view message)
{
    return Domain::Result<T>::failure(Domain::makeError(
        Domain::ErrorCodes::IntegrityFailure, std::string{message}));
}

[[nodiscard]] Domain::Result<void> validateContext(
    const Domain::OperationContext& context,
    const Contracts::IClock& clock) noexcept
{
    try {
        if (context.isCancellationRequested()) {
            return Domain::Result<void>::failure(Domain::makeError(
                Domain::ErrorCodes::Cancelled,
                "The legacy continuity operation was cancelled."));
        }
        if (context.isExpired(clock.monotonicNow())) {
            return Domain::Result<void>::failure(Domain::makeError(
                Domain::ErrorCodes::DeadlineExceeded,
                "The legacy continuity operation deadline expired."));
        }
        return Domain::Result<void>::success();
    } catch (...) {
        return Domain::Result<void>::failure(Domain::makeError(
            Domain::ErrorCodes::InternalFailure,
            "The legacy continuity operation context could not be validated."));
    }
}

[[nodiscard]] bool isConflict(const Domain::Error& error) noexcept
{
    return error.code == Domain::ErrorCodes::Conflict;
}

[[nodiscard]] bool isOpenStatus(const std::string_view status) noexcept
{
    return status == "open" || status == "active" || status == "running" ||
           status == "started";
}

[[nodiscard]] Domain::UtcTimePoint canonicalPersistenceTimestamp(
    const Domain::UtcTimePoint timestamp) noexcept
{
    return Domain::UtcTimePoint{
        std::chrono::duration_cast<std::chrono::milliseconds>(
            timestamp.time_since_epoch())};
}

void canonicalizePersistenceTimestamps(
    Domain::LegacyHandoffPacket& packet) noexcept
{
    packet.createdAt = canonicalPersistenceTimestamp(packet.createdAt);
    packet.updatedAt = canonicalPersistenceTimestamp(packet.updatedAt);
    for (auto& agent : packet.agents) {
        if (agent.updatedAt) {
            agent.updatedAt = canonicalPersistenceTimestamp(*agent.updatedAt);
        }
    }
}

[[nodiscard]] Domain::Result<void> validateSnapshots(
    std::vector<Domain::LegacyAgentContinuitySnapshot>& snapshots) noexcept
{
    try {
        if (snapshots.size() >
            Domain::LegacyContinuityLimits::MaximumAgentSnapshots) {
            return dependencyIntegrityFailure<void>(
                "The agent-session source exceeded the snapshot limit.");
        }
        std::set<std::string> ids;
        for (const auto& snapshot : snapshots) {
            auto valid = Domain::validateLegacyAgentContinuitySnapshot(snapshot);
            if (!valid || !isOpenStatus(snapshot.status) ||
                !ids.insert(snapshot.sessionId.value()).second) {
                return dependencyIntegrityFailure<void>(
                    "The agent-session source returned invalid, closed, or duplicate snapshots.");
            }
        }
        std::stable_sort(
            snapshots.begin(),
            snapshots.end(),
            [](const auto& left, const auto& right) {
                return left.sessionId.value() < right.sessionId.value();
            });
        return Domain::Result<void>::success();
    } catch (...) {
        return internalFailure<void>(
            "Agent continuity snapshots could not be validated.");
    }
}

[[nodiscard]] Domain::Result<void> validateBinding(
    const std::optional<Domain::LegacyActiveBindingSnapshot>& binding) noexcept
{
    if (!binding) {
        return Domain::Result<void>::success();
    }
    auto valid = Domain::validateLegacyActiveBindingSnapshot(*binding);
    if (!valid) {
        return dependencyIntegrityFailure<void>(
            "The agent-session source returned an invalid active binding.");
    }
    return Domain::Result<void>::success();
}

[[nodiscard]] Domain::Result<void> validateReason(
    const std::string_view reason) noexcept
{
    if (reason.empty() || reason.size() >
            Domain::LegacyContinuityLimits::MaximumTextBytes ||
        reason.find('\0') != std::string_view::npos ||
        !Domain::isValidUtf8(reason)) {
        return Domain::Result<void>::failure(Domain::makeError(
            Domain::ErrorCodes::InvalidRequest,
            "Continuity automation reason must be nonempty bounded UTF-8."));
    }
    return Domain::Result<void>::success();
}

[[nodiscard]] Domain::Result<Domain::LegacyHandoffPacket> newPacket(
    Contracts::IUuidGenerator& uuidGenerator,
    const Domain::UtcTimePoint now,
    const Domain::LegacyHandoffSource source,
    const Domain::ClientId& clientId) noexcept
{
    try {
        auto uuid = uuidGenerator.next();
        if (!uuid) {
            return propagateFailure<Domain::LegacyHandoffPacket>(
                std::move(uuid));
        }
        auto handoffId = Domain::LegacyHandoffId::parse(uuid.value().value());
        if (!handoffId) {
            return propagateFailure<Domain::LegacyHandoffPacket>(
                std::move(handoffId));
        }
        return Domain::Result<Domain::LegacyHandoffPacket>::success(
            Domain::LegacyHandoffPacket{
                std::move(handoffId).value(),
                Domain::LegacyContinuityLimits::SchemaVersion,
                now,
                now,
                source,
                false,
                std::nullopt,
                clientId,
                {},
                "in_progress",
                std::nullopt,
                std::nullopt,
                {},
                {},
                {},
                {},
                {},
                {},
                {},
                false});
    } catch (...) {
        return internalFailure<Domain::LegacyHandoffPacket>(
            "A legacy continuity packet id could not be allocated.");
    }
}

[[nodiscard]] Domain::Result<void> validateLoadedRecord(
    const Domain::LegacyContinuityRecord& record) noexcept
{
    auto valid = Domain::validateLegacyContinuityRecord(record);
    if (!valid) {
        return dependencyIntegrityFailure<void>(
            "The legacy continuity repository returned an invalid record.");
    }
    return Domain::Result<void>::success();
}

[[nodiscard]] Domain::Result<void> validateLatestSelection(
    const std::optional<Domain::LegacyContinuityRecord>& record,
    const std::optional<Domain::ClientId>& clientId,
    const bool resumeReadyOnly) noexcept
{
    if (!record) {
        return Domain::Result<void>::success();
    }
    auto valid = validateLoadedRecord(*record);
    if (!valid) {
        return valid;
    }
    if (resumeReadyOnly && !record->packet.resumeReady) {
        return dependencyIntegrityFailure<void>(
            "The repository returned a non-resume packet for a resume-only query.");
    }
    if (clientId &&
        (!record->packet.clientId || *record->packet.clientId != *clientId)) {
        return dependencyIntegrityFailure<void>(
            "The repository returned a packet owned by another client.");
    }
    return Domain::Result<void>::success();
}

[[nodiscard]] Domain::Result<void> validateStoredMutation(
    const Domain::LegacyContinuityRecord& record,
    const Domain::LegacyContinuityCompareExchange& mutation) noexcept
{
    auto valid = validateLoadedRecord(record);
    if (!valid) {
        return valid;
    }
    if (record.packet != mutation.packet ||
        (mutation.expectedWriteSequence &&
         record.writeSequence <= *mutation.expectedWriteSequence) ||
        !record.documents.packetJson || !record.documents.payloadJson ||
        !record.documents.contentSha256) {
        return dependencyIntegrityFailure<void>(
            "The continuity compare-exchange result is inconsistent or lacks canonical documents.");
    }
    return Domain::Result<void>::success();
}

[[nodiscard]] Domain::Result<std::string> appendNarrativeNote(
    const std::string_view existing,
    const std::string_view note) noexcept
{
    try {
        if (existing.find(note) != std::string_view::npos) {
            return Domain::Result<std::string>::success(std::string{existing});
        }
        std::string combined;
        if (existing.empty()) {
            combined.assign(note);
        } else {
            combined.reserve(existing.size() + note.size() + 2U);
            combined.assign(existing);
            combined.append("\n\n");
            combined.append(note);
        }
        return Domain::truncateLegacyNarrative(combined);
    } catch (...) {
        return internalFailure<std::string>(
            "A continuity narrative note could not be appended.");
    }
}

} // namespace

class LegacyContextContinuityService::Impl final {
public:
    Impl(
        Contracts::ILegacyContinuityRepository& repository,
        Contracts::IContinuityProjectionStore& projections,
        Contracts::ILegacyContinuitySessionSource& sessions,
        Contracts::IClock& clock,
        Contracts::IUuidGenerator& uuidGenerator) noexcept
        : repository_{repository},
          projections_{projections},
          sessions_{sessions},
          clock_{clock},
          uuidGenerator_{uuidGenerator}
    {
    }

    [[nodiscard]] Domain::Result<Domain::LegacyContinuityPersistOutcome>
    checkpoint(
        const Domain::LegacyContinuityWriteRequest& request,
        const Domain::ClientId& clientId,
        const Domain::LegacyHandoffSource source,
        const Domain::OperationContext& context) noexcept
    {
        return execute<Domain::LegacyContinuityPersistOutcome>(context, [&]() {
            return persist(
                PersistSpec{
                    request.handoffId,
                    request.patch,
                    source,
                    PersistMode::Checkpoint,
                    false,
                    false,
                    false,
                    {}},
                clientId,
                context);
        });
    }

    [[nodiscard]] Domain::Result<Domain::LegacyContinuityPersistOutcome>
    handoff(
        const Domain::LegacyContinuityWriteRequest& request,
        const Domain::ClientId& clientId,
        const Domain::LegacyHandoffSource source,
        const Domain::OperationContext& context) noexcept
    {
        return execute<Domain::LegacyContinuityPersistOutcome>(context, [&]() {
            return persist(
                PersistSpec{
                    request.handoffId,
                    request.patch,
                    source,
                    PersistMode::Handoff,
                    true,
                    false,
                    false,
                    {}},
                clientId,
                context);
        });
    }

    [[nodiscard]] Domain::Result<Domain::LegacyContinuityPersistOutcome>
    automaticPersist(
        const Domain::LegacyContinuityAutomaticRequest& request,
        const Domain::ClientId& clientId,
        const Domain::OperationContext& context) noexcept
    {
        return execute<Domain::LegacyContinuityPersistOutcome>(context, [&]() {
            auto reason = validateReason(request.reason);
            if (!reason) {
                return propagateFailure<Domain::LegacyContinuityPersistOutcome>(
                    std::move(reason));
            }
            return persist(
                PersistSpec{
                    std::nullopt,
                    request.inferred,
                    Domain::LegacyHandoffSource::Automatic,
                    PersistMode::Automatic,
                    request.finalize,
                    true,
                    true,
                    request.reason},
                clientId,
                context);
        });
    }

    [[nodiscard]] Domain::Result<Domain::LegacyContinuityPersistOutcome>
    budgetHandoff(
        const Domain::ClientId& clientId,
        const std::string_view reason,
        const Domain::OperationContext& context) noexcept
    {
        return execute<Domain::LegacyContinuityPersistOutcome>(context, [&]() {
            auto valid = validateReason(reason);
            if (!valid) {
                return propagateFailure<Domain::LegacyContinuityPersistOutcome>(
                    std::move(valid));
            }
            Domain::LegacyContinuityPatch patch;
            patch.status = "budget_pressure";
            return persist(
                PersistSpec{
                    std::nullopt,
                    std::move(patch),
                    Domain::LegacyHandoffSource::Budget,
                    PersistMode::Budget,
                    true,
                    false,
                    false,
                    std::string{reason}},
                clientId,
                context);
        });
    }

    [[nodiscard]] Domain::Result<Domain::LegacyContinuityGetOutcome> get(
        const Domain::LegacyContinuityGetRequest& request,
        const Domain::OperationContext& context) noexcept
    {
        return execute<Domain::LegacyContinuityGetOutcome>(context, [&]() {
            Domain::Result<std::optional<Domain::LegacyContinuityRecord>> loaded =
                request.handoffId
                    ? repository_.get(*request.handoffId, context)
                    : repository_.latest(
                          std::nullopt, request.preferResumeReady, context);
            if (!loaded) {
                return propagateFailure<Domain::LegacyContinuityGetOutcome>(
                    std::move(loaded));
            }
            auto record = std::move(loaded).value();
            if (!request.handoffId) {
                auto valid = validateLatestSelection(
                    record, std::nullopt, request.preferResumeReady);
                if (!valid) {
                    return propagateFailure<
                        Domain::LegacyContinuityGetOutcome>(std::move(valid));
                }
            }
            if (!request.handoffId && request.preferResumeReady && !record) {
                loaded = repository_.latest(std::nullopt, false, context);
                if (!loaded) {
                    return propagateFailure<Domain::LegacyContinuityGetOutcome>(
                        std::move(loaded));
                }
                record = std::move(loaded).value();
                auto valid = validateLatestSelection(
                    record, std::nullopt, false);
                if (!valid) {
                    return propagateFailure<
                        Domain::LegacyContinuityGetOutcome>(std::move(valid));
                }
            }
            if (record) {
                auto valid = validateLoadedRecord(*record);
                if (!valid) {
                    return propagateFailure<Domain::LegacyContinuityGetOutcome>(
                        std::move(valid));
                }
                if (request.handoffId &&
                    record->packet.id != *request.handoffId) {
                    return dependencyIntegrityFailure<
                        Domain::LegacyContinuityGetOutcome>(
                        "The continuity repository returned a mismatched handoff id.");
                }
            }
            return Domain::Result<Domain::LegacyContinuityGetOutcome>::success(
                Domain::LegacyContinuityGetOutcome{
                    std::move(record), request.handoffId.has_value()});
        });
    }

    [[nodiscard]] Domain::Result<Domain::LegacyContinuityListOutcome> list(
        const Domain::LegacyContinuityListRequest& request,
        const Domain::OperationContext& context) noexcept
    {
        return execute<Domain::LegacyContinuityListOutcome>(context, [&]() {
            const auto limit = Domain::normalizeLegacyContinuityListLimit(
                request.requestedLimit);
            auto loaded = repository_.list(limit, context);
            if (!loaded) {
                return propagateFailure<Domain::LegacyContinuityListOutcome>(
                    std::move(loaded));
            }
            auto records = std::move(loaded).value();
            auto valid = Domain::validateLegacyContinuityList(records, limit);
            if (!valid) {
                return dependencyIntegrityFailure<
                    Domain::LegacyContinuityListOutcome>(
                    "The legacy continuity list dependency returned invalid ordering or rows.");
            }
            std::vector<Domain::LegacyContinuityListItem> items;
            items.reserve(records.size());
            for (auto& record : records) {
                const auto& packet = record.packet;
                items.push_back(Domain::LegacyContinuityListItem{
                    packet.id,
                    packet.updatedAt,
                    packet.source,
                    packet.resumeReady,
                    packet.goal,
                    packet.status,
                    packet.agents.size(),
                    record.writeSequence});
            }
            return Domain::Result<Domain::LegacyContinuityListOutcome>::success(
                Domain::LegacyContinuityListOutcome{std::move(items)});
        });
    }

    [[nodiscard]] Domain::Result<Domain::LegacyContinuityStatusSummary>
    statusSummary(const Domain::OperationContext& context) noexcept
    {
        return execute<Domain::LegacyContinuityStatusSummary>(context, [&]() {
            constexpr std::size_t MaximumOpenAgentSessions = 10'000U;
            auto latest = repository_.latest(std::nullopt, false, context);
            if (!latest) {
                return propagateFailure<Domain::LegacyContinuityStatusSummary>(
                    std::move(latest));
            }
            auto latestRecord = std::move(latest).value();
            auto valid = validateLatestSelection(
                latestRecord, std::nullopt, false);
            if (!valid) {
                return dependencyIntegrityFailure<
                    Domain::LegacyContinuityStatusSummary>(
                        "The continuity repository returned an invalid latest status row.");
            }
            if (latestRecord) {
                valid = validateLoadedRecord(*latestRecord);
                if (!valid) {
                    return dependencyIntegrityFailure<
                        Domain::LegacyContinuityStatusSummary>(
                            "The continuity repository returned an invalid latest status record.");
                }
            }

            valid = validateContext(context, clock_);
            if (!valid) {
                return propagateFailure<Domain::LegacyContinuityStatusSummary>(
                    std::move(valid));
            }
            auto resume = repository_.latest(std::nullopt, true, context);
            if (!resume) {
                return propagateFailure<Domain::LegacyContinuityStatusSummary>(
                    std::move(resume));
            }
            auto resumeRecord = std::move(resume).value();
            valid = validateLatestSelection(
                resumeRecord, std::nullopt, true);
            if (!valid) {
                return dependencyIntegrityFailure<
                    Domain::LegacyContinuityStatusSummary>(
                        "The continuity repository returned an invalid resume-ready status row.");
            }
            if (resumeRecord) {
                valid = validateLoadedRecord(*resumeRecord);
                if (!valid) {
                    return dependencyIntegrityFailure<
                        Domain::LegacyContinuityStatusSummary>(
                            "The continuity repository returned an invalid resume-ready status record.");
                }
            }

            valid = validateContext(context, clock_);
            if (!valid) {
                return propagateFailure<Domain::LegacyContinuityStatusSummary>(
                    std::move(valid));
            }
            auto openAgentSessions = sessions_.countOpen(
                MaximumOpenAgentSessions, context);
            if (!openAgentSessions) {
                return propagateFailure<Domain::LegacyContinuityStatusSummary>(
                    std::move(openAgentSessions));
            }

            Domain::LegacyContinuityStatusSummary summary;
            summary.openAgentSessions = openAgentSessions.value();
            if (latestRecord) {
                summary.latestId = latestRecord->packet.id;
                summary.latestUpdatedAt = latestRecord->packet.updatedAt;
            }
            summary.resumeReady = resumeRecord.has_value();
            if (resumeRecord) {
                summary.resumeId = resumeRecord->packet.id;
            }
            return Domain::Result<
                Domain::LegacyContinuityStatusSummary>::success(
                    std::move(summary));
        });
    }

    [[nodiscard]] Domain::Result<Domain::LegacyContinuityProjectionRepairOutcome>
    repairProjections(const Domain::OperationContext& context) noexcept
    {
        return execute<Domain::LegacyContinuityProjectionRepairOutcome>(
            context,
            [&]() {
                auto pointers = repository_.repairPointers(context);
                if (!pointers) {
                    return propagateFailure<
                        Domain::LegacyContinuityProjectionRepairOutcome>(
                        std::move(pointers));
                }
                auto records = repository_.listAll(
                    Domain::LegacyContinuityLimits::MaximumRepairRows, context);
                if (!records) {
                    return propagateFailure<
                        Domain::LegacyContinuityProjectionRepairOutcome>(
                        std::move(records));
                }
                auto values = std::move(records).value();
                auto valid = Domain::validateLegacyContinuityList(
                    values, Domain::LegacyContinuityLimits::MaximumRepairRows);
                if (!valid || !pointersMatch(values, pointers.value())) {
                    return dependencyIntegrityFailure<
                        Domain::LegacyContinuityProjectionRepairOutcome>(
                        "The authoritative continuity pointers do not match ordered handoff rows.");
                }
                auto repaired = projections_.repair(
                    values, pointers.value(), context);
                if (!repaired) {
                    return repaired;
                }
                if (repaired.value().packetFilesWritten > values.size() ||
                    (values.empty() &&
                     (repaired.value().latestWritten ||
                      repaired.value().currentTaskWritten))) {
                    return dependencyIntegrityFailure<
                        Domain::LegacyContinuityProjectionRepairOutcome>(
                        "The projection repair dependency returned an inconsistent receipt.");
                }
                return repaired;
            });
    }

    [[nodiscard]] Domain::Result<Domain::LegacyContinuityResetOutcome> reset(
        const Domain::DestructiveConfirmation& confirmation,
        const Domain::OperationContext& context) noexcept
    {
        return execute<Domain::LegacyContinuityResetOutcome>(context, [&]() {
            auto valid = Domain::validateDestructiveConfirmation(
                confirmation, ResetAction, ResetScope, ResetToken);
            if (!valid) {
                return propagateFailure<Domain::LegacyContinuityResetOutcome>(
                    std::move(valid));
            }
            auto authoritative = repository_.reset(confirmation, context);
            if (!authoritative) {
                return authoritative;
            }
            auto outcome = std::move(authoritative).value();
            if (!outcome.authoritativeScopeCommitted || !outcome.verified ||
                outcome.projectionScopeCommitted ||
                outcome.projectionFilesRemoved != 0U) {
                return dependencyIntegrityFailure<
                    Domain::LegacyContinuityResetOutcome>(
                    "The central continuity reset dependency returned an inconsistent report.");
            }

            auto stillValid = validateContext(context, clock_);
            if (!stillValid) {
                outcome.verified = false;
                outcome.projectionWarning = std::move(stillValid).error();
                return Domain::Result<Domain::LegacyContinuityResetOutcome>::success(
                    std::move(outcome));
            }
            auto projected = projections_.reset(confirmation, context);
            if (!projected) {
                outcome.verified = false;
                outcome.projectionWarning = std::move(projected).error();
                return Domain::Result<Domain::LegacyContinuityResetOutcome>::success(
                    std::move(outcome));
            }
            outcome.projectionFilesRemoved = projected.value();
            outcome.projectionScopeCommitted = true;
            outcome.verified = true;
            outcome.projectionWarning.reset();
            return Domain::Result<Domain::LegacyContinuityResetOutcome>::success(
                std::move(outcome));
        });
    }

    void shutdown() noexcept
    {
        bool leader{};
        try {
            std::unique_lock lock{lifecycleMutex_};
            if (shutdownComplete_.load(std::memory_order_acquire)) {
                return;
            }
            if (!accepting_) {
                lifecycleChanged_.wait(lock, [&]() {
                    return shutdownComplete_.load(std::memory_order_acquire);
                });
                return;
            }
            accepting_ = false;
            leader = true;
            lifecycleChanged_.wait(lock, [&]() { return activeOperations_ == 0U; });
            lock.unlock();
            projections_.close();
            repository_.close();
            shutdownComplete_.store(true, std::memory_order_release);
            lifecycleChanged_.notify_all();
        } catch (...) {
            if (leader) {
                projections_.close();
                repository_.close();
                shutdownComplete_.store(true, std::memory_order_release);
                lifecycleChanged_.notify_all();
            }
        }
    }

private:
    class Admission final {
    public:
        explicit Admission(Impl& owner) noexcept : owner_{&owner} {}
        Admission(const Admission&) = delete;
        Admission& operator=(const Admission&) = delete;
        Admission(Admission&& other) noexcept
            : owner_{std::exchange(other.owner_, nullptr)}
        {
        }
        Admission& operator=(Admission&& other) noexcept
        {
            if (this != &other) {
                release();
                owner_ = std::exchange(other.owner_, nullptr);
            }
            return *this;
        }
        ~Admission() noexcept { release(); }

    private:
        void release() noexcept
        {
            if (owner_ != nullptr) {
                owner_->releaseOperation();
                owner_ = nullptr;
            }
        }
        Impl* owner_{};
    };

    [[nodiscard]] Domain::Result<Admission> admit(
        const Domain::OperationContext& context) noexcept
    {
        try {
            auto valid = validateContext(context, clock_);
            if (!valid) return propagateFailure<Admission>(std::move(valid));
            std::lock_guard lock{lifecycleMutex_};
            valid = validateContext(context, clock_);
            if (!valid) return propagateFailure<Admission>(std::move(valid));
            if (!accepting_) {
                return Domain::Result<Admission>::failure(Domain::makeError(
                    Domain::ErrorCodes::Cancelled,
                    "The legacy continuity service is shutting down."));
            }
            if (activeOperations_ == std::numeric_limits<std::size_t>::max()) {
                return Domain::Result<Admission>::failure(Domain::makeError(
                    Domain::ErrorCodes::LimitExceeded,
                    "The legacy continuity admission counter overflowed."));
            }
            ++activeOperations_;
            return Domain::Result<Admission>::success(Admission{*this});
        } catch (...) {
            return internalFailure<Admission>(
                "The legacy continuity operation could not be admitted.");
        }
    }

    void releaseOperation() noexcept
    {
        std::lock_guard lock{lifecycleMutex_};
        if (activeOperations_ != 0U) --activeOperations_;
        if (!accepting_ && activeOperations_ == 0U) {
            lifecycleChanged_.notify_all();
        }
    }

    template <typename T, typename Function>
    [[nodiscard]] Domain::Result<T> execute(
        const Domain::OperationContext& context,
        Function&& operation) noexcept
    {
        try {
            auto admitted = admit(context);
            if (!admitted) return propagateFailure<T>(std::move(admitted));
            [[maybe_unused]] auto admission = std::move(admitted).value();
            return std::forward<Function>(operation)();
        } catch (...) {
            return internalFailure<T>(
                "The legacy continuity application boundary failed internally.");
        }
    }

    [[nodiscard]] Domain::Result<std::optional<Domain::LegacyContinuityRecord>>
    selectInitial(
        const PersistSpec& spec,
        const Domain::ClientId& clientId,
        const Domain::OperationContext& context) noexcept
    {
        if (spec.explicitId) {
            auto loaded = repository_.get(*spec.explicitId, context);
            if (!loaded) return loaded;
            if (!loaded.value()) {
                return Domain::Result<
                    std::optional<Domain::LegacyContinuityRecord>>::failure(
                    Domain::makeError(
                        Domain::ErrorCodes::RecordNotFound,
                        "Unknown legacy handoff packet: " +
                            spec.explicitId->value()));
            }
            auto valid = validateLoadedRecord(*loaded.value());
            if (!valid) {
                return propagateFailure<
                    std::optional<Domain::LegacyContinuityRecord>>(
                    std::move(valid));
            }
            return loaded;
        }

        auto loaded = repository_.latest(clientId, false, context);
        if (!loaded) return loaded;
        auto valid = validateLatestSelection(loaded.value(), clientId, false);
        if (!valid) {
            return propagateFailure<
                std::optional<Domain::LegacyContinuityRecord>>(
                std::move(valid));
        }

        if (spec.mode == PersistMode::Automatic && !loaded.value()) {
            loaded = repository_.latest(std::nullopt, false, context);
            if (!loaded) return loaded;
            valid = validateLatestSelection(loaded.value(), std::nullopt, false);
            if (!valid) {
                return propagateFailure<
                    std::optional<Domain::LegacyContinuityRecord>>(
                    std::move(valid));
            }
        }
        if ((spec.mode == PersistMode::Checkpoint ||
             spec.mode == PersistMode::Handoff) &&
            loaded.value() && loaded.value()->packet.resumeReady) {
            return Domain::Result<
                std::optional<Domain::LegacyContinuityRecord>>::success(
                std::nullopt);
        }
        return loaded;
    }

    [[nodiscard]] Domain::Result<std::vector<Domain::LegacyAgentContinuitySnapshot>>
    currentSnapshots(
        const Domain::ClientId& clientId,
        const Domain::OperationContext& context) noexcept
    {
        auto snapshots = sessions_.listOpenForClient(
            clientId,
            Domain::LegacyContinuityLimits::MaximumAgentSnapshots,
            context);
        if (!snapshots) return snapshots;
        auto values = std::move(snapshots).value();
        auto valid = validateSnapshots(values);
        if (!valid) {
            return propagateFailure<
                std::vector<Domain::LegacyAgentContinuitySnapshot>>(
                std::move(valid));
        }
        return Domain::Result<
            std::vector<Domain::LegacyAgentContinuitySnapshot>>::success(
            std::move(values));
    }

    [[nodiscard]] Domain::Result<std::vector<Domain::LegacyAgentContinuitySnapshot>>
    mergeSnapshots(
        const std::vector<Domain::LegacyAgentContinuitySnapshot>& prior,
        const std::vector<Domain::LegacyAgentContinuitySnapshot>& current,
        const Domain::OperationContext& context) noexcept
    {
        try {
            std::map<std::string, Domain::LegacyAgentContinuitySnapshot>
                currentBySession;
            for (const auto& snapshot : current) {
                currentBySession.emplace(snapshot.sessionId.value(), snapshot);
            }
            std::vector<Domain::LegacyAgentContinuitySnapshot> merged;
            merged.reserve(Domain::LegacyContinuityLimits::MaximumAgentSnapshots);
            std::set<std::string> seen;
            for (const auto& snapshot : prior) {
                if (merged.size() >=
                    Domain::LegacyContinuityLimits::MaximumAgentSnapshots) {
                    break;
                }
                if (!seen.insert(snapshot.sessionId.value()).second) continue;
                auto open = sessions_.isOpen(snapshot.sessionId, context);
                if (!open) {
                    return propagateFailure<
                        std::vector<Domain::LegacyAgentContinuitySnapshot>>(
                        std::move(open));
                }
                if (!open.value()) continue;
                const auto replacement = currentBySession.find(
                    snapshot.sessionId.value());
                if (replacement == currentBySession.end()) {
                    merged.push_back(snapshot);
                } else {
                    merged.push_back(std::move(replacement->second));
                    currentBySession.erase(replacement);
                }
            }
            for (const auto& snapshot : current) {
                if (merged.size() >=
                    Domain::LegacyContinuityLimits::MaximumAgentSnapshots) {
                    break;
                }
                if (seen.insert(snapshot.sessionId.value()).second) {
                    merged.push_back(snapshot);
                }
            }
            return Domain::Result<
                std::vector<Domain::LegacyAgentContinuitySnapshot>>::success(
                std::move(merged));
        } catch (...) {
            return internalFailure<
                std::vector<Domain::LegacyAgentContinuitySnapshot>>(
                "Open agent snapshots could not be merged.");
        }
    }

    [[nodiscard]] Domain::Result<Domain::LegacyHandoffPacket> buildPacket(
        const PersistSpec& spec,
        const Domain::ClientId& clientId,
        const std::optional<Domain::LegacyContinuityRecord>& current,
        const Domain::LegacyHandoffPacket& fresh,
        const std::vector<Domain::LegacyAgentContinuitySnapshot>& snapshots,
        const std::optional<Domain::LegacyActiveBindingSnapshot>& binding,
        const bool preservePriorAgents,
        const Domain::UtcTimePoint now,
        const Domain::OperationContext& context) noexcept
    {
        try {
            auto packet = current ? current->packet : fresh;
            packet.updatedAt = now;
            if (!current ||
                !(spec.preserveAuthorIdentity && !spec.finalize)) {
                packet.source = spec.source;
            }
            if (!current || !spec.preserveAuthorIdentity || !packet.clientId) {
                packet.clientId = clientId;
            }

            const auto& patch = spec.patch;
            if (patch.goal && !patch.goal->empty() &&
                (!spec.fillOnly || packet.goal.empty())) {
                packet.goal = *patch.goal;
            }
            if (patch.status && !patch.status->empty() &&
                (!spec.fillOnly || packet.status.empty())) {
                packet.status = *patch.status;
            }
            if (patch.projectSlug &&
                (!spec.fillOnly || !packet.projectSlug ||
                 packet.projectSlug->empty())) {
                packet.projectSlug = *patch.projectSlug;
            }
            if (patch.workingDirectory &&
                (!spec.fillOnly || !packet.workingDirectory ||
                 packet.workingDirectory->empty())) {
                packet.workingDirectory = *patch.workingDirectory;
            }
            if (patch.chatLabel &&
                (!spec.fillOnly || !packet.chatLabel || packet.chatLabel->empty())) {
                packet.chatLabel = *patch.chatLabel;
            }
            if (patch.narrative &&
                (!spec.fillOnly || packet.narrative.empty())) {
                auto narrative = Domain::truncateLegacyNarrative(*patch.narrative);
                if (!narrative) {
                    return propagateFailure<Domain::LegacyHandoffPacket>(
                        std::move(narrative));
                }
                packet.narrative = std::move(narrative).value();
            }
            if (patch.resumeSeed &&
                (!spec.fillOnly || packet.resumeSeed.empty())) {
                packet.resumeSeed = *patch.resumeSeed;
                packet.resumeSeedIsCustom = !patch.resumeSeed->empty();
            }
            if (patch.blockers &&
                (!spec.fillOnly || packet.blockers.empty())) {
                packet.blockers = *patch.blockers;
            }
            if (patch.nextActions &&
                (!spec.fillOnly || packet.nextActions.empty())) {
                packet.nextActions = *patch.nextActions;
            }
            if (patch.keyFiles &&
                (!spec.fillOnly || packet.keyFiles.empty())) {
                packet.keyFiles = *patch.keyFiles;
            }
            if (patch.decisions &&
                (!spec.fillOnly || packet.decisions.empty())) {
                packet.decisions = *patch.decisions;
            }

            if (preservePriorAgents && current) {
                auto merged = mergeSnapshots(
                    current->packet.agents, snapshots, context);
                if (!merged) {
                    return propagateFailure<Domain::LegacyHandoffPacket>(
                        std::move(merged));
                }
                packet.agents = std::move(merged).value();
            } else {
                packet.agents = snapshots;
            }

            if (binding) {
                if (packet.goal.empty()) packet.goal = binding->goal;
                if ((!packet.workingDirectory ||
                     packet.workingDirectory->empty()) &&
                    binding->workingDirectory) {
                    packet.workingDirectory = binding->workingDirectory;
                }
            }

            if (spec.mode == PersistMode::Automatic) {
                if (spec.finalize && !spec.patch.status) {
                    packet.status = "handoff_ready";
                }
                if (packet.goal.empty()) {
                    packet.goal = spec.finalize
                        ? "Auto-handoff: " + spec.reason
                        : "Auto-checkpoint: " + spec.reason;
                }
                if (spec.finalize) {
                    auto narrative = appendNarrativeNote(
                        packet.narrative,
                        "Runtime continuity: " + spec.reason);
                    if (!narrative) {
                        return propagateFailure<Domain::LegacyHandoffPacket>(
                            std::move(narrative));
                    }
                    packet.narrative = std::move(narrative).value();
                }
            } else if (spec.mode == PersistMode::Budget) {
                if (packet.goal.empty()) {
                    packet.goal = "Auto-checkpoint: " + spec.reason;
                }
                if (packet.nextActions.empty()) {
                    packet.nextActions = {
                        "Start a new chat if context is full",
                        "Call context_get",
                        "Continue from open agents"};
                }
                auto narrative = appendNarrativeNote(
                    packet.narrative, "Budget trigger: " + spec.reason);
                if (!narrative) {
                    return propagateFailure<Domain::LegacyHandoffPacket>(
                        std::move(narrative));
                }
                packet.narrative = std::move(narrative).value();
            }

            canonicalizePersistenceTimestamps(packet);
            if (spec.finalize) packet.resumeReady = true;
            if ((!patch.resumeSeed && !packet.resumeSeedIsCustom) ||
                (spec.finalize && packet.resumeSeed.empty())) {
                auto seed = Domain::makeLegacyDefaultResumeSeed(packet);
                if (!seed) {
                    return propagateFailure<Domain::LegacyHandoffPacket>(
                        std::move(seed));
                }
                packet.resumeSeed = std::move(seed).value();
                packet.resumeSeedIsCustom = false;
            }

            auto valid = Domain::validateLegacyHandoffPacket(packet);
            if (!valid) {
                return propagateFailure<Domain::LegacyHandoffPacket>(
                    std::move(valid));
            }
            return Domain::Result<Domain::LegacyHandoffPacket>::success(
                std::move(packet));
        } catch (...) {
            return internalFailure<Domain::LegacyHandoffPacket>(
                "A legacy continuity packet could not be built.");
        }
    }

    [[nodiscard]] Domain::Result<Domain::LegacyContinuityPersistOutcome> persist(
        PersistSpec spec,
        const Domain::ClientId& clientId,
        const Domain::OperationContext& context) noexcept
    {
        auto valid = Domain::validateLegacyContinuityPatch(spec.patch);
        if (!valid) {
            return propagateFailure<Domain::LegacyContinuityPersistOutcome>(
                std::move(valid));
        }
        const auto now = clock_.utcNow();
        auto selected = selectInitial(spec, clientId, context);
        if (!selected) {
            return propagateFailure<Domain::LegacyContinuityPersistOutcome>(
                std::move(selected));
        }
        auto current = std::move(selected).value();
        const bool preservePriorAgents = spec.explicitId.has_value() ||
            ((spec.mode == PersistMode::Automatic ||
              spec.mode == PersistMode::Budget) && current.has_value());

        auto fresh = newPacket(uuidGenerator_, now, spec.source, clientId);
        if (!fresh) {
            return propagateFailure<Domain::LegacyContinuityPersistOutcome>(
                std::move(fresh));
        }
        auto freshPacket = std::move(fresh).value();

        auto snapshots = currentSnapshots(clientId, context);
        if (!snapshots) {
            return propagateFailure<Domain::LegacyContinuityPersistOutcome>(
                std::move(snapshots));
        }
        auto binding = sessions_.binding(clientId, context);
        if (!binding) {
            return propagateFailure<Domain::LegacyContinuityPersistOutcome>(
                std::move(binding));
        }
        valid = validateBinding(binding.value());
        if (!valid) {
            return propagateFailure<Domain::LegacyContinuityPersistOutcome>(
                std::move(valid));
        }

        std::size_t conflictRetries{};
        for (;;) {
            valid = validateContext(context, clock_);
            if (!valid) {
                return propagateFailure<Domain::LegacyContinuityPersistOutcome>(
                    std::move(valid));
            }
            auto packet = buildPacket(
                spec,
                clientId,
                current,
                freshPacket,
                snapshots.value(),
                binding.value(),
                preservePriorAgents,
                now,
                context);
            if (!packet) {
                return propagateFailure<Domain::LegacyContinuityPersistOutcome>(
                    std::move(packet));
            }
            Domain::LegacyContinuityCompareExchange mutation{
                std::move(packet).value(),
                current
                    ? std::optional<std::uint64_t>{current->writeSequence}
                    : std::nullopt};
            auto stored = repository_.compareExchange(mutation, context);
            if (stored) {
                valid = validateStoredMutation(stored.value(), mutation);
                if (!valid) {
                    return propagateFailure<
                        Domain::LegacyContinuityPersistOutcome>(
                        std::move(valid));
                }
                return projectCommitted(
                    std::move(stored).value(), spec.finalize, context);
            }
            if (!isConflict(stored.error())) {
                return propagateFailure<Domain::LegacyContinuityPersistOutcome>(
                    std::move(stored));
            }
            if (conflictRetries >=
                Domain::LegacyContinuityLimits::MaximumConflictRetries) {
                return Domain::Result<
                    Domain::LegacyContinuityPersistOutcome>::failure(
                    Domain::makeError(
                        Domain::ErrorCodes::Conflict,
                        "Legacy continuity compare-exchange exhausted eight conflict retries.",
                        true));
            }
            ++conflictRetries;

            auto reloaded = repository_.get(mutation.packet.id, context);
            if (!reloaded) {
                return propagateFailure<Domain::LegacyContinuityPersistOutcome>(
                    std::move(reloaded));
            }
            if (!reloaded.value()) {
                if (spec.explicitId) {
                    return Domain::Result<
                        Domain::LegacyContinuityPersistOutcome>::failure(
                        Domain::makeError(
                            Domain::ErrorCodes::RecordNotFound,
                            "The explicitly continued handoff was removed during conflict recovery."));
                }
                auto replacement = newPacket(
                    uuidGenerator_, now, spec.source, clientId);
                if (!replacement) {
                    return propagateFailure<
                        Domain::LegacyContinuityPersistOutcome>(
                        std::move(replacement));
                }
                freshPacket = std::move(replacement).value();
                current.reset();
                continue;
            }
            valid = validateLoadedRecord(*reloaded.value());
            if (!valid) {
                return propagateFailure<Domain::LegacyContinuityPersistOutcome>(
                    std::move(valid));
            }
            current = std::move(reloaded).value();
        }
    }

    [[nodiscard]] Domain::Result<Domain::LegacyContinuityPersistOutcome>
    projectCommitted(
        Domain::LegacyContinuityRecord record,
        const bool handoffRequired,
        const Domain::OperationContext& context) noexcept
    {
        Domain::LegacyContinuityPersistOutcome outcome{
            std::move(record),
            handoffRequired,
            true,
            false,
            std::nullopt,
            {}};
        auto valid = validateContext(context, clock_);
        if (!valid) {
            outcome.projectionOk = false;
            outcome.projectionRepairPending = true;
            outcome.projectionWarning = std::move(valid).error();
            return Domain::Result<
                Domain::LegacyContinuityPersistOutcome>::success(
                std::move(outcome));
        }
        auto projected = projections_.write(outcome.record, context);
        if (!projected) {
            outcome.projectionOk = false;
            outcome.projectionRepairPending = true;
            outcome.projectionWarning = std::move(projected).error();
            return Domain::Result<
                Domain::LegacyContinuityPersistOutcome>::success(
                std::move(outcome));
        }
        outcome.projection = std::move(projected).value();
        return Domain::Result<
            Domain::LegacyContinuityPersistOutcome>::success(
            std::move(outcome));
    }

    [[nodiscard]] bool pointersMatch(
        const std::vector<Domain::LegacyContinuityRecord>& records,
        const Domain::LegacyContinuityPointerRepairOutcome& pointers) const
        noexcept
    {
        std::optional<Domain::LegacyHandoffId> latest;
        std::optional<Domain::LegacyHandoffId> resume;
        if (!records.empty()) latest = records.front().packet.id;
        for (const auto& record : records) {
            if (record.packet.resumeReady) {
                resume = record.packet.id;
                break;
            }
        }
        return latest == pointers.latestId && resume == pointers.resumeReadyId;
    }

    Contracts::ILegacyContinuityRepository& repository_;
    Contracts::IContinuityProjectionStore& projections_;
    Contracts::ILegacyContinuitySessionSource& sessions_;
    Contracts::IClock& clock_;
    Contracts::IUuidGenerator& uuidGenerator_;
    std::mutex lifecycleMutex_;
    std::condition_variable lifecycleChanged_;
    std::size_t activeOperations_{};
    bool accepting_{true};
    std::atomic<bool> shutdownComplete_{};
};

LegacyContextContinuityService::LegacyContextContinuityService(
    Contracts::ILegacyContinuityRepository& repository,
    Contracts::IContinuityProjectionStore& projections,
    Contracts::ILegacyContinuitySessionSource& sessions,
    Contracts::IClock& clock,
    Contracts::IUuidGenerator& uuidGenerator)
    : implementation_{std::make_unique<Impl>(
          repository, projections, sessions, clock, uuidGenerator)}
{
}

LegacyContextContinuityService::~LegacyContextContinuityService() noexcept
{
    implementation_->shutdown();
}

Domain::Result<Domain::LegacyContinuityPersistOutcome>
LegacyContextContinuityService::checkpoint(
    const Domain::LegacyContinuityWriteRequest& request,
    const Domain::ClientId& clientId,
    const Domain::LegacyHandoffSource source,
    const Domain::OperationContext& context) noexcept
{
    return implementation_->checkpoint(request, clientId, source, context);
}

Domain::Result<Domain::LegacyContinuityPersistOutcome>
LegacyContextContinuityService::handoff(
    const Domain::LegacyContinuityWriteRequest& request,
    const Domain::ClientId& clientId,
    const Domain::LegacyHandoffSource source,
    const Domain::OperationContext& context) noexcept
{
    return implementation_->handoff(request, clientId, source, context);
}

Domain::Result<Domain::LegacyContinuityPersistOutcome>
LegacyContextContinuityService::automaticPersist(
    const Domain::LegacyContinuityAutomaticRequest& request,
    const Domain::ClientId& clientId,
    const Domain::OperationContext& context) noexcept
{
    return implementation_->automaticPersist(request, clientId, context);
}

Domain::Result<Domain::LegacyContinuityPersistOutcome>
LegacyContextContinuityService::budgetHandoff(
    const Domain::ClientId& clientId,
    const std::string_view reason,
    const Domain::OperationContext& context) noexcept
{
    return implementation_->budgetHandoff(clientId, reason, context);
}

Domain::Result<Domain::LegacyContinuityGetOutcome>
LegacyContextContinuityService::get(
    const Domain::LegacyContinuityGetRequest& request,
    const Domain::OperationContext& context) noexcept
{
    return implementation_->get(request, context);
}

Domain::Result<Domain::LegacyContinuityListOutcome>
LegacyContextContinuityService::list(
    const Domain::LegacyContinuityListRequest& request,
    const Domain::OperationContext& context) noexcept
{
    return implementation_->list(request, context);
}

Domain::Result<Domain::LegacyContinuityStatusSummary>
LegacyContextContinuityService::statusSummary(
    const Domain::OperationContext& context) noexcept
{
    return implementation_->statusSummary(context);
}

Domain::Result<Domain::LegacyContinuityProjectionRepairOutcome>
LegacyContextContinuityService::repairProjections(
    const Domain::OperationContext& context) noexcept
{
    return implementation_->repairProjections(context);
}

Domain::Result<Domain::LegacyContinuityResetOutcome>
LegacyContextContinuityService::reset(
    const Domain::DestructiveConfirmation& confirmation,
    const Domain::OperationContext& context) noexcept
{
    return implementation_->reset(confirmation, context);
}

void LegacyContextContinuityService::shutdown() noexcept
{
    implementation_->shutdown();
}

} // namespace ForgeConductor::Application
