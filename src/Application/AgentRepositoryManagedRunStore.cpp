#include "ForgeConductor/Application/AgentRepositoryManagedRunStore.h"

#include <nlohmann/json.hpp>

#include <span>
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
    nlohmann::json value{
        {"allow_tools", record.allowTools},
        {"authority_generation", record.authorityGeneration},
        {"input_tokens", record.inputTokens},
        {"kind", "forge_managed_run"},
        {"output_tokens", record.outputTokens},
        {"pending_count", record.pendingFunctionCalls.size()},
        {"state", static_cast<std::uint32_t>(record.state)},
        {"version", 1U}};
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
    if (isTerminal(record.state)) {
        auto seal = evidenceDigest(record, value, hasher);
        if (!seal) {
            return Domain::Result<std::optional<std::string>>::failure(
                std::move(seal).error());
        }
        value["evidence_seal_sha256"] = seal.value().value();
    }
    return Domain::Result<std::optional<std::string>>::success(value.dump());
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
        const auto pendingCount = value.value("pending_count", 0U);
        if (pendingCount > 0U ||
            record.state == Domain::ManagedRunState::Running ||
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
                    "Superseded by a newer Manager-owned run."};
                auto saved = repository_.startRun(mutation, context);
                if (!saved) {
                    return Domain::Result<void>::failure(
                        std::move(saved).error());
                }
                return repository_.save(session, context);
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
