#include "ForgeConductor/Application/ManagedRunService.h"

#include "ForgeConductor/Domain/Utf8.h"

#include <nlohmann/json.hpp>

#include <condition_variable>
#include <map>
#include <limits>
#include <mutex>
#include <set>
#include <stop_token>
#include <thread>
#include <utility>
#include <vector>

namespace ForgeConductor::Application {
namespace {

[[nodiscard]] Domain::Error failure(
    const std::string_view code,
    const char* const message,
    const bool retryable = false)
{
    return Domain::makeError(code, message, retryable);
}

[[nodiscard]] Domain::Result<void> validate(
    const Domain::ManagedRunStartRequest& request,
    const Domain::OperationContext& context)
{
    if (context.isCancellationRequested()) {
        return Domain::Result<void>::failure(failure(
            Domain::ErrorCodes::Cancelled,
            "The managed run start was cancelled."));
    }
    if (request.authorityGeneration == 0U) {
        return Domain::Result<void>::failure(failure(
            Domain::ErrorCodes::InvalidRequest,
            "The managed run requires a nonzero project authority generation."));
    }
    if (request.task.empty() ||
        request.task.size() > Domain::MaximumManagedRunTaskBytes ||
        request.task.find('\0') != std::string::npos ||
        !Domain::isValidUtf8(request.task)) {
        return Domain::Result<void>::failure(failure(
            Domain::ErrorCodes::InvalidRequest,
            "The managed run task is empty, invalid, or exceeds its bound."));
    }
    if (request.instructionCursorAdvances.size() >
        Domain::MaximumManagedRunCursorAdvances) {
        return Domain::Result<void>::failure(failure(
            Domain::ErrorCodes::LimitExceeded,
            "The managed run cursor plan exceeds its durable bound."));
    }
    std::set<Domain::MemoryRecordId> cursorRecords;
    for (const auto& advance : request.instructionCursorAdvances) {
        if (advance.expectedVersion == 0U ||
            advance.expectedVersion ==
                (std::numeric_limits<std::uint32_t>::max)() ||
            advance.queueRowId.empty() ||
            advance.queueRowId.size() > 256U ||
            advance.targetEntry == 0U ||
            !cursorRecords.insert(advance.recordId).second) {
            return Domain::Result<void>::failure(failure(
                Domain::ErrorCodes::InvalidRequest,
                "The managed run cursor plan is invalid or duplicated."));
        }
    }
    return Domain::Result<void>::success();
}

[[nodiscard]] Domain::ManagedRunSnapshot snapshot(
    const Domain::ManagedRunRecord& record,
    const bool cancellationRequested,
    const bool pauseRequested = false)
{
    return Domain::ManagedRunSnapshot{
        record,
        true,
        cancellationRequested,
        pauseRequested};
}

[[nodiscard]] std::string continuityIdentity(
    const Domain::SessionId& runId,
    std::uint64_t sequence)
{
    auto value = runId.value();
    if (sequence == 0U) return value;
    const auto digitValue = [](const char digit) -> std::optional<unsigned> {
        if (digit >= '0' && digit <= '9') {
            return static_cast<unsigned>(digit - '0');
        }
        if (digit >= 'a' && digit <= 'f') {
            return static_cast<unsigned>(digit - 'a' + 10);
        }
        if (digit >= 'A' && digit <= 'F') {
            return static_cast<unsigned>(digit - 'A' + 10);
        }
        return std::nullopt;
    };
    constexpr std::string_view Hex{"0123456789abcdef"};
    for (auto cursor = value.rbegin();
         cursor != value.rend() && sequence != 0U; ++cursor) {
        if (*cursor == '-') continue;
        const auto digit = digitValue(*cursor);
        if (!digit) return {};
        const auto sum = *digit + static_cast<unsigned>(sequence & 0xFU);
        *cursor = Hex[sum & 0xFU];
        sequence = (sequence >> 4U) + (sum >> 4U);
    }
    return sequence == 0U ? value : std::string{};
}

[[nodiscard]] std::string_view utf8PrefixByBytes(
    const std::string_view value,
    const std::size_t maximumBytes) noexcept
{
    if (value.size() <= maximumBytes) return value;
    auto prefixBytes = maximumBytes;
    while (prefixBytes > 0U &&
           (static_cast<unsigned char>(value[prefixBytes]) & 0xc0U) == 0x80U) {
        --prefixBytes;
    }
    return value.substr(0U, prefixBytes);
}

} // namespace

class ManagedRunService::Impl final {
public:
    Impl(
        Contracts::IManagedResponsesTransport& transport,
        Contracts::IManagedRunStore& store,
        Contracts::IClock& clock,
        ManagedRunToolDependencies tools,
        ManagedRunContinuityDependencies continuity)
        : transport_{transport},
          store_{store},
          clock_{clock},
          tools_{tools},
          continuity_{std::move(continuity)}
    {
    }

    ~Impl() noexcept { shutdown(); }

    [[nodiscard]] Domain::Result<
        std::optional<Domain::ManagedRunSnapshot>> resolveReplay(
        const Domain::ManagedRunStartRequest& requested,
        const Domain::OperationContext& context) noexcept
    {
        try {
            auto prepared = prepareRequest(requested, context);
            if (!prepared) {
                return Domain::Result<
                    std::optional<Domain::ManagedRunSnapshot>>::failure(
                    std::move(prepared).error());
            }
            std::lock_guard admissionLock{admissionMutex_};
            return resolvePreparedReplay(prepared.value(), context);
        } catch (...) {
            return Domain::Result<
                std::optional<Domain::ManagedRunSnapshot>>::failure(failure(
                Domain::ErrorCodes::InternalFailure,
                "The managed run replay could not be resolved safely."));
        }
    }

    [[nodiscard]] Domain::Result<Domain::ManagedRunStartOutcome> start(
        const Domain::ManagedRunStartRequest& requested,
        const Domain::OperationContext& context) noexcept
    {
        try {
            auto prepared = prepareRequest(requested, context);
            if (!prepared) {
                return Domain::Result<Domain::ManagedRunStartOutcome>::failure(
                    std::move(prepared).error());
            }
            auto request = std::move(prepared).value();
            std::lock_guard admissionLock{admissionMutex_};
            auto replayed = resolvePreparedReplay(request, context);
            if (!replayed) {
                return Domain::Result<Domain::ManagedRunStartOutcome>::failure(
                    std::move(replayed).error());
            }
            if (replayed.value()) {
                return Domain::Result<Domain::ManagedRunStartOutcome>::success({
                    std::move(*replayed.value()),
                    Domain::ManagedRunAdmissionDisposition::Replayed});
            }

            const auto now = clock_.utcNow();
            Domain::ManagedRunRecord record{
                request.runId,
                request.projectId,
                request.clientId,
                request.task,
                request.authorityGeneration,
                Domain::ManagedRunState::Running,
                std::nullopt,
                0U,
                0U,
                std::nullopt,
                std::nullopt,
                std::nullopt,
                {},
                now,
                now,
                request.allowTools};
            record.automaticContinuity = request.automaticContinuity;
            record.admissionIdentity = request.admissionIdentity;
            record.dispatchPending =
                !request.instructionCursorAdvances.empty();
            record.instructionCursorAdvances =
                request.instructionCursorAdvances;
            record.dispatchPhase = record.dispatchPending
                ? Domain::ManagedRunDispatchPhase::CursorPending
                : Domain::ManagedRunDispatchPhase::Ready;
            record.dispatchOperationId = request.operationId;
            record.dispatchCorrelationId = request.correlationId;
            if (auto saved = store_.save(record, context); !saved) {
                return Domain::Result<Domain::ManagedRunStartOutcome>::failure(
                    std::move(saved).error());
            }

            auto active = std::make_shared<ActiveRun>(request, record);
            {
                std::lock_guard lock{mutex_};
                if (shutdown_) {
                    return Domain::Result<Domain::ManagedRunStartOutcome>::failure(
                        failure(
                            Domain::ErrorCodes::TransportClosed,
                            "The managed run service shut down during admission."));
                }
                const auto [_, inserted] =
                    active_.emplace(request.runId, active);
                if (!inserted) {
                    const auto& existing = active_.at(request.runId);
                    if (sameRequest(existing->request, request)) {
                        return Domain::Result<
                            Domain::ManagedRunStartOutcome>::success({
                            snapshot(existing->record, false),
                            Domain::ManagedRunAdmissionDisposition::Replayed});
                    }
                    return Domain::Result<
                        Domain::ManagedRunStartOutcome>::failure(failure(
                        Domain::ErrorCodes::Conflict,
                        "The managed run id was admitted concurrently."));
                }
            }
            auto released = releaseDispatch(active, context);
            if (!released) {
                return Domain::Result<Domain::ManagedRunStartOutcome>::failure(
                    std::move(released).error());
            }
            return Domain::Result<Domain::ManagedRunStartOutcome>::success({
                std::move(released).value(),
                Domain::ManagedRunAdmissionDisposition::Admitted});
        } catch (...) {
            return Domain::Result<Domain::ManagedRunStartOutcome>::failure(failure(
                Domain::ErrorCodes::InternalFailure,
                "The managed run could not be started safely."));
        }
    }

    [[nodiscard]] Domain::Result<Domain::ManagedRunSnapshot> status(
        const Domain::SessionId& runId,
        const Domain::OperationContext& context) noexcept
    {
        try {
            {
                std::lock_guard lock{mutex_};
                const auto found = active_.find(runId);
                if (found != active_.end()) {
                    return Domain::Result<Domain::ManagedRunSnapshot>::success(
                        snapshot(
                            found->second->record,
                            found->second->worker.get_stop_token()
                                .stop_requested(),
                            found->second->pauseRequested));
                }
            }
            auto persisted = store_.load(runId, context);
            if (!persisted) {
                return Domain::Result<Domain::ManagedRunSnapshot>::failure(
                    std::move(persisted).error());
            }
            if (!persisted.value()) {
                return Domain::Result<Domain::ManagedRunSnapshot>::failure(
                    failure(
                        Domain::ErrorCodes::SessionNotFound,
                        "The managed run was not found."));
            }
            return Domain::Result<Domain::ManagedRunSnapshot>::success(
                snapshot(*persisted.value(), false));
        } catch (...) {
            return Domain::Result<Domain::ManagedRunSnapshot>::failure(failure(
                Domain::ErrorCodes::InternalFailure,
                "The managed run status failed safely."));
        }
    }

    [[nodiscard]] Domain::Result<Domain::ManagedRunSnapshot> cancel(
        const Domain::SessionId& runId,
        const Domain::OperationContext& context) noexcept
    {
        try {
            std::lock_guard admissionLock{admissionMutex_};
            std::shared_ptr<ActiveRun> active;
            std::optional<Domain::ProviderSessionId> responseId;
            std::optional<Domain::ManagedRunRecord> cancelledPending;
            {
                std::lock_guard lock{mutex_};
                const auto found = active_.find(runId);
                if (found != active_.end()) {
                    active = found->second;
                    if (active->record.dispatchPending &&
                        !active->worker.joinable()) {
                        cancelledPending = active->record;
                        cancelledPending->state =
                            Domain::ManagedRunState::Cancelled;
                        cancelledPending->dispatchPending = false;
                        cancelledPending->instructionCursorAdvances.clear();
                        cancelledPending->dispatchPhase =
                            Domain::ManagedRunDispatchPhase::Ready;
                        cancelledPending->updatedAt = clock_.utcNow();
                    }
                    if (cancelledPending) {
                        responseId = active->record.providerResponseId;
                    } else {
                        if (active->record.state ==
                                Domain::ManagedRunState::Running ||
                            active->record.state ==
                                Domain::ManagedRunState::Paused) {
                            active->record.state =
                                Domain::ManagedRunState::Cancelling;
                            active->record.updatedAt = clock_.utcNow();
                        }
                        active->pauseRequested = false;
                        active->worker.request_stop();
                        responseId = active->record.providerResponseId;
                        active->boundaryChanged.notify_all();
                    }
                }
            }
            if (!active) {
                auto persisted = store_.load(runId, context);
                if (!persisted) {
                    return Domain::Result<
                        Domain::ManagedRunSnapshot>::failure(
                        std::move(persisted).error());
                }
                if (!persisted.value()) {
                    return Domain::Result<
                        Domain::ManagedRunSnapshot>::failure(failure(
                        Domain::ErrorCodes::SessionNotFound,
                        "The managed run was not found."));
                }
                auto durable = std::move(*persisted.value());
                if (!terminal(durable.state) &&
                    (durable.dispatchPhase ==
                            Domain::ManagedRunDispatchPhase::CursorPending ||
                     durable.dispatchPhase ==
                            Domain::ManagedRunDispatchPhase::Ready)) {
                    durable.state = Domain::ManagedRunState::Cancelled;
                    durable.dispatchPending = false;
                    durable.instructionCursorAdvances.clear();
                    durable.dispatchPhase =
                        Domain::ManagedRunDispatchPhase::Ready;
                    durable.updatedAt = clock_.utcNow();
                    if (auto saved = store_.save(durable, context); !saved) {
                        return Domain::Result<
                            Domain::ManagedRunSnapshot>::failure(
                            std::move(saved).error());
                    }
                    return Domain::Result<
                        Domain::ManagedRunSnapshot>::success(
                        snapshot(durable, true));
                }
                return Domain::Result<Domain::ManagedRunSnapshot>::success(
                    snapshot(durable, false));
            }
            if (cancelledPending) {
                if (auto saved = store_.save(*cancelledPending, context);
                    !saved) {
                    return Domain::Result<
                        Domain::ManagedRunSnapshot>::failure(
                        std::move(saved).error());
                }
                std::lock_guard lock{mutex_};
                active->record = *cancelledPending;
                active->request.instructionCursorAdvances.clear();
                return Domain::Result<Domain::ManagedRunSnapshot>::success(
                    snapshot(active->record, true));
            }
            transport_.cancel(
                active->request.operationId,
                responseId);
            if (tools_.router) {
                tools_.router->cancel(active->request.operationId);
            }
            return Domain::Result<Domain::ManagedRunSnapshot>::success(
                snapshot(active->record, true));
        } catch (...) {
            return Domain::Result<Domain::ManagedRunSnapshot>::failure(failure(
                Domain::ErrorCodes::InternalFailure,
                "The managed run cancellation failed safely."));
        }
    }

    [[nodiscard]] Domain::Result<Domain::ManagedRunSnapshot> pause(
        const Domain::SessionId& runId,
        const Domain::OperationContext& context) noexcept
    {
        try {
            std::shared_ptr<ActiveRun> active;
            {
                std::lock_guard lock{mutex_};
                const auto found = active_.find(runId);
                if (found != active_.end()) {
                    active = found->second;
                    if (active->record.state == Domain::ManagedRunState::Running) {
                        active->pauseRequested = true;
                    }
                    return Domain::Result<Domain::ManagedRunSnapshot>::success(
                        snapshot(
                            active->record,
                            active->worker.get_stop_token().stop_requested(),
                            active->pauseRequested));
                }
            }
            return status(runId, context);
        } catch (...) {
            return Domain::Result<Domain::ManagedRunSnapshot>::failure(failure(
                Domain::ErrorCodes::InternalFailure,
                "The managed run pause request failed safely."));
        }
    }

    [[nodiscard]] Domain::Result<Domain::ManagedRunSnapshot> resume(
        const Domain::SessionId& runId,
        const Domain::OperationContext& context) noexcept
    {
        try {
            std::shared_ptr<ActiveRun> active;
            std::optional<Domain::ManagedRunRecord> record;
            bool foundActive{};
            {
                std::lock_guard lock{mutex_};
                const auto found = active_.find(runId);
                if (found != active_.end()) {
                    foundActive = true;
                    active = found->second;
                    if (active->record.state == Domain::ManagedRunState::Paused ||
                        (active->record.state == Domain::ManagedRunState::Running &&
                         active->pauseRequested)) {
                        active->pauseRequested = false;
                        if (active->record.state == Domain::ManagedRunState::Paused) {
                            active->record.state = Domain::ManagedRunState::Running;
                            active->record.updatedAt = clock_.utcNow();
                        }
                        record.emplace(active->record);
                        active->boundaryChanged.notify_all();
                    } else {
                        return Domain::Result<Domain::ManagedRunSnapshot>::success(
                            snapshot(
                                active->record,
                                active->worker.get_stop_token().stop_requested(),
                                active->pauseRequested));
                    }
                }
            }
            if (!foundActive) {
                return status(runId, context);
            }
            auto saved = store_.save(*record, context);
            if (!saved) {
                return Domain::Result<Domain::ManagedRunSnapshot>::failure(
                    std::move(saved).error());
            }
            return Domain::Result<Domain::ManagedRunSnapshot>::success(
                snapshot(*record, false, false));
        } catch (...) {
            return Domain::Result<Domain::ManagedRunSnapshot>::failure(failure(
                Domain::ErrorCodes::InternalFailure,
                "The managed run resume request failed safely."));
        }
    }

    void shutdown() noexcept
    {
        std::vector<std::shared_ptr<ActiveRun>> active;
        try {
            {
                std::lock_guard lock{mutex_};
                if (shutdown_) {
                    return;
                }
                shutdown_ = true;
                active.reserve(active_.size());
                for (const auto& entry : active_) {
                    active.push_back(entry.second);
                }
            }
            for (const auto& run : active) {
                run->pauseRequested = false;
                run->worker.request_stop();
                run->boundaryChanged.notify_all();
                transport_.cancel(
                    run->request.operationId,
                    std::nullopt);
                if (tools_.router) {
                    tools_.router->cancel(run->request.operationId);
                }
            }
            for (const auto& run : active) {
                if (run->worker.joinable()) {
                    run->worker.join();
                }
            }
        } catch (...) {
        }
    }

private:
    struct ActiveRun final {
        ActiveRun(
            Domain::ManagedRunStartRequest value,
            Domain::ManagedRunRecord initial)
            : request{std::move(value)}, record{std::move(initial)}
        {
        }

        Domain::ManagedRunStartRequest request;
        Domain::ManagedRunRecord record;
        bool pauseRequested{};
        std::condition_variable_any boundaryChanged;
        std::jthread worker;
    };

    [[nodiscard]] static bool terminal(
        const Domain::ManagedRunState state) noexcept
    {
        return state == Domain::ManagedRunState::Completed ||
            state == Domain::ManagedRunState::Failed ||
            state == Domain::ManagedRunState::Cancelled;
    }

    [[nodiscard]] static Domain::ManagedRunStartRequest requestForRecord(
        const Domain::ManagedRunRecord& record,
        const Domain::ManagedRunStartRequest& incoming)
    {
        return Domain::ManagedRunStartRequest{
            record.runId,
            record.projectId,
            record.clientId,
            record.dispatchOperationId.value_or(incoming.operationId),
            record.dispatchCorrelationId.value_or(incoming.correlationId),
            record.authorityGeneration,
            record.task,
            record.allowTools,
            record.automaticContinuity,
            record.admissionIdentity,
            record.instructionCursorAdvances};
    }

    [[nodiscard]] Domain::Result<
        std::vector<Domain::UpdateProjectMemoryRequest>>
    pendingCursorUpdates(
        const Domain::ManagedRunRecord& record,
        const Domain::OperationContext& context)
    {
        if (record.instructionCursorAdvances.empty()) {
            return Domain::Result<
                std::vector<Domain::UpdateProjectMemoryRequest>>::success({});
        }
        if (!record.dispatchCorrelationId) {
            return Domain::Result<
                std::vector<Domain::UpdateProjectMemoryRequest>>::failure(
                failure(
                    Domain::ErrorCodes::IntegrityFailure,
                    "The durable managed-run cursor plan has no dispatch correlation."));
        }
        if (tools_.projectMemory == nullptr) {
            return Domain::Result<
                std::vector<Domain::UpdateProjectMemoryRequest>>::failure(
                failure(
                    Domain::ErrorCodes::IntegrityFailure,
                    "Managed-run cursor admission is unavailable."));
        }
        std::vector<Domain::MemoryRecordId> ids;
        ids.reserve(record.instructionCursorAdvances.size());
        for (const auto& advance : record.instructionCursorAdvances) {
            ids.push_back(advance.recordId);
        }
        auto loaded = tools_.projectMemory->get(
            Domain::GetProjectMemoryRequest{
                record.projectId, std::move(ids), true, 256U * 1024U},
            context);
        if (!loaded) {
            return Domain::Result<
                std::vector<Domain::UpdateProjectMemoryRequest>>::failure(
                std::move(loaded).error());
        }
        std::map<Domain::MemoryRecordId, Domain::ProjectMemoryRecord> byId;
        for (auto& stored : loaded.value().records) {
            byId.insert_or_assign(stored.id, std::move(stored));
        }
        std::vector<Domain::UpdateProjectMemoryRequest> updates;
        updates.reserve(record.instructionCursorAdvances.size());
        std::size_t alreadyApplied{};
        try {
            for (const auto& advance : record.instructionCursorAdvances) {
                const auto found = byId.find(advance.recordId);
                if (found == byId.end() || !found->second.body) {
                    return Domain::Result<
                        std::vector<Domain::UpdateProjectMemoryRequest>>::failure(
                        failure(
                            Domain::ErrorCodes::Conflict,
                            "A durable managed-run cursor row is unavailable."));
                }
                const auto& stored = found->second;
                auto document = nlohmann::json::parse(*stored.body);
                if (document.value("schema", std::string{}) !=
                        "forge-instruction-package-queue-v2" ||
                    document.value("project_id", std::string{}) !=
                        record.projectId.value() ||
                    document.value("queue_row_id", std::string{}) !=
                        advance.queueRowId ||
                    !document.contains("cursor") ||
                    !document.at("cursor").is_object()) {
                    return Domain::Result<
                        std::vector<Domain::UpdateProjectMemoryRequest>>::failure(
                        failure(
                            Domain::ErrorCodes::IntegrityFailure,
                            "A durable managed-run cursor row changed identity."));
                }
                const auto currentEntry = document.at("cursor").value(
                    "entry", std::uint64_t{});
                const auto targetState =
                    advance.completed ? "completed" : "active";
                if (stored.version ==
                        static_cast<std::uint32_t>(
                            advance.expectedVersion + 1U) &&
                    currentEntry == advance.targetEntry &&
                    document.value("state", std::string{}) == targetState) {
                    if (document.value(
                            "correlation_id", std::string{}) ==
                            record.dispatchCorrelationId->value() &&
                        document.value(
                            "managed_run_id", std::string{}) ==
                            record.runId.value()) {
                        ++alreadyApplied;
                        continue;
                    }
                    return Domain::Result<
                        std::vector<Domain::UpdateProjectMemoryRequest>>::failure(
                        failure(
                            Domain::ErrorCodes::Conflict,
                            "The durable managed-run cursor row was advanced by another admission."));
                }
                if (stored.version != advance.expectedVersion ||
                    currentEntry > advance.targetEntry) {
                    return Domain::Result<
                        std::vector<Domain::UpdateProjectMemoryRequest>>::failure(
                        failure(
                            Domain::ErrorCodes::Conflict,
                            "A durable managed-run cursor row changed before dispatch."));
                }
                document["cursor"] = {
                    {"entry", advance.targetEntry}, {"byte_offset", 0U}};
                document["state"] = targetState;
                document["correlation_id"] =
                    record.dispatchCorrelationId->value();
                document["managed_run_id"] = record.runId.value();
                document["last_error"] = nullptr;
                updates.push_back(Domain::UpdateProjectMemoryRequest{
                    record.projectId,
                    advance.recordId,
                    advance.expectedVersion,
                    std::nullopt,
                    std::optional<std::string>{
                        "Execution cursor advanced to entry " +
                        std::to_string(advance.targetEntry)},
                    std::optional<std::string>{document.dump()},
                    std::nullopt});
            }
        } catch (...) {
            return Domain::Result<
                std::vector<Domain::UpdateProjectMemoryRequest>>::failure(
                failure(
                    Domain::ErrorCodes::IntegrityFailure,
                    "A durable managed-run cursor row is malformed."));
        }
        if (alreadyApplied != 0U && !updates.empty()) {
            return Domain::Result<
                std::vector<Domain::UpdateProjectMemoryRequest>>::failure(
                failure(
                    Domain::ErrorCodes::Conflict,
                    "The durable managed-run cursor plan is only partially applied."));
        }
        return Domain::Result<
            std::vector<Domain::UpdateProjectMemoryRequest>>::success(
            std::move(updates));
    }

    [[nodiscard]] Domain::Result<void> commitInstructionCursors(
        const Domain::ManagedRunRecord& record,
        const Domain::OperationContext& context)
    {
        auto pending = pendingCursorUpdates(record, context);
        if (!pending) {
            return Domain::Result<void>::failure(
                std::move(pending).error());
        }
        if (pending.value().empty()) {
            return Domain::Result<void>::success();
        }
        Domain::Result<void> committed = Domain::Result<void>::success();
        if (pending.value().size() == 1U) {
            auto updated = tools_.projectMemory->update(
                pending.value().front(), context);
            if (!updated) {
                committed = Domain::Result<void>::failure(
                    std::move(updated).error());
            }
        } else {
            auto updated = tools_.projectMemory->updateBatch(
                Domain::UpdateProjectMemoryBatchRequest{
                    record.projectId, pending.value()},
                context);
            if (!updated) {
                committed = Domain::Result<void>::failure(
                    std::move(updated).error());
            }
        }
        if (committed) {
            return committed;
        }
        auto originalError = std::move(committed).error();
        auto reconciled = pendingCursorUpdates(record, context);
        if (reconciled && reconciled.value().empty()) {
            return Domain::Result<void>::success();
        }
        return Domain::Result<void>::failure(std::move(originalError));
    }

    [[nodiscard]] Domain::Result<Domain::ManagedRunSnapshot> releaseDispatch(
        const std::shared_ptr<ActiveRun>& active,
        const Domain::OperationContext& context)
    {
        {
            std::lock_guard lock{mutex_};
            if (active->worker.joinable() || terminal(active->record.state)) {
                return Domain::Result<Domain::ManagedRunSnapshot>::success(
                    snapshot(
                        active->record,
                        active->worker.joinable() &&
                            active->worker.get_stop_token().stop_requested(),
                        active->pauseRequested));
            }
        }
        const bool cursorPending = active->record.dispatchPhase ==
            Domain::ManagedRunDispatchPhase::CursorPending;
        if (cursorPending != active->record.dispatchPending ||
            (cursorPending &&
                active->record.instructionCursorAdvances.empty()) ||
            (!cursorPending &&
                !active->record.instructionCursorAdvances.empty())) {
            return Domain::Result<Domain::ManagedRunSnapshot>::failure(
                failure(
                    Domain::ErrorCodes::IntegrityFailure,
                    "The managed run dispatch phase and cursor plan disagree."));
        }
        if (active->record.dispatchPhase ==
            Domain::ManagedRunDispatchPhase::ProviderClaimed) {
            auto indeterminate = active->record;
            indeterminate.state = Domain::ManagedRunState::Failed;
            indeterminate.lastError = failure(
                Domain::ErrorCodes::Conflict,
                "The previous provider dispatch outcome is indeterminate; it was not replayed.");
            indeterminate.updatedAt = clock_.utcNow();
            if (auto saved = store_.save(indeterminate, context); !saved) {
                return Domain::Result<Domain::ManagedRunSnapshot>::failure(
                    std::move(saved).error());
            }
            {
                std::lock_guard lock{mutex_};
                active->record = std::move(indeterminate);
            }
            return Domain::Result<Domain::ManagedRunSnapshot>::success(
                snapshot(active->record, false));
        }
        if (cursorPending) {
            auto committed = commitInstructionCursors(active->record, context);
            if (!committed) {
                return Domain::Result<Domain::ManagedRunSnapshot>::failure(
                    std::move(committed).error());
            }
            auto released = active->record;
            released.dispatchPending = false;
            released.instructionCursorAdvances.clear();
            released.dispatchPhase =
                Domain::ManagedRunDispatchPhase::Ready;
            released.updatedAt = clock_.utcNow();
            if (auto saved = store_.save(released, context); !saved) {
                return Domain::Result<Domain::ManagedRunSnapshot>::failure(
                    std::move(saved).error());
            }
            {
                std::lock_guard lock{mutex_};
                active->record = std::move(released);
                active->request.instructionCursorAdvances.clear();
            }
        }
        if (active->record.dispatchPhase !=
            Domain::ManagedRunDispatchPhase::Ready) {
            return Domain::Result<Domain::ManagedRunSnapshot>::failure(
                failure(
                    Domain::ErrorCodes::IntegrityFailure,
                    "The managed run is not ready for provider dispatch."));
        }
        std::lock_guard lock{mutex_};
        if (shutdown_) {
            return Domain::Result<Domain::ManagedRunSnapshot>::failure(
                failure(
                    Domain::ErrorCodes::TransportClosed,
                    "The managed run service shut down during dispatch."));
        }
        if (!terminal(active->record.state) && !active->worker.joinable()) {
            const auto request = active->request;
            active->worker = std::jthread{
                [this, request](const std::stop_token token) {
                    execute(request, token);
                }};
        }
        return Domain::Result<Domain::ManagedRunSnapshot>::success(
            snapshot(
                active->record,
                active->worker.joinable() &&
                    active->worker.get_stop_token().stop_requested(),
                active->pauseRequested));
    }

    [[nodiscard]] Domain::Result<
        std::optional<Domain::ManagedRunSnapshot>> resolvePreparedReplay(
        const Domain::ManagedRunStartRequest& request,
        const Domain::OperationContext& context)
    {
        std::shared_ptr<ActiveRun> active;
        {
            std::lock_guard lock{mutex_};
            if (shutdown_) {
                return Domain::Result<
                    std::optional<Domain::ManagedRunSnapshot>>::failure(
                    failure(
                        Domain::ErrorCodes::TransportClosed,
                        "The managed run service is shut down."));
            }
            const auto found = active_.find(request.runId);
            if (found != active_.end()) {
                if (!sameRequest(found->second->request, request)) {
                    return Domain::Result<
                        std::optional<Domain::ManagedRunSnapshot>>::failure(
                        failure(
                            Domain::ErrorCodes::Conflict,
                            "The managed run id is already bound to another request."));
                }
                active = found->second;
            }
        }
        if (active) {
            auto released = releaseDispatch(active, context);
            return released
                ? Domain::Result<
                      std::optional<Domain::ManagedRunSnapshot>>::success(
                      std::move(released).value())
                : Domain::Result<
                      std::optional<Domain::ManagedRunSnapshot>>::failure(
                      std::move(released).error());
        }

        auto persisted = store_.load(request.runId, context);
        if (!persisted) {
            return Domain::Result<
                std::optional<Domain::ManagedRunSnapshot>>::failure(
                std::move(persisted).error());
        }
        if (!persisted.value()) {
            return Domain::Result<
                std::optional<Domain::ManagedRunSnapshot>>::success(
                std::nullopt);
        }
        if (!sameDurableRequest(*persisted.value(), request)) {
            return Domain::Result<
                std::optional<Domain::ManagedRunSnapshot>>::failure(
                failure(
                    Domain::ErrorCodes::Conflict,
                    "The durable managed run id belongs to another request."));
        }
        if (terminal(persisted.value()->state)) {
            return Domain::Result<
                std::optional<Domain::ManagedRunSnapshot>>::success(
                snapshot(*persisted.value(), false));
        }
        auto resumed = std::make_shared<ActiveRun>(
            requestForRecord(*persisted.value(), request),
            *persisted.value());
        {
            std::lock_guard lock{mutex_};
            if (shutdown_) {
                return Domain::Result<
                    std::optional<Domain::ManagedRunSnapshot>>::failure(
                    failure(
                        Domain::ErrorCodes::TransportClosed,
                        "The managed run service shut down during replay."));
            }
            const auto [found, inserted] =
                active_.emplace(request.runId, resumed);
            if (!inserted) {
                if (!sameRequest(found->second->request, request)) {
                    return Domain::Result<
                        std::optional<Domain::ManagedRunSnapshot>>::failure(
                        failure(
                            Domain::ErrorCodes::Conflict,
                            "The managed run id was resumed concurrently."));
                }
                resumed = found->second;
            }
        }
        auto released = releaseDispatch(resumed, context);
        return released
            ? Domain::Result<
                  std::optional<Domain::ManagedRunSnapshot>>::success(
                  std::move(released).value())
            : Domain::Result<
                  std::optional<Domain::ManagedRunSnapshot>>::failure(
                  std::move(released).error());
    }

    [[nodiscard]] Domain::Result<Domain::ManagedRunStartRequest> prepareRequest(
        const Domain::ManagedRunStartRequest& requested,
        const Domain::OperationContext& context)
    {
        auto request = requested;
        if (request.authorityGeneration == 0U && tools_.workspaceAuthority) {
            auto resolved = tools_.workspaceAuthority->authorityFor(
                request.projectId, context);
            if (!resolved) {
                return Domain::Result<Domain::ManagedRunStartRequest>::failure(
                    std::move(resolved).error());
            }
            request.authorityGeneration = resolved.value().generation();
            request.clientId = resolved.value().callerId();
        }
        if (auto valid = validate(request, context); !valid) {
            return Domain::Result<Domain::ManagedRunStartRequest>::failure(
                std::move(valid).error());
        }
        return Domain::Result<Domain::ManagedRunStartRequest>::success(
            std::move(request));
    }

    [[nodiscard]] static bool sameRequest(
        const Domain::ManagedRunStartRequest& left,
        const Domain::ManagedRunStartRequest& right) noexcept
    {
        const bool stableAdmission =
            left.admissionIdentity && right.admissionIdentity;
        return left.runId == right.runId &&
            left.projectId == right.projectId &&
            left.clientId == right.clientId &&
            left.authorityGeneration == right.authorityGeneration &&
            left.allowTools == right.allowTools &&
            (stableAdmission
                ? left.admissionIdentity == right.admissionIdentity
                : left.operationId == right.operationId &&
                    left.task == right.task &&
                    left.automaticContinuity == right.automaticContinuity);
    }

    [[nodiscard]] static bool sameDurableRequest(
        const Domain::ManagedRunRecord& record,
        const Domain::ManagedRunStartRequest& request) noexcept
    {
        const bool stableAdmission =
            record.admissionIdentity && request.admissionIdentity;
        return record.runId == request.runId &&
            record.projectId == request.projectId &&
            record.clientId == request.clientId &&
            record.authorityGeneration == request.authorityGeneration &&
            record.allowTools == request.allowTools &&
            (stableAdmission
                ? record.admissionIdentity == request.admissionIdentity
                : record.task == request.task &&
                    record.automaticContinuity ==
                        request.automaticContinuity);
    }

    void publishActive(const Domain::ManagedRunRecord& record) noexcept
    {
        try {
            std::lock_guard lock{mutex_};
            const auto found = active_.find(record.runId);
            if (found != active_.end()) {
                found->second->record = record;
            }
        } catch (...) {
        }
    }

    [[nodiscard]] bool awaitDispatchBoundary(
        const Domain::ManagedRunStartRequest& request,
        Domain::ManagedRunRecord& record,
        const Domain::OperationContext& persistenceContext,
        const std::stop_token token) noexcept
    {
        try {
            std::shared_ptr<ActiveRun> active;
            {
                std::lock_guard lock{mutex_};
                const auto found = active_.find(request.runId);
                if (found == active_.end()) {
                    record.state = Domain::ManagedRunState::Failed;
                    record.lastError = failure(
                        Domain::ErrorCodes::IntegrityFailure,
                        "The managed run owner disappeared before a dispatch boundary.");
                    return false;
                }
                active = found->second;
            }
            for (;;) {
                bool shouldPause{};
                {
                    std::lock_guard lock{mutex_};
                    shouldPause = active->pauseRequested;
                }
                if (!shouldPause) {
                    if (token.stop_requested()) {
                        record.state = Domain::ManagedRunState::Cancelled;
                        return false;
                    }
                    if (record.state == Domain::ManagedRunState::Paused) {
                        record.state = Domain::ManagedRunState::Running;
                        record.updatedAt = clock_.utcNow();
                        publishActive(record);
                        if (auto saved = store_.save(record, persistenceContext); !saved) {
                            record.state = Domain::ManagedRunState::Failed;
                            record.lastError = saved.error();
                            return false;
                        }
                    }
                    return true;
                }

                if (record.state != Domain::ManagedRunState::Paused) {
                    record.state = Domain::ManagedRunState::Paused;
                    record.updatedAt = clock_.utcNow();
                    publishActive(record);
                    if (auto saved = store_.save(record, persistenceContext); !saved) {
                        record.state = Domain::ManagedRunState::Failed;
                        record.lastError = saved.error();
                        return false;
                    }
                }
                std::unique_lock lock{mutex_};
                active->boundaryChanged.wait(lock, token, [&] {
                    return !active->pauseRequested;
                });
                if (token.stop_requested()) {
                    record.state = Domain::ManagedRunState::Cancelled;
                    return false;
                }
            }
        } catch (...) {
            record.state = Domain::ManagedRunState::Failed;
            record.lastError = failure(
                Domain::ErrorCodes::InternalFailure,
                "The managed run dispatch boundary failed safely.");
            return false;
        }
    }

    [[nodiscard]] Domain::Result<
        std::optional<Domain::ContinuityAutomationOutcome>>
    observeContinuity(
        const Domain::ManagedRunStartRequest& request,
        const Domain::ManagedRunRecord& record,
        const std::vector<Domain::ContinuityWorkEntry>& completedToolWork,
        const Domain::SessionId& predecessorSessionId,
        const std::uint64_t sequence,
        const Domain::OperationContext& context) noexcept
    {
        if (!request.automaticContinuity || !record.retainedContextTokens ||
            !continuity_.automation || !continuity_.codec ||
            !continuity_.projects || !continuity_.adapterId ||
            continuity_.contextCapacity == 0U) {
            return Domain::Result<
                std::optional<Domain::ContinuityAutomationOutcome>>::success(
                std::nullopt);
        }
        auto descriptor = continuity_.projects->descriptor(
            request.projectId, context);
        if (!descriptor) {
            return Domain::Result<
                std::optional<Domain::ContinuityAutomationOutcome>>::failure(
                std::move(descriptor).error());
        }
        if (descriptor.value().aliases.empty()) {
            return Domain::Result<
                std::optional<Domain::ContinuityAutomationOutcome>>::failure(
                failure(
                    Domain::ErrorCodes::IntegrityFailure,
                    "The managed run project has no canonical workspace alias."));
        }
        const auto identity = continuityIdentity(request.runId, sequence);
        auto handoffId = Domain::ContinuityHandoffId::parse(identity);
        auto operationId = Domain::ContinuityOperationId::parse(identity);
        auto placeholder = Domain::Sha256Digest::parse(std::string(64U, '0'));
        if (!handoffId || !operationId || !placeholder) {
            return Domain::Result<
                std::optional<Domain::ContinuityAutomationOutcome>>::failure(
                failure(
                    Domain::ErrorCodes::IntegrityFailure,
                    "The managed run identity cannot form continuity identity."));
        }
        auto completedWork = completedToolWork;
        if (record.outputText) {
            auto summary = *record.outputText;
            summary.resize(utf8PrefixByBytes(summary, 2U * 1024U).size());
            completedWork.push_back({
                std::nullopt,
                std::move(summary),
                std::optional<std::string>{"completed"}});
        }
        std::vector<Domain::ContinuityWorkEntry> openWork;
        if (!record.pendingFunctionCalls.empty()) {
            openWork.push_back({
                std::optional<std::string>{"pending-provider-tools"},
                "Resolve pending provider tool calls without repeating uncertain effects.",
                std::optional<std::string>{"open"}});
        }
        auto mission = std::string{utf8PrefixByBytes(
            request.task, 8U * 1024U)};
        auto activeFiles = descriptor.value().aliases;
        if (activeFiles.size() > Domain::MaximumContinuityHandoffListItems) {
            activeFiles.erase(
                activeFiles.begin() +
                    static_cast<std::ptrdiff_t>(
                        Domain::MaximumContinuityHandoffListItems),
                activeFiles.end());
        }
        Domain::ContinuityHandoff draft{
            std::move(handoffId).value(),
            std::move(operationId).value(),
            record.updatedAt,
            Domain::ContinuityProject{
                request.projectId,
                descriptor.value().displayName,
                descriptor.value().aliases.front(),
                "unknown",
                "unknown",
                {}},
            Domain::ContinuitySession{
                predecessorSessionId,
                record.providerResponseId,
                continuity_.model,
                continuity_.provider},
            std::nullopt,
            std::move(mission),
            {
                "Continue only from the canonical context handoff.",
                "Do not repeat a tool effect whose completion is uncertain."},
            Domain::ContinuityCurrentWork{
                "R1",
                request.runId.value(),
                record.pendingFunctionCalls.empty()
                    ? "Continue the Manager-owned task."
                    : "Resolve pending provider tool calls without repeating uncertain effects.",
                std::move(activeFiles)},
            std::move(completedWork),
            std::move(openWork),
            {{"The Manager owns ordinary inference and successor activation.",
              std::nullopt}},
            Domain::ContinuityValidation{{}, {}, {}},
            {},
            {},
            {{1U,
              "Resume the Manager-owned task from this handoff.",
              "",
              "Useful work continues under the acknowledged successor."}},
            Domain::ContinuityHostState{
                *continuity_.adapterId,
                Domain::ContinuityState::Idle,
                "provider_usage",
                {},
                std::nullopt},
            std::move(placeholder).value(),
            true};
        auto document = continuity_.codec->encode(draft, context);
        if (!document) {
            return Domain::Result<
                std::optional<Domain::ContinuityAutomationOutcome>>::failure(
                std::move(document).error());
        }
        auto observed = continuity_.automation->observe(
            Domain::ContinuityAutomationObservation{
                std::move(document).value().handoff,
                Domain::ContextBudgetSignals{
                    continuity_.contextCapacity,
                    continuity_.reservedTokens,
                    std::nullopt,
                    record.retainedContextTokens,
                    std::nullopt,
                    std::nullopt,
                    false},
                false},
            context);
        if (!observed) {
            return Domain::Result<
                std::optional<Domain::ContinuityAutomationOutcome>>::failure(
                std::move(observed).error());
        }
        return Domain::Result<
            std::optional<Domain::ContinuityAutomationOutcome>>::success(
            std::move(observed).value());
    }

    [[nodiscard]] bool claimProviderDispatch(
        const Domain::ManagedRunStartRequest& request,
        Domain::ManagedRunRecord& record,
        const Domain::OperationContext& persistenceContext,
        const std::stop_token token) noexcept
    {
        try {
            std::lock_guard admissionLock{admissionMutex_};
            {
                std::lock_guard lock{mutex_};
                const auto found = active_.find(request.runId);
                if (found == active_.end()) {
                    return false;
                }
                record = found->second->record;
            }
            if (token.stop_requested() ||
                record.state == Domain::ManagedRunState::Cancelling ||
                record.state == Domain::ManagedRunState::Cancelled) {
                record.state = Domain::ManagedRunState::Cancelled;
                record.dispatchPending = false;
                record.instructionCursorAdvances.clear();
                record.dispatchPhase = Domain::ManagedRunDispatchPhase::Ready;
                record.updatedAt = clock_.utcNow();
                static_cast<void>(store_.save(record, persistenceContext));
                publishActive(record);
                return false;
            }
            if (record.state != Domain::ManagedRunState::Running ||
                record.dispatchPending ||
                !record.instructionCursorAdvances.empty() ||
                record.dispatchPhase !=
                    Domain::ManagedRunDispatchPhase::Ready) {
                record.state = Domain::ManagedRunState::Failed;
                record.lastError = failure(
                    Domain::ErrorCodes::IntegrityFailure,
                    "The managed run provider dispatch was not durably ready.");
                record.updatedAt = clock_.utcNow();
                static_cast<void>(store_.save(record, persistenceContext));
                publishActive(record);
                return false;
            }
            auto claimed = record;
            claimed.dispatchPhase =
                Domain::ManagedRunDispatchPhase::ProviderClaimed;
            claimed.updatedAt = clock_.utcNow();
            if (auto saved = store_.save(claimed, persistenceContext); !saved) {
                record.state = Domain::ManagedRunState::Failed;
                record.lastError = saved.error();
                record.updatedAt = clock_.utcNow();
                static_cast<void>(store_.save(record, persistenceContext));
                publishActive(record);
                return false;
            }
            record = std::move(claimed);
            publishActive(record);
            return true;
        } catch (...) {
            record.state = Domain::ManagedRunState::Failed;
            record.lastError = failure(
                Domain::ErrorCodes::InternalFailure,
                "The managed run provider dispatch could not be claimed safely.");
            record.updatedAt = clock_.utcNow();
            publishActive(record);
            return false;
        }
    }

    void execute(
        const Domain::ManagedRunStartRequest& request,
        const std::stop_token token) noexcept
    {
        const Domain::OperationContext providerContext{
            request.operationId,
            Domain::MonotonicTimePoint::max(),
            token,
            request.correlationId};
        Domain::ManagedRunRecord record = [&] {
            std::lock_guard lock{mutex_};
            return active_.at(request.runId)->record;
        }();
        const Domain::OperationContext persistenceContext{
            request.operationId,
            Domain::MonotonicTimePoint::max(),
            {},
            request.correlationId};

        if (!claimProviderDispatch(
                request, record, persistenceContext, token)) {
            return;
        }

        if (token.stop_requested()) {
            record.state = Domain::ManagedRunState::Cancelled;
            record.updatedAt = clock_.utcNow();
        }

        std::optional<Contracts::WorkspaceAuthority> authority;
        std::vector<Domain::McpToolDescriptor> descriptors;
        if (tools_.workspaceAuthority &&
            (!request.allowTools || (tools_.catalog && tools_.router))) {
            auto resolved = tools_.workspaceAuthority->authorityFor(
                request.projectId, providerContext);
            if (!resolved) {
                record.lastError = resolved.error();
                record.state = Domain::ManagedRunState::Failed;
            } else if (resolved.value().generation() !=
                           request.authorityGeneration ||
                       resolved.value().callerId() != request.clientId) {
                record.lastError = failure(
                    Domain::ErrorCodes::Unauthorized,
                    "The managed run authority generation or client is stale.");
                record.state = Domain::ManagedRunState::Failed;
            } else {
                if (request.allowTools) {
                    authority.emplace(std::move(resolved).value());
                    const auto available = tools_.catalog->tools();
                    descriptors.assign(available.begin(), available.end());
                }
            }
        }

        std::string input = request.task;
        std::vector<Domain::ManagedFunctionCallOutput> toolOutputs;
        std::vector<Domain::ContinuityWorkEntry> completedToolWork;
        std::set<Domain::ProviderSessionId> observedResponses;
        Domain::SessionId continuityPredecessorSessionId = request.runId;
        std::uint64_t continuitySequence{};
        std::optional<Domain::ContinuityOperationId>
            pendingContinuityCheckpoint;
        while (record.state == Domain::ManagedRunState::Running) {
            if (!awaitDispatchBoundary(
                    request, record, persistenceContext, token)) {
                break;
            }
            Domain::ManagedProviderTurnRequest turn{
                request.projectId,
                request.runId,
                request.authorityGeneration,
                std::move(input),
                record.providerResponseId,
                descriptors,
                std::move(toolOutputs)};
            auto outcome = transport_.complete(turn, providerContext);
            record.updatedAt = clock_.utcNow();
            if (!outcome) {
                record.lastError = outcome.error();
                record.state = token.stop_requested() ||
                        outcome.error().code == Domain::ErrorCodes::Cancelled
                    ? Domain::ManagedRunState::Cancelled
                    : Domain::ManagedRunState::Failed;
                break;
            }

            auto value = std::move(outcome).value();
            if (!observedResponses.insert(value.responseId).second) {
                record.lastError = failure(
                    Domain::ErrorCodes::IntegrityFailure,
                    "The provider repeated a response identity; usage and effects were not replayed.");
                record.state = Domain::ManagedRunState::Failed;
                break;
            }
            record.providerResponseId = std::move(value.responseId);
            const auto maximum = (std::numeric_limits<std::uint64_t>::max)();
            record.inputTokens = record.inputTokens > maximum - value.inputTokens
                ? maximum
                : record.inputTokens + value.inputTokens;
            record.outputTokens = record.outputTokens > maximum - value.outputTokens
                ? maximum
                : record.outputTokens + value.outputTokens;
            record.retainedContextTokens = value.retainedContextTokens;
            record.pendingFunctionCalls = value.functionCalls;
            if (auto saved = store_.save(record, persistenceContext); !saved) {
                record.state = Domain::ManagedRunState::Failed;
                record.lastError = saved.error();
                break;
            }
            publishActive(record);
            if (!awaitDispatchBoundary(
                    request, record, persistenceContext, token)) {
                break;
            }
            if (value.functionCalls.empty()) {
                if (value.outputText.size() > Domain::MaximumManagedRunOutputBytes) {
                    value.outputText.resize(Domain::MaximumManagedRunOutputBytes);
                }
                record.outputText = std::move(value.outputText);
                record.pendingFunctionCalls.clear();
                record.state = token.stop_requested()
                    ? Domain::ManagedRunState::Cancelled
                    : Domain::ManagedRunState::Completed;
                break;
            }
            if (!authority || !tools_.router) {
                record.lastError = failure(
                    Domain::ErrorCodes::HostCapabilityUnavailable,
                    "The provider requested a tool, but managed native tools are unavailable.");
                record.state = Domain::ManagedRunState::Failed;
                break;
            }

            toolOutputs.clear();
            toolOutputs.reserve(value.functionCalls.size());
            for (const auto& call : value.functionCalls) {
                if (!awaitDispatchBoundary(
                        request, record, persistenceContext, token)) {
                    break;
                }
                auto toolRequestId = Domain::RequestId::parse(call.callId, 512U);
                if (!toolRequestId) {
                    record.lastError = failure(
                        Domain::ErrorCodes::MalformedMessage,
                        "The provider function call id is invalid.");
                    record.state = Domain::ManagedRunState::Failed;
                    break;
                }
                Domain::ToolCallRequest toolRequest{
                    Domain::McpRequestMetadata{
                        std::move(toolRequestId).value(),
                        request.correlationId,
                        request.clientId,
                        request.projectId,
                        "managed-run-v1"},
                    call.name,
                    call.canonicalArguments};
                auto invoked = tools_.router->invoke(
                    toolRequest, *authority, providerContext);
                if (invoked) {
                    toolOutputs.push_back(
                        {call.callId, std::move(invoked).value().canonicalPayload});
                } else {
                    nlohmann::json errorResult{
                        {"ok", false},
                        {"error",
                         {{"code", invoked.error().code},
                          {"message", invoked.error().message},
                          {"retryable", invoked.error().retryable}}}};
                    toolOutputs.push_back({call.callId, errorResult.dump()});
                }
                auto summary = "Native tool " + call.name + " result: " +
                    toolOutputs.back().canonicalOutput;
                summary.resize(
                    utf8PrefixByBytes(summary, 2U * 1024U).size());
                if (completedToolWork.size() ==
                    Domain::MaximumContinuityHandoffListItems) {
                    completedToolWork.erase(completedToolWork.begin());
                }
                completedToolWork.push_back(
                    {call.callId, std::move(summary),
                     std::optional<std::string>{"completed"}});
            }
            if (record.state != Domain::ManagedRunState::Running) {
                break;
            }
            record.pendingFunctionCalls.clear();
            if (auto saved = store_.save(record, persistenceContext); !saved) {
                record.state = Domain::ManagedRunState::Failed;
                record.lastError = saved.error();
                break;
            }
            publishActive(record);
            auto continuity = observeContinuity(
                request, record, completedToolWork,
                continuityPredecessorSessionId,
                continuitySequence, providerContext);
            if (!continuity) {
                auto attemptedOperationId =
                    Domain::ContinuityOperationId::parse(
                        continuityIdentity(
                            request.runId, continuitySequence));
                if (attemptedOperationId) {
                    pendingContinuityCheckpoint =
                        std::move(attemptedOperationId).value();
                }
                record.state = Domain::ManagedRunState::Failed;
                record.lastError = std::move(continuity).error();
                break;
            }
            if (continuity.value() &&
                continuity.value()->checkpointPersisted) {
                if (!continuity.value()->operationId) {
                    record.state = Domain::ManagedRunState::Failed;
                    record.lastError = failure(
                        Domain::ErrorCodes::IntegrityFailure,
                        "Continuity persisted a checkpoint without an operation identity.");
                    break;
                }
                pendingContinuityCheckpoint =
                    continuity.value()->operationId;
            }
            if (continuity.value() &&
                continuity.value()->successorActivated) {
                if (!continuity.value()->successorSessionId ||
                    !continuity.value()->successorProviderResponseId) {
                    record.state = Domain::ManagedRunState::Failed;
                    record.lastError = failure(
                        Domain::ErrorCodes::IntegrityFailure,
                        "Continuity activated a successor without complete session and provider response identities.");
                    break;
                }
                if (continuitySequence ==
                    (std::numeric_limits<std::uint64_t>::max)()) {
                    record.state = Domain::ManagedRunState::Failed;
                    record.lastError = failure(
                        Domain::ErrorCodes::LimitExceeded,
                        "The managed run exhausted its continuity identity sequence.");
                    break;
                }
                record.providerResponseId =
                    continuity.value()->successorProviderResponseId;
                continuityPredecessorSessionId =
                    *continuity.value()->successorSessionId;
                ++continuitySequence;
                pendingContinuityCheckpoint.reset();
                toolOutputs.clear();
                input =
                    "Continue the exact original Manager-owned task below from "
                    "the canonical handoff. Treat completed_work as authoritative "
                    "and do not repeat completed tool effects. Do not return a "
                    "status object, progress summary, or handoff acknowledgement. "
                    "Satisfy the original task's requested final-response format "
                    "exactly. Use authorized tools only if remaining work requires "
                    "them; once the original task is satisfied, return a terminal "
                    "response.\n\n[ORIGINAL MANAGER TASK]\n";
                constexpr std::size_t MaximumRepeatedTaskBytes = 8U * 1024U;
                const auto repeatedTask = utf8PrefixByBytes(
                    request.task, MaximumRepeatedTaskBytes);
                input.append(repeatedTask.data(), repeatedTask.size());
                input += "\n[END ORIGINAL MANAGER TASK]\n\n"
                    "[AUTHORITATIVE COMPLETED WORK - OLDEST TO NEWEST]\n";
                constexpr std::size_t MaximumRepeatedCompletedWorkBytes =
                    32U * 1024U;
                std::size_t repeatedCompletedWorkBytes{};
                std::size_t firstRepeatedCompletedWork =
                    completedToolWork.size();
                while (firstRepeatedCompletedWork > 0U) {
                    const auto& candidate =
                        completedToolWork[firstRepeatedCompletedWork - 1U]
                            .summary;
                    const auto addition = candidate.size() + 3U;
                    if (repeatedCompletedWorkBytes + addition >
                        MaximumRepeatedCompletedWorkBytes) {
                        break;
                    }
                    repeatedCompletedWorkBytes += addition;
                    --firstRepeatedCompletedWork;
                }
                for (auto index = firstRepeatedCompletedWork;
                     index < completedToolWork.size(); ++index) {
                    input += "- " + completedToolWork[index].summary + "\n";
                }
                input += "[END AUTHORITATIVE COMPLETED WORK]";
                if (auto saved = store_.save(record, persistenceContext); !saved) {
                    record.state = Domain::ManagedRunState::Failed;
                    record.lastError = saved.error();
                    break;
                }
                publishActive(record);
                continue;
            }
            input.clear();
        }
        if (record.state == Domain::ManagedRunState::Running) {
            record.lastError = failure(
                Domain::ErrorCodes::LimitExceeded,
                "The managed run exceeded the bounded provider tool loop.");
            record.state = Domain::ManagedRunState::Failed;
        }
        if (pendingContinuityCheckpoint && continuity_.automation) {
            auto abandoned = continuity_.automation->abandonCheckpoint(
                request.projectId,
                *pendingContinuityCheckpoint,
                persistenceContext);
            if (!abandoned) {
                record.state = Domain::ManagedRunState::Failed;
                record.lastError = std::move(abandoned).error();
            }
        }
        record.updatedAt = clock_.utcNow();
        if (auto saved = store_.save(record, persistenceContext); !saved) {
            record.state = Domain::ManagedRunState::Failed;
            record.lastError = saved.error();
        }
        try {
            std::lock_guard lock{mutex_};
            const auto found = active_.find(request.runId);
            if (found != active_.end()) {
                found->second->record = std::move(record);
            }
        } catch (...) {
        }
    }

    Contracts::IManagedResponsesTransport& transport_;
    Contracts::IManagedRunStore& store_;
    Contracts::IClock& clock_;
    ManagedRunToolDependencies tools_;
    ManagedRunContinuityDependencies continuity_;
    std::mutex admissionMutex_;
    std::mutex mutex_;
    std::map<Domain::SessionId, std::shared_ptr<ActiveRun>> active_;
    bool shutdown_{};
};

ManagedRunService::ManagedRunService(
    Contracts::IManagedResponsesTransport& transport,
    Contracts::IManagedRunStore& store,
    Contracts::IClock& clock,
    ManagedRunToolDependencies tools,
    ManagedRunContinuityDependencies continuity)
    : implementation_{
          std::make_unique<Impl>(
              transport, store, clock, tools, std::move(continuity))}
{
}

ManagedRunService::~ManagedRunService() noexcept = default;

Domain::Result<std::optional<Domain::ManagedRunSnapshot>>
ManagedRunService::resolveReplay(
    const Domain::ManagedRunStartRequest& request,
    const Domain::OperationContext& context) noexcept
{
    return implementation_->resolveReplay(request, context);
}

Domain::Result<Domain::ManagedRunStartOutcome> ManagedRunService::start(
    const Domain::ManagedRunStartRequest& request,
    const Domain::OperationContext& context) noexcept
{
    return implementation_->start(request, context);
}

Domain::Result<Domain::ManagedRunSnapshot> ManagedRunService::status(
    const Domain::SessionId& runId,
    const Domain::OperationContext& context) noexcept
{
    return implementation_->status(runId, context);
}

Domain::Result<Domain::ManagedRunSnapshot> ManagedRunService::cancel(
    const Domain::SessionId& runId,
    const Domain::OperationContext& context) noexcept
{
    return implementation_->cancel(runId, context);
}

Domain::Result<Domain::ManagedRunSnapshot> ManagedRunService::pause(
    const Domain::SessionId& runId,
    const Domain::OperationContext& context) noexcept
{
    return implementation_->pause(runId, context);
}

Domain::Result<Domain::ManagedRunSnapshot> ManagedRunService::resume(
    const Domain::SessionId& runId,
    const Domain::OperationContext& context) noexcept
{
    return implementation_->resume(runId, context);
}

void ManagedRunService::shutdown() noexcept
{
    implementation_->shutdown();
}

} // namespace ForgeConductor::Application
