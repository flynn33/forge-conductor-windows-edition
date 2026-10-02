#include "ForgeConductor/Application/AgentRepositoryManagedRunStore.h"

#include <nlohmann/json.hpp>

#include <chrono>
#include <span>
#include <stdexcept>
#include <utility>

namespace ForgeConductor::Application {
namespace {

[[nodiscard]] Domain::SessionStatus sessionStatus(
    const Domain::ManagedRunState state) noexcept
{
    switch (state) {
    case Domain::ManagedRunState::Running:
    case Domain::ManagedRunState::Cancelling:
    case Domain::ManagedRunState::Paused:
        return Domain::SessionStatus::Running;
    case Domain::ManagedRunState::Completed:
        return Domain::SessionStatus::Completed;
    case Domain::ManagedRunState::Failed:
        return Domain::SessionStatus::Failed;
    case Domain::ManagedRunState::Cancelled:
        return Domain::SessionStatus::Closed;
    }
    return Domain::SessionStatus::Failed;
}

[[nodiscard]] Domain::ManagedRunState managedState(
    const Domain::SessionStatus state) noexcept
{
    switch (state) {
    case Domain::SessionStatus::Completed:
        return Domain::ManagedRunState::Completed;
    case Domain::SessionStatus::Failed:
        return Domain::ManagedRunState::Failed;
    case Domain::SessionStatus::Closed:
        return Domain::ManagedRunState::Cancelled;
    default:
        return Domain::ManagedRunState::Running;
    }
}

[[nodiscard]] bool isTerminal(const Domain::ManagedRunState state) noexcept
{
    return state == Domain::ManagedRunState::Completed ||
        state == Domain::ManagedRunState::Failed ||
        state == Domain::ManagedRunState::Cancelled;
}

[[nodiscard]] Domain::Result<Domain::Sha256Digest> evidenceDigest(
    const Domain::ManagedRunRecord& record,
    const nlohmann::json& persistedSummary,
    Contracts::IHasher& hasher)
{
    auto summaryWithoutSeal = persistedSummary;
    summaryWithoutSeal.erase("evidence_seal_sha256");
    const nlohmann::json envelope{
        {"client_id", record.clientId.value()},
        {"project_id", record.projectId.value()},
        {"run_id", record.runId.value()},
        {"summary", std::move(summaryWithoutSeal)},
        {"task", record.task}};
    const auto encoded = envelope.dump();
    return hasher.sha256(std::as_bytes(std::span<const char>{
        encoded.data(), encoded.size()}));
}

[[nodiscard]] Domain::Result<std::optional<std::string>> summary(
    const Domain::ManagedRunRecord& record,
    Contracts::IHasher& hasher)
{
    const bool cursorPending = record.dispatchPhase ==
        Domain::ManagedRunDispatchPhase::CursorPending;
    if (cursorPending != record.dispatchPending ||
        (cursorPending && record.instructionCursorAdvances.empty()) ||
        (!cursorPending && !record.instructionCursorAdvances.empty())) {
        return Domain::Result<std::optional<std::string>>::failure(
            Domain::makeError(
                Domain::ErrorCodes::IntegrityFailure,
                "The durable managed-run dispatch phase and cursor plan disagree."));
    }
    nlohmann::json value{
        {"allow_tools", record.allowTools},
        {"automatic_continuity", record.automaticContinuity},
        {"authority_generation", record.authorityGeneration},
        {"dispatch_pending", record.dispatchPending},
        {"dispatch_phase", static_cast<std::uint32_t>(record.dispatchPhase)},
        {"input_tokens", record.inputTokens},
        {"kind", "forge_managed_run"},
        {"output_tokens", record.outputTokens},
        {"pending_count", record.pendingFunctionCalls.size()},
        {"state", static_cast<std::uint32_t>(record.state)},
        {"version", 1U}};
    if (record.dispatchOperationId) {
        value["dispatch_operation_id"] =
            record.dispatchOperationId->value();
    } else {
        value["dispatch_operation_id"] = nullptr;
    }
    if (record.dispatchCorrelationId) {
        value["dispatch_correlation_id"] =
            record.dispatchCorrelationId->value();
    } else {
        value["dispatch_correlation_id"] = nullptr;
    }
    if (record.instructionCursorAdvances.size() >
        Domain::MaximumManagedRunCursorAdvances) {
        return Domain::Result<std::optional<std::string>>::failure(
            Domain::makeError(
                Domain::ErrorCodes::LimitExceeded,
                "The durable managed run has too many cursor advances."));
    }
    value["instruction_cursor_advances"] = nlohmann::json::array();
    for (const auto& advance : record.instructionCursorAdvances) {
        value["instruction_cursor_advances"].push_back(nlohmann::json{
            {"completed", advance.completed},
            {"expected_version", advance.expectedVersion},
            {"queue_row_id", advance.queueRowId},
            {"record_id", advance.recordId.value()},
            {"target_entry", advance.targetEntry}});
    }
    if (record.admissionIdentity) {
        value["admission_identity_sha256"] =
            record.admissionIdentity->value();
    } else {
        value["admission_identity_sha256"] = nullptr;
    }
    if (record.providerResponseId) {
        value["provider_response_id"] = record.providerResponseId->value();
    } else {
        value["provider_response_id"] = nullptr;
    }
    if (record.retainedContextTokens) {
        value["retained_context_tokens"] = *record.retainedContextTokens;
    } else {
        value["retained_context_tokens"] = nullptr;
    }
    if (record.outputText) {
        value["output_text"] =
            Domain::truncateAgentSummaryUtf8(*record.outputText, 1'500U);
    } else {
        value["output_text"] = nullptr;
    }
    if (record.lastError) {
        value["error"] = {
            {"code", record.lastError->code},
            {"message", Domain::truncateAgentSummaryUtf8(
                record.lastError->message, 768U)},
            {"retryable", record.lastError->retryable}};
    } else {
        value["error"] = nullptr;
    }
    if (record.nativeTaskChecks.size() > 8U) {
        return Domain::Result<std::optional<std::string>>::failure(
            Domain::makeError(Domain::ErrorCodes::LimitExceeded,
                "The durable run has too many native task checks."));
    }
    value["native_task_checks"] = nlohmann::json::array();
    for (const auto& check : record.nativeTaskChecks) {
        value["native_task_checks"].push_back(nlohmann::json{
            {"command_sha256", check.commandDigest.value()},
            {"stdout_sha256", check.stdoutDigest.value()},
            {"stderr_sha256", check.stderrDigest.value()},
            {"exit_code", check.exitCode},
            {"passed", check.passed},
            {"timed_out", check.timedOut},
            {"cancelled", check.cancelled},
            {"termination_confirmed", check.terminationConfirmed},
            {"elapsed_ms", check.elapsedMilliseconds},
            {"checked_at_utc_ms", std::chrono::duration_cast<
                std::chrono::milliseconds>(check.checkedAt.time_since_epoch()).count()}});
    }
    if (isTerminal(record.state)) {
        auto seal = evidenceDigest(record, value, hasher);
        if (!seal) {
            return Domain::Result<std::optional<std::string>>::failure(
                std::move(seal).error());
        }
        value["evidence_seal_sha256"] = seal.value().value();
    }
    auto encoded = value.dump();
    if (encoded.size() > Domain::AgentSessionLimits::MaximumSummaryUnits) {
        return Domain::Result<std::optional<std::string>>::failure(
            Domain::makeError(
                Domain::ErrorCodes::LimitExceeded,
                "The durable managed-run admission metadata exceeds its bound."));
    }
    return Domain::Result<std::optional<std::string>>::success(
        std::move(encoded));
}

void applySummary(
    const std::optional<std::string>& encoded,
    Domain::ManagedRunRecord& record,
    Contracts::IHasher& hasher)
{
    if (!encoded || !encoded->starts_with('{')) return;
    try {
        const auto value = nlohmann::json::parse(*encoded);
        if (!value.is_object() || value.value("kind", "") !=
                "forge_managed_run" || value.value("version", 0U) != 1U) {
            return;
        }
        const auto sealText = value.contains("evidence_seal_sha256") &&
            value["evidence_seal_sha256"].is_string()
                ? std::optional<std::string>{
                      value["evidence_seal_sha256"].get<std::string>()}
                : std::nullopt;
        const auto persistedDigest = sealText
            ? Domain::Sha256Digest::parse(*sealText)
            : Domain::Result<Domain::Sha256Digest>::failure(
                  Domain::makeError(Domain::ErrorCodes::InvalidRequest,
                      "The run evidence seal is absent."));
        record.authorityGeneration =
            value.value("authority_generation", 0ULL);
        record.allowTools = value.value("allow_tools", true);
        record.automaticContinuity =
            value.value("automatic_continuity", true);
        record.dispatchPending = value.value("dispatch_pending", false);
        const bool hasDispatchPhase = value.contains("dispatch_phase");
        if (hasDispatchPhase) {
            const auto dispatchPhase = value.at("dispatch_phase")
                .get<std::uint32_t>();
            if (dispatchPhase > static_cast<std::uint32_t>(
                    Domain::ManagedRunDispatchPhase::ProviderClaimed)) {
                throw std::runtime_error{
                    "Managed-run dispatch phase is malformed."};
            }
            record.dispatchPhase =
                static_cast<Domain::ManagedRunDispatchPhase>(dispatchPhase);
        }
        if (value.contains("dispatch_operation_id") &&
            value["dispatch_operation_id"].is_string()) {
            auto operationId = Domain::OperationId::parse(
                value["dispatch_operation_id"].get<std::string>());
            if (!operationId) {
                throw std::runtime_error{
                    "Managed-run dispatch operation id is malformed."};
            }
            record.dispatchOperationId = std::move(operationId).value();
        }
        if (value.contains("dispatch_correlation_id") &&
            value["dispatch_correlation_id"].is_string()) {
            auto correlationId = Domain::CorrelationId::parse(
                value["dispatch_correlation_id"].get<std::string>());
            if (!correlationId) {
                throw std::runtime_error{
                    "Managed-run dispatch correlation id is malformed."};
            }
            record.dispatchCorrelationId =
                std::move(correlationId).value();
        }
        if (value.contains("admission_identity_sha256") &&
            value["admission_identity_sha256"].is_string()) {
            auto identity = Domain::Sha256Digest::parse(
                value["admission_identity_sha256"].get<std::string>());
            if (!identity) {
                throw std::runtime_error{
                    "Managed-run admission identity is malformed."};
            }
            record.admissionIdentity = std::move(identity).value();
        }
        if (value.contains("instruction_cursor_advances")) {
            if (!value["instruction_cursor_advances"].is_array() ||
                value["instruction_cursor_advances"].size() >
                    Domain::MaximumManagedRunCursorAdvances) {
                throw std::runtime_error{
                    "Managed-run cursor advances are malformed."};
            }
            for (const auto& item :
                 value["instruction_cursor_advances"]) {
                if (!item.is_object()) {
                    throw std::runtime_error{
                        "Managed-run cursor advance is malformed."};
                }
                auto recordId = Domain::MemoryRecordId::parse(
                    item.at("record_id").get<std::string>());
                if (!recordId) {
                    throw std::runtime_error{
                        "Managed-run cursor record id is malformed."};
                }
                record.instructionCursorAdvances.push_back(
                    Domain::ManagedRunInstructionCursorAdvance{
                        std::move(recordId).value(),
                        item.at("expected_version").get<std::uint32_t>(),
                        item.at("queue_row_id").get<std::string>(),
                        item.at("target_entry").get<std::uint64_t>(),
                        item.at("completed").get<bool>()});
            }
        }
        record.inputTokens = value.value("input_tokens", 0ULL);
        record.outputTokens = value.value("output_tokens", 0ULL);
        if (value.contains("retained_context_tokens") &&
            value["retained_context_tokens"].is_number_unsigned()) {
            record.retainedContextTokens =
                value["retained_context_tokens"].get<std::uint64_t>();
        }
        if (value.contains("provider_response_id") &&
            value["provider_response_id"].is_string()) {
            auto id = Domain::ProviderSessionId::parse(
                value["provider_response_id"].get<std::string>(), 512U);
            if (id) record.providerResponseId = std::move(id).value();
        }
        if (value.contains("output_text") && value["output_text"].is_string()) {
            record.outputText = value["output_text"].get<std::string>();
        }
        if (value.contains("error") && value["error"].is_object()) {
            record.lastError = Domain::makeError(
                value["error"].value("code", std::string{
                    Domain::ErrorCodes::InternalFailure}),
                value["error"].value("message", std::string{
                    "The managed run failed."}),
                value["error"].value("retryable", false));
        }
        const auto state = value.value("state", 0U);
        if (state <= static_cast<std::uint32_t>(
                Domain::ManagedRunState::Paused)) {
            record.state = static_cast<Domain::ManagedRunState>(state);
        }
        if (!hasDispatchPhase) {
            // A legacy pending cursor plan is safe to reconcile. A legacy
            // released Running record is ambiguous and must never cause a
            // provider request to be issued again after restart.
            record.dispatchPhase = record.dispatchPending
                ? Domain::ManagedRunDispatchPhase::CursorPending
                : Domain::ManagedRunDispatchPhase::ProviderClaimed;
        }
        const bool cursorPending = record.dispatchPhase ==
            Domain::ManagedRunDispatchPhase::CursorPending;
        if (cursorPending != record.dispatchPending ||
            (cursorPending && record.instructionCursorAdvances.empty()) ||
            (!cursorPending && !record.instructionCursorAdvances.empty())) {
            throw std::runtime_error{
                "Managed-run dispatch phase and cursor plan disagree."};
        }
        const auto pendingCount = value.value("pending_count", 0U);
        if (pendingCount > 0U ||
            record.state == Domain::ManagedRunState::Cancelling ||
            record.state == Domain::ManagedRunState::Paused) {
            record.state = Domain::ManagedRunState::Failed;
            record.lastError = Domain::makeError(
                Domain::ErrorCodes::Conflict,
                pendingCount > 0U
                    ? "The recovered managed run has pending tool effects and requires review before retry."
                    : "The recovered managed run stopped before a terminal provider result.",
                true);
        }
        if (value.contains("native_task_checks")) {
            if (!value["native_task_checks"].is_array() ||
                value["native_task_checks"].size() > 8U) {
                throw std::runtime_error{"Native task checks are malformed."};
            }
            for (const auto& item : value["native_task_checks"]) {
                if (!item.is_object()) {
                    throw std::runtime_error{"Native task check is malformed."};
                }
                const auto command = Domain::Sha256Digest::parse(
                    item.at("command_sha256").get<std::string>());
                const auto stdoutDigestResult = Domain::Sha256Digest::parse(
                    item.at("stdout_sha256").get<std::string>());
                const auto stderrDigestResult = Domain::Sha256Digest::parse(
                    item.at("stderr_sha256").get<std::string>());
                if (!command || !stdoutDigestResult || !stderrDigestResult) {
                    throw std::runtime_error{"Native task check digest is malformed."};
                }
                record.nativeTaskChecks.push_back(Domain::ManagedNativeTaskCheck{
                    command.value(), stdoutDigestResult.value(),
                    stderrDigestResult.value(),
                    item.at("exit_code").get<int>(),
                    item.at("passed").get<bool>(),
                    item.at("timed_out").get<bool>(),
                    item.at("cancelled").get<bool>(),
                    item.at("termination_confirmed").get<bool>(),
                    item.at("elapsed_ms").get<std::uint64_t>(),
                    Domain::UtcTimePoint{std::chrono::milliseconds{
                        item.at("checked_at_utc_ms").get<std::int64_t>()}}});
            }
        }
        if (isTerminal(record.state)) {
            record.evidenceIntegrity =
                Domain::ManagedRunEvidenceIntegrity::LegacyUnsealed;
            if (sealText) {
                if (persistedDigest) {
                    record.evidenceSeal = persistedDigest.value();
                    auto recomputed = evidenceDigest(record, value, hasher);
                    record.evidenceIntegrity = recomputed &&
                        recomputed.value() == persistedDigest.value()
                            ? Domain::ManagedRunEvidenceIntegrity::Verified
                            : Domain::ManagedRunEvidenceIntegrity::Mismatch;
                } else {
                    record.evidenceIntegrity =
                        Domain::ManagedRunEvidenceIntegrity::Mismatch;
                }
            }
        }
    } catch (...) {
        record.state = Domain::ManagedRunState::Failed;
        record.evidenceIntegrity = Domain::ManagedRunEvidenceIntegrity::Mismatch;
        record.lastError = Domain::makeError(
            Domain::ErrorCodes::IntegrityFailure,
            "The durable managed-run summary is malformed.");
    }
}

} // namespace

class AgentRepositoryManagedRunStore::Impl final {
public:
    Impl(
        Contracts::IAgentSessionRepository& repository,
        Domain::AgentId managedAgentId,
        Contracts::IHasher& hasher)
        : repository_{repository}, managedAgentId_{std::move(managedAgentId)},
          hasher_{hasher}
    {
    }

    [[nodiscard]] Domain::Result<std::optional<Domain::ManagedRunRecord>> load(
        const Domain::SessionId& runId,
        const Domain::OperationContext& context) noexcept
    {
        auto loaded = repository_.getRun(runId, context);
        if (!loaded) {
            return Domain::Result<
                std::optional<Domain::ManagedRunRecord>>::failure(
                std::move(loaded).error());
        }
        if (!loaded.value()) {
            return Domain::Result<
                std::optional<Domain::ManagedRunRecord>>::success(std::nullopt);
        }
        const auto& run = *loaded.value();
        if (!run.projectId || !run.session.clientId ||
            !run.goal || run.session.agentId != managedAgentId_) {
            return Domain::Result<
                std::optional<Domain::ManagedRunRecord>>::failure(
                Domain::makeError(
                    Domain::ErrorCodes::IntegrityFailure,
                    "The durable run is not a complete managed-run record."));
        }
        Domain::ManagedRunRecord record{
            run.session.id,
            *run.projectId,
            *run.session.clientId,
            *run.goal,
            0U,
            managedState(run.session.status),
            std::nullopt,
            0U,
            0U,
            std::nullopt,
            std::nullopt,
            std::nullopt,
            {},
            run.session.createdAt,
            run.session.updatedAt};
        applySummary(run.session.summary, record, hasher_);
        return Domain::Result<
            std::optional<Domain::ManagedRunRecord>>::success(
            std::move(record));
    }

    [[nodiscard]] Domain::Result<void> save(
        const Domain::ManagedRunRecord& record,
        const Domain::OperationContext& context) noexcept
    {
        try {
            auto loaded = repository_.getRun(record.runId, context);
            if (!loaded) {
                return Domain::Result<void>::failure(
                    std::move(loaded).error());
            }
            auto encodedSummary = summary(record, hasher_);
            if (!encodedSummary) {
                return Domain::Result<void>::failure(
                    std::move(encodedSummary).error());
            }
            Domain::AgentSession session{
                record.runId,
                managedAgentId_,
                record.clientId,
                sessionStatus(record.state),
                std::move(encodedSummary).value(),
                record.createdAt,
                record.updatedAt};
            if (!loaded.value()) {
                auto initialSession = session;
                initialSession.status = Domain::SessionStatus::Open;
                initialSession.summary.reset();
                Domain::AgentRunStartMutation mutation{
                    Domain::AgentRunRecord{
                        std::move(initialSession),
                        record.projectId,
                        record.task,
                        std::nullopt,
                        {},
                        {},
                        std::nullopt},
                    Domain::ActiveBinding{
                        record.runId,
                        managedAgentId_,
                        record.task,
                        {},
                        {},
                        {},
                        {},
                        std::nullopt},
                    "Superseded by a newer Manager-owned run.",
                    session.summary};
                auto saved = repository_.startRun(mutation, context);
                if (!saved) {
                    return Domain::Result<void>::failure(
                        std::move(saved).error());
                }
                if (saved.value().run.session.summary != session.summary) {
                    return Domain::Result<void>::failure(
                        Domain::makeError(
                            Domain::ErrorCodes::IntegrityFailure,
                            "The managed-run admission metadata was not inserted atomically."));
                }
                return Domain::Result<void>::success();
            }
            if (loaded.value()->session.agentId != managedAgentId_ ||
                loaded.value()->projectId !=
                    std::optional<Domain::ProjectId>{record.projectId} ||
                loaded.value()->session.clientId !=
                    std::optional<Domain::ClientId>{record.clientId} ||
                loaded.value()->goal !=
                    std::optional<std::string>{record.task}) {
                return Domain::Result<void>::failure(Domain::makeError(
                    Domain::ErrorCodes::Conflict,
                    "The durable run identity does not match this managed run."));
            }
            return repository_.save(session, context);
        } catch (...) {
            return Domain::Result<void>::failure(Domain::makeError(
                Domain::ErrorCodes::InternalFailure,
                "The managed-run record could not be persisted safely."));
        }
    }

private:
    Contracts::IAgentSessionRepository& repository_;
    Domain::AgentId managedAgentId_;
    Contracts::IHasher& hasher_;
};

AgentRepositoryManagedRunStore::AgentRepositoryManagedRunStore(
    Contracts::IAgentSessionRepository& repository,
    Domain::AgentId managedAgentId,
    Contracts::IHasher& hasher)
    : implementation_{std::make_unique<Impl>(
          repository,
          std::move(managedAgentId),
          hasher)}
{
}

AgentRepositoryManagedRunStore::~AgentRepositoryManagedRunStore() noexcept =
    default;

Domain::Result<std::optional<Domain::ManagedRunRecord>>
AgentRepositoryManagedRunStore::load(
    const Domain::SessionId& runId,
    const Domain::OperationContext& context) noexcept
{
    return implementation_->load(runId, context);
}

Domain::Result<void> AgentRepositoryManagedRunStore::save(
    const Domain::ManagedRunRecord& record,
    const Domain::OperationContext& context) noexcept
{
    return implementation_->save(record, context);
}

} // namespace ForgeConductor::Application
