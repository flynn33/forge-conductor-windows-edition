#pragma once

#include "BoundaryFakeSupport.h"
#include "DeterministicResult.h"
#include "ForgeConductor/Contracts/IAgentServices.h"
#include "ForgeConductor/Contracts/IForgeApplicationLifecycle.h"
#include "ForgeConductor/Contracts/IMcpServer.h"
#include "ForgeConductor/Contracts/ISessionHostAdapter.h"

#include <algorithm>
#include <cstddef>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ForgeConductor::Tests::Fakes {

namespace ApplicationServiceFakeDetail {

inline void addSaturating(
    std::size_t& total,
    const std::size_t amount) noexcept
{
    const auto maximum = (std::numeric_limits<std::size_t>::max)();
    total = amount > maximum - total ? maximum : total + amount;
}

[[nodiscard]] inline std::size_t agentSpecItems(
    const Domain::AgentSpec& spec) noexcept
{
    std::size_t count{1};
    addSaturating(count, spec.tools.size());
    addSaturating(count, spec.toolsForbidden.size());
    addSaturating(count, spec.whenToUse.size());
    addSaturating(count, spec.firstMoves.size());
    addSaturating(count, spec.doneDefinition.size());
    addSaturating(count, spec.outputSchema.size());
    addSaturating(count, spec.handoff.size());
    addSaturating(count, spec.qualityBar.size());
    return count;
}

inline void addTextBytes(
    std::size_t& total,
    const std::vector<std::string>& values) noexcept
{
    for (const auto& value : values) {
        addSaturating(total, value.size());
    }
}

[[nodiscard]] inline std::size_t agentSpecTextBytes(
    const Domain::AgentSpec& spec) noexcept
{
    std::size_t bytes{};
    addSaturating(bytes, spec.id.value().size());
    addSaturating(bytes, spec.displayName.size());
    addSaturating(bytes, spec.description.size());
    addSaturating(bytes, spec.body.size());
    addSaturating(bytes, spec.source.size());
    addTextBytes(bytes, spec.tools);
    addTextBytes(bytes, spec.toolsForbidden);
    addTextBytes(bytes, spec.whenToUse);
    addTextBytes(bytes, spec.firstMoves);
    addTextBytes(bytes, spec.doneDefinition);
    addTextBytes(bytes, spec.outputSchema);
    addTextBytes(bytes, spec.handoff);
    addTextBytes(bytes, spec.qualityBar);
    return bytes;
}

[[nodiscard]] inline bool agentSpecWithin(
    const Domain::AgentSpec& spec,
    const std::size_t maximumItems,
    const std::size_t maximumTextBytes) noexcept
{
    return agentSpecItems(spec) <= maximumItems &&
        agentSpecTextBytes(spec) <= maximumTextBytes;
}

[[nodiscard]] inline std::size_t agentSessionTextBytes(
    const Domain::AgentSession& session) noexcept
{
    std::size_t bytes{};
    addSaturating(bytes, session.id.value().size());
    addSaturating(bytes, session.agentId.value().size());
    if (session.clientId) {
        addSaturating(bytes, session.clientId->value().size());
    }
    if (session.summary) {
        addSaturating(bytes, session.summary->size());
    }
    return bytes;
}

[[nodiscard]] inline std::string boundedText(
    const std::string_view value,
    const std::size_t maximumBytes)
{
    return std::string{value.substr(0, (std::min)(value.size(), maximumBytes))};
}

template <typename T>
[[nodiscard]] Domain::Result<T> gateFailure(Domain::Result<void> gate)
{
    return Domain::Result<T>::failure(std::move(gate).error());
}

template <typename T>
[[nodiscard]] Domain::Result<T> internalFailure(const char* const message)
{
    return Domain::Result<T>::failure(
        Domain::makeError(Domain::ErrorCodes::InternalFailure, message));
}

template <typename T>
[[nodiscard]] Domain::Result<T> unauthorized(const char* const message)
{
    return Domain::Result<T>::failure(
        Domain::makeError(Domain::ErrorCodes::Unauthorized, message));
}

[[nodiscard]] inline bool hasMutationGrant(
    const Contracts::WorkspaceAuthority& authority) noexcept
{
    const auto hasWrite = std::find(
        authority.grants().begin(),
        authority.grants().end(),
        Domain::FileAccess::Write) != authority.grants().end();
    const auto deniedWrite = std::find(
        authority.denials().begin(),
        authority.denials().end(),
        Domain::FileAccess::Write) != authority.denials().end();
    return hasWrite && !deniedWrite;
}

[[nodiscard]] inline bool statusCapabilitiesWithin(
    const Contracts::WorkspaceAuthority& authority,
    const Contracts::AuthorizedToolCall& authorization,
    const std::size_t maximumItems,
    const std::size_t maximumTextBytes) noexcept
{
    std::size_t items{};
    addSaturating(items, authority.trustedRoots().size());
    addSaturating(items, authority.grants().size());
    addSaturating(items, authority.denials().size());

    std::size_t bytes{};
    addSaturating(bytes, authority.authorityId().value().size());
    addSaturating(bytes, authority.projectId().value().size());
    addSaturating(bytes, authority.callerId().value().size());
    for (const auto& root : authority.trustedRoots()) {
        addSaturating(bytes, root.value().size());
    }
    addSaturating(bytes, authorization.requestId().value().size());
    addSaturating(bytes, authorization.correlationId().value().size());
    addSaturating(bytes, authorization.clientId().value().size());
    addSaturating(bytes, authorization.toolName().size());
    addSaturating(bytes, authorization.canonicalRequest().size());
    addSaturating(
        bytes,
        authorization.request().metadata.protocolVersion.size());
    return items <= maximumItems && bytes <= maximumTextBytes;
}

} // namespace ApplicationServiceFakeDetail

enum class AgentCatalogCall {
    All,
    Get,
    Recommend
};

struct AgentCatalogCapture final {
    AgentCatalogCall call{AgentCatalogCall::All};
    std::optional<Domain::AgentId> agentId;
    std::string task;
    std::size_t requestedTextBytes{};
};

class RecordingAgentCatalogFake final : public Contracts::IAgentCatalog {
public:
    explicit RecordingAgentCatalogFake(
        const std::size_t outputItemsMaximum =
            DefaultBoundaryCaptureItemsMaximum,
        const std::size_t textBytesMaximum =
            DefaultBoundaryCaptureTextBytesMaximum,
        const Domain::MonotonicTimePoint now = {}) noexcept
        : state_{now},
          outputItemsMaximum_{outputItemsMaximum},
          textBytesMaximum_{textBytesMaximum}
    {
    }

    DeterministicResult<std::vector<Domain::AgentSpec>> allResult;
    DeterministicResult<std::optional<Domain::AgentSpec>> getResult;
    DeterministicResult<Domain::AgentSpec> recommendResult;

    [[nodiscard]] Domain::Result<std::vector<Domain::AgentSpec>> all(
        const Domain::OperationContext& context) noexcept override
    {
        try {
            auto gate = state_.begin(context);
            if (!gate) {
                return ApplicationServiceFakeDetail::gateFailure<
                    std::vector<Domain::AgentSpec>>(std::move(gate));
            }
            lastCapture_.emplace(AgentCatalogCapture{});
            auto result = allResult.get();
            if (result && !specsWithin(result.value())) {
                return Domain::Result<std::vector<Domain::AgentSpec>>::failure(
                    boundaryLimitExceeded(
                        "The scripted agent catalog exceeds its output bound."));
            }
            return result;
        } catch (...) {
            return ApplicationServiceFakeDetail::internalFailure<
                std::vector<Domain::AgentSpec>>(
                    "The deterministic agent catalog could not record all().");
        }
    }

    [[nodiscard]] Domain::Result<std::optional<Domain::AgentSpec>> get(
        const Domain::AgentId& agentId,
        const Domain::OperationContext& context) noexcept override
    {
        try {
            auto gate = state_.begin(context);
            if (!gate) {
                return ApplicationServiceFakeDetail::gateFailure<
                    std::optional<Domain::AgentSpec>>(std::move(gate));
            }
            lastCapture_.emplace(AgentCatalogCapture{
                AgentCatalogCall::Get,
                agentId,
                {},
                0});
            auto result = getResult.get();
            if (result && result.value() &&
                !ApplicationServiceFakeDetail::agentSpecWithin(
                    result.value().value(),
                    outputItemsMaximum_,
                    textBytesMaximum_)) {
                return Domain::Result<std::optional<Domain::AgentSpec>>::failure(
                    boundaryLimitExceeded(
                        "The scripted agent specification exceeds its output bound."));
            }
            return result;
        } catch (...) {
            return ApplicationServiceFakeDetail::internalFailure<
                std::optional<Domain::AgentSpec>>(
                    "The deterministic agent lookup could not be captured.");
        }
    }

    [[nodiscard]] Domain::Result<Domain::AgentSpec> recommend(
        const std::string_view task,
        const Domain::OperationContext& context) noexcept override
    {
        try {
            auto gate = state_.begin(context);
            if (!gate) {
                return ApplicationServiceFakeDetail::gateFailure<
                    Domain::AgentSpec>(std::move(gate));
            }
            lastCapture_.emplace(AgentCatalogCapture{
                AgentCatalogCall::Recommend,
                std::nullopt,
                ApplicationServiceFakeDetail::boundedText(
                    task,
                    textBytesMaximum_),
                task.size()});
            if (task.size() > textBytesMaximum_) {
                return Domain::Result<Domain::AgentSpec>::failure(
                    boundaryPayloadTooLarge(
                        "The agent recommendation task exceeds its capture bound."));
            }
            auto result = recommendResult.get();
            if (result &&
                !ApplicationServiceFakeDetail::agentSpecWithin(
                    result.value(),
                    outputItemsMaximum_,
                    textBytesMaximum_)) {
                return Domain::Result<Domain::AgentSpec>::failure(
                    boundaryLimitExceeded(
                        "The recommended agent exceeds its output bound."));
            }
            return result;
        } catch (...) {
            return ApplicationServiceFakeDetail::internalFailure<Domain::AgentSpec>(
                "The deterministic agent recommendation could not be captured.");
        }
    }

    void setNow(const Domain::MonotonicTimePoint now) noexcept
    {
        state_.setNow(now);
    }

    [[nodiscard]] std::size_t calls() const noexcept { return state_.calls(); }

    [[nodiscard]] const std::optional<Domain::OperationContext>&
    lastContext() const noexcept
    {
        return state_.lastContext();
    }

    [[nodiscard]] const std::optional<AgentCatalogCapture>&
    lastCapture() const noexcept
    {
        return lastCapture_;
    }

private:
    [[nodiscard]] bool specsWithin(
        const std::vector<Domain::AgentSpec>& specs) const noexcept
    {
        if (specs.size() > outputItemsMaximum_) {
            return false;
        }
        std::size_t items{};
        std::size_t bytes{};
        for (const auto& spec : specs) {
            ApplicationServiceFakeDetail::addSaturating(
                items,
                ApplicationServiceFakeDetail::agentSpecItems(spec));
            ApplicationServiceFakeDetail::addSaturating(
                bytes,
                ApplicationServiceFakeDetail::agentSpecTextBytes(spec));
        }
        return items <= outputItemsMaximum_ && bytes <= textBytesMaximum_;
    }

    DeterministicBoundaryState state_;
    std::size_t outputItemsMaximum_;
    std::size_t textBytesMaximum_;
    std::optional<AgentCatalogCapture> lastCapture_;
};

enum class AgentSessionRepositoryCall {
    Save,
    Get,
    List,
    StartRun,
    GetRun,
    ReattachRun,
    CompleteRun,
    TouchRun,
    LatestOpenRun,
    RecoverRun,
    RepairProjection,
    CloseStale,
    QuickCheck,
    Close
};

struct AgentSessionRepositoryCapture final {
    AgentSessionRepositoryCall call{AgentSessionRepositoryCall::Get};
    std::optional<Domain::AgentSession> session;
    std::optional<Domain::SessionId> sessionId;
    std::optional<Domain::AgentId> agentId;
    std::optional<Domain::SessionStatus> status;
    std::size_t maximumCount{};
    std::size_t requestedTextBytes{};
    std::optional<Domain::ClientId> clientId;
    std::optional<Domain::AgentRunStartMutation> startMutation;
    std::optional<Domain::AgentRunReattachMutation> reattachMutation;
    std::optional<Domain::AgentRunCompleteMutation> completeMutation;
    std::optional<Domain::AgentRunRecoveryRequest> recoveryRequest;
    std::optional<Domain::AgentProjectionRepairRequest> projectionRequest;
    std::optional<Domain::AgentStaleCloseRequest> staleRequest;
    std::optional<Domain::UtcTimePoint> timestamp;
};

class RecordingAgentSessionRepositoryFake final
    : public Contracts::IAgentSessionRepository {
public:
    explicit RecordingAgentSessionRepositoryFake(
        const std::size_t outputItemsMaximum =
            DefaultBoundaryCaptureItemsMaximum,
        const std::size_t textBytesMaximum =
            DefaultBoundaryCaptureTextBytesMaximum,
        const Domain::MonotonicTimePoint now = {}) noexcept
        : state_{now},
          outputItemsMaximum_{outputItemsMaximum},
          textBytesMaximum_{textBytesMaximum}
    {
    }

    DeterministicResult<void> saveResult;
    DeterministicResult<std::optional<Domain::AgentSession>> getResult;
    DeterministicResult<std::vector<Domain::AgentSession>> listResult;
    DeterministicResult<Domain::AgentRunStartPersistenceOutcome> startRunResult;
    DeterministicResult<std::optional<Domain::AgentRunRecord>> getRunResult;
    DeterministicResult<Domain::AgentRunReattachOutcome> reattachRunResult;
    DeterministicResult<Domain::AgentRunCompletePersistenceOutcome>
        completeRunResult;
    DeterministicResult<bool> touchRunResult;
    DeterministicResult<std::optional<Domain::AgentRunRecord>> latestOpenRunResult;
    DeterministicResult<Domain::AgentRunRecoveryOutcome> recoverRunResult;
    DeterministicResult<Domain::AgentProjectionRepairOutcome>
        repairProjectionResult;
    DeterministicResult<Domain::AgentStaleCloseOutcome> closeStaleResult;
    DeterministicResult<void> quickCheckResult;

    [[nodiscard]] Domain::Result<void> save(
        const Domain::AgentSession& session,
        const Domain::OperationContext& context) noexcept override
    {
        try {
            auto gate = state_.begin(context);
            if (!gate) {
                return gate;
            }
            const auto bytes =
                ApplicationServiceFakeDetail::agentSessionTextBytes(session);
            const auto bounded = bytes <= textBytesMaximum_;
            lastCapture_.emplace(AgentSessionRepositoryCapture{
                AgentSessionRepositoryCall::Save,
                bounded
                    ? std::optional<Domain::AgentSession>{session}
                    : std::nullopt,
                session.id,
                std::nullopt,
                std::nullopt,
                0,
                bytes});
            if (!bounded) {
                return Domain::Result<void>::failure(boundaryPayloadTooLarge(
                    "The agent session exceeds its capture bound."));
            }
            return saveResult.get();
        } catch (...) {
            return Domain::Result<void>::failure(Domain::makeError(
                Domain::ErrorCodes::InternalFailure,
                "The deterministic agent session save could not be captured."));
        }
    }

    [[nodiscard]] Domain::Result<std::optional<Domain::AgentSession>> get(
        const Domain::SessionId& sessionId,
        const Domain::OperationContext& context) noexcept override
    {
        try {
            auto gate = state_.begin(context);
            if (!gate) {
                return ApplicationServiceFakeDetail::gateFailure<
                    std::optional<Domain::AgentSession>>(std::move(gate));
            }
            lastCapture_.emplace(AgentSessionRepositoryCapture{
                AgentSessionRepositoryCall::Get,
                std::nullopt,
                sessionId,
                std::nullopt,
                std::nullopt,
                0,
                0});
            auto result = getResult.get();
            if (result && result.value() &&
                ApplicationServiceFakeDetail::agentSessionTextBytes(
                    result.value().value()) > textBytesMaximum_) {
                return Domain::Result<std::optional<Domain::AgentSession>>::failure(
                    boundaryPayloadTooLarge(
                        "The scripted agent session exceeds its output bound."));
            }
            return result;
        } catch (...) {
            return ApplicationServiceFakeDetail::internalFailure<
                std::optional<Domain::AgentSession>>(
                    "The deterministic agent session lookup could not be captured.");
        }
    }

    [[nodiscard]] Domain::Result<std::vector<Domain::AgentSession>> list(
        const std::optional<Domain::AgentId>& agentId,
        const std::optional<Domain::SessionStatus>& status,
        const std::size_t maximumCount,
        const Domain::OperationContext& context) noexcept override
    {
        try {
            auto gate = state_.begin(context);
            if (!gate) {
                return ApplicationServiceFakeDetail::gateFailure<
                    std::vector<Domain::AgentSession>>(std::move(gate));
            }
            lastCapture_.emplace(AgentSessionRepositoryCapture{
                AgentSessionRepositoryCall::List,
                std::nullopt,
                std::nullopt,
                agentId,
                status,
                maximumCount,
                0});
            if (maximumCount > outputItemsMaximum_) {
                return Domain::Result<std::vector<Domain::AgentSession>>::failure(
                    boundaryLimitExceeded(
                        "The requested agent session count exceeds its bound."));
            }
            auto result = listResult.get();
            if (result && !sessionsWithin(result.value(), maximumCount)) {
                return Domain::Result<std::vector<Domain::AgentSession>>::failure(
                    boundaryLimitExceeded(
                        "The scripted agent session list exceeds its output bound."));
            }
            return result;
        } catch (...) {
            return ApplicationServiceFakeDetail::internalFailure<
                std::vector<Domain::AgentSession>>(
                    "The deterministic agent session list could not be captured.");
        }
    }

    [[nodiscard]] Domain::Result<Domain::AgentRunStartPersistenceOutcome>
    startRun(
        const Domain::AgentRunStartMutation& mutation,
        const Domain::OperationContext& context) noexcept override
    {
        try {
            auto gate = state_.begin(context);
            if (!gate) {
                return ApplicationServiceFakeDetail::gateFailure<
                    Domain::AgentRunStartPersistenceOutcome>(std::move(gate));
            }
            lastCapture_.emplace();
            lastCapture_->call = AgentSessionRepositoryCall::StartRun;
            lastCapture_->sessionId = mutation.run.session.id;
            const auto validRun = Domain::validateAgentRunRecord(mutation.run);
            const auto validBinding = !mutation.activeBinding ||
                Domain::validateActiveBinding(*mutation.activeBinding).hasValue();
            if (!validRun || !validBinding ||
                mutation.supersedeSummary.size() > textBytesMaximum_) {
                return Domain::Result<Domain::AgentRunStartPersistenceOutcome>::failure(
                    boundaryPayloadTooLarge(
                        "The agent start mutation exceeds its capture bound."));
            }
            lastCapture_->startMutation = mutation;
            return startRunResult.get();
        } catch (...) {
            return ApplicationServiceFakeDetail::internalFailure<
                Domain::AgentRunStartPersistenceOutcome>(
                    "The deterministic agent start transaction could not be captured.");
        }
    }

    [[nodiscard]] Domain::Result<std::optional<Domain::AgentRunRecord>> getRun(
        const Domain::SessionId& sessionId,
        const Domain::OperationContext& context) noexcept override
    {
        try {
            auto gate = state_.begin(context);
            if (!gate) {
                return ApplicationServiceFakeDetail::gateFailure<
                    std::optional<Domain::AgentRunRecord>>(std::move(gate));
            }
            lastCapture_.emplace();
            lastCapture_->call = AgentSessionRepositoryCall::GetRun;
            lastCapture_->sessionId = sessionId;
            return getRunResult.get();
        } catch (...) {
            return ApplicationServiceFakeDetail::internalFailure<
                std::optional<Domain::AgentRunRecord>>(
                    "The deterministic agent run lookup could not be captured.");
        }
    }

    [[nodiscard]] Domain::Result<Domain::AgentRunReattachOutcome> reattachRun(
        const Domain::AgentRunReattachMutation& mutation,
        const Domain::OperationContext& context) noexcept override
    {
        try {
            auto gate = state_.begin(context);
            if (!gate) {
                return ApplicationServiceFakeDetail::gateFailure<
                    Domain::AgentRunReattachOutcome>(std::move(gate));
            }
            lastCapture_.emplace();
            lastCapture_->call = AgentSessionRepositoryCall::ReattachRun;
            lastCapture_->sessionId = mutation.sessionId;
            lastCapture_->clientId = mutation.clientId;
            auto valid = Domain::validateActiveBinding(mutation.binding);
            if (!valid || mutation.supersedeSummary.size() > textBytesMaximum_) {
                return Domain::Result<Domain::AgentRunReattachOutcome>::failure(
                    boundaryPayloadTooLarge(
                        "The agent reattach mutation exceeds its capture bound."));
            }
            lastCapture_->reattachMutation = mutation;
            return reattachRunResult.get();
        } catch (...) {
            return ApplicationServiceFakeDetail::internalFailure<
                Domain::AgentRunReattachOutcome>(
                    "The deterministic agent reattach transaction could not be captured.");
        }
    }

    [[nodiscard]] Domain::Result<Domain::AgentRunCompletePersistenceOutcome>
    completeRun(
        const Domain::AgentRunCompleteMutation& mutation,
        const Domain::OperationContext& context) noexcept override
    {
        try {
            auto gate = state_.begin(context);
            if (!gate) {
                return ApplicationServiceFakeDetail::gateFailure<
                    Domain::AgentRunCompletePersistenceOutcome>(std::move(gate));
            }
            lastCapture_.emplace();
            lastCapture_->call = AgentSessionRepositoryCall::CompleteRun;
            lastCapture_->sessionId = mutation.sessionId;
            if (mutation.reportJson.size() > textBytesMaximum_ ||
                mutation.summary.size() > textBytesMaximum_ ||
                mutation.missingSchemaKeys.size() > outputItemsMaximum_) {
                return Domain::Result<
                    Domain::AgentRunCompletePersistenceOutcome>::failure(
                        boundaryPayloadTooLarge(
                            "The agent completion mutation exceeds its capture bound."));
            }
            lastCapture_->completeMutation = mutation;
            return completeRunResult.get();
        } catch (...) {
            return ApplicationServiceFakeDetail::internalFailure<
                Domain::AgentRunCompletePersistenceOutcome>(
                    "The deterministic agent completion transaction could not be captured.");
        }
    }

    [[nodiscard]] Domain::Result<bool> touchRun(
        const Domain::SessionId& sessionId,
        const Domain::UtcTimePoint touchedAt,
        const Domain::OperationContext& context) noexcept override
    {
        try {
            auto gate = state_.begin(context);
            if (!gate) {
                return ApplicationServiceFakeDetail::gateFailure<bool>(
                    std::move(gate));
            }
            lastCapture_.emplace();
            lastCapture_->call = AgentSessionRepositoryCall::TouchRun;
            lastCapture_->sessionId = sessionId;
            lastCapture_->timestamp = touchedAt;
            return touchRunResult.get();
        } catch (...) {
            return ApplicationServiceFakeDetail::internalFailure<bool>(
                "The deterministic agent touch could not be captured.");
        }
    }

    [[nodiscard]] Domain::Result<std::optional<Domain::AgentRunRecord>>
    latestOpenRun(
        const Domain::ClientId& clientId,
        const Domain::OperationContext& context) noexcept override
    {
        try {
            auto gate = state_.begin(context);
            if (!gate) {
                return ApplicationServiceFakeDetail::gateFailure<
                    std::optional<Domain::AgentRunRecord>>(std::move(gate));
            }
            lastCapture_.emplace();
            lastCapture_->call = AgentSessionRepositoryCall::LatestOpenRun;
            lastCapture_->clientId = clientId;
            return latestOpenRunResult.get();
        } catch (...) {
            return ApplicationServiceFakeDetail::internalFailure<
                std::optional<Domain::AgentRunRecord>>(
                    "The deterministic latest agent run could not be captured.");
        }
    }

    [[nodiscard]] Domain::Result<Domain::AgentRunRecoveryOutcome> recoverRun(
        const Domain::AgentRunRecoveryRequest& request,
        const Domain::OperationContext& context) noexcept override
    {
        try {
            auto gate = state_.begin(context);
            if (!gate) {
                return ApplicationServiceFakeDetail::gateFailure<
                    Domain::AgentRunRecoveryOutcome>(std::move(gate));
            }
            lastCapture_.emplace();
            lastCapture_->call = AgentSessionRepositoryCall::RecoverRun;
            lastCapture_->clientId = request.clientId;
            lastCapture_->recoveryRequest = request;
            return recoverRunResult.get();
        } catch (...) {
            return ApplicationServiceFakeDetail::internalFailure<
                Domain::AgentRunRecoveryOutcome>(
                    "The deterministic agent recovery could not be captured.");
        }
    }

    [[nodiscard]] Domain::Result<Domain::AgentProjectionRepairOutcome>
    repairProjection(
        const Domain::AgentProjectionRepairRequest& request,
        const Domain::OperationContext& context) noexcept override
    {
        try {
            auto gate = state_.begin(context);
            if (!gate) {
                return ApplicationServiceFakeDetail::gateFailure<
                    Domain::AgentProjectionRepairOutcome>(std::move(gate));
            }
            lastCapture_.emplace();
            lastCapture_->call = AgentSessionRepositoryCall::RepairProjection;
            lastCapture_->clientId = request.clientId;
            auto valid = Domain::validateActiveBinding(request.binding);
            if (!valid) {
                return ApplicationServiceFakeDetail::gateFailure<
                    Domain::AgentProjectionRepairOutcome>(std::move(valid));
            }
            lastCapture_->projectionRequest = request;
            return repairProjectionResult.get();
        } catch (...) {
            return ApplicationServiceFakeDetail::internalFailure<
                Domain::AgentProjectionRepairOutcome>(
                    "The deterministic projection repair could not be captured.");
        }
    }

    [[nodiscard]] Domain::Result<Domain::AgentStaleCloseOutcome> closeStale(
        const Domain::AgentStaleCloseRequest& request,
        const Domain::OperationContext& context) noexcept override
    {
        try {
            auto gate = state_.begin(context);
            if (!gate) {
                return ApplicationServiceFakeDetail::gateFailure<
                    Domain::AgentStaleCloseOutcome>(std::move(gate));
            }
            lastCapture_.emplace();
            lastCapture_->call = AgentSessionRepositoryCall::CloseStale;
            lastCapture_->maximumCount = request.maximumCount;
            lastCapture_->staleRequest = request;
            if (request.maximumCount > outputItemsMaximum_) {
                return Domain::Result<Domain::AgentStaleCloseOutcome>::failure(
                    boundaryLimitExceeded(
                        "The stale close count exceeds its capture bound."));
            }
            auto result = closeStaleResult.get();
            if (result && result.value().closedRuns.size() > request.maximumCount) {
                return Domain::Result<Domain::AgentStaleCloseOutcome>::failure(
                    boundaryLimitExceeded(
                        "The scripted stale close exceeds its requested bound."));
            }
            return result;
        } catch (...) {
            return ApplicationServiceFakeDetail::internalFailure<
                Domain::AgentStaleCloseOutcome>(
                    "The deterministic stale close could not be captured.");
        }
    }

    [[nodiscard]] Domain::Result<void> quickCheck(
        const Domain::OperationContext& context) noexcept override
    {
        try {
            auto gate = state_.begin(context);
            if (!gate) {
                return gate;
            }
            lastCapture_.emplace();
            lastCapture_->call = AgentSessionRepositoryCall::QuickCheck;
            return quickCheckResult.get();
        } catch (...) {
            return Domain::Result<void>::failure(Domain::makeError(
                Domain::ErrorCodes::InternalFailure,
                "The deterministic agent repository check could not be captured."));
        }
    }

    void close() noexcept override
    {
        lastCapture_.emplace();
        lastCapture_->call = AgentSessionRepositoryCall::Close;
        state_.shutdown();
    }

    void setNow(const Domain::MonotonicTimePoint now) noexcept
    {
        state_.setNow(now);
    }

    [[nodiscard]] std::size_t calls() const noexcept { return state_.calls(); }

    [[nodiscard]] const std::optional<Domain::OperationContext>&
    lastContext() const noexcept
    {
        return state_.lastContext();
    }

    [[nodiscard]] const std::optional<AgentSessionRepositoryCapture>&
    lastCapture() const noexcept
    {
        return lastCapture_;
    }

private:
    [[nodiscard]] bool sessionsWithin(
        const std::vector<Domain::AgentSession>& sessions,
        const std::size_t maximumCount) const noexcept
    {
        if (sessions.size() > maximumCount ||
            sessions.size() > outputItemsMaximum_) {
            return false;
        }
        std::size_t bytes{};
        for (const auto& session : sessions) {
            ApplicationServiceFakeDetail::addSaturating(
                bytes,
                ApplicationServiceFakeDetail::agentSessionTextBytes(session));
        }
        return bytes <= textBytesMaximum_;
    }

    DeterministicBoundaryState state_;
    std::size_t outputItemsMaximum_;
    std::size_t textBytesMaximum_;
    std::optional<AgentSessionRepositoryCapture> lastCapture_;
};

enum class AgentSessionServiceCall {
    Start,
    Status,
    Complete,
    PruneStale,
    StartRun,
    RunStatus,
    CompleteRun,
    Attach,
    Rehydrate,
    Binding,
    TouchIfActive
};

struct AgentSessionServiceCapture final {
    AgentSessionServiceCall call{AgentSessionServiceCall::Start};
    std::optional<Domain::AgentId> agentId;
    std::optional<Domain::ClientId> clientId;
    std::optional<Domain::SessionId> sessionId;
    std::string text;
    std::optional<Domain::PathText> workingDirectory;
    std::optional<Contracts::WorkspaceAuthority> authority;
    std::optional<Contracts::AuthorizedToolCall> authorization;
    std::size_t requestedTextBytes{};
    bool succeeded{};
    std::optional<Domain::AgentRunStartRequest> startRequest;
    std::optional<Domain::AgentRunStatusRequest> statusRequest;
    std::optional<Domain::AgentRunCompleteRequest> completeRequest;
    std::optional<Domain::AgentRunReattachRequest> reattachRequest;
    std::optional<Domain::AgentRunRecoveryRequest> recoveryRequest;
};

class RecordingAgentSessionServiceFake final
    : public Contracts::IAgentSessionService {
public:
    static constexpr std::string_view StatusToolName = "agent_run_status";

    explicit RecordingAgentSessionServiceFake(
        const std::size_t outputItemsMaximum =
            DefaultBoundaryCaptureItemsMaximum,
        const std::size_t textBytesMaximum =
            DefaultBoundaryCaptureTextBytesMaximum,
        const Domain::MonotonicTimePoint now = {}) noexcept
        : state_{now},
          outputItemsMaximum_{outputItemsMaximum},
          textBytesMaximum_{textBytesMaximum}
    {
    }

    DeterministicResult<Domain::AgentSession> startResult;
    DeterministicResult<Domain::AgentSession> statusResult;
    DeterministicResult<Domain::AgentSession> completeResult;
    DeterministicResult<std::size_t> pruneStaleResult;
    DeterministicResult<Domain::AgentRunStartOutcome> startRunResult;
    DeterministicResult<Domain::AgentRunStatusOutcome> runStatusResult;
    DeterministicResult<Domain::AgentRunCompleteOutcome> completeRunResult;
    DeterministicResult<Domain::AgentRunReattachOutcome> attachResult;
    DeterministicResult<Domain::AgentRunRecoveryOutcome> rehydrateResult;
    DeterministicResult<std::optional<Domain::ActiveBinding>> bindingResult;
    DeterministicResult<bool> touchIfActiveResult;

    [[nodiscard]] Domain::Result<Domain::AgentSession> start(
        const Domain::AgentId& agentId,
        const std::optional<Domain::ClientId>& clientId,
        const std::string_view goal,
        const std::optional<Domain::PathText>& workingDirectory,
        const Domain::OperationContext& context) noexcept override
    {
        try {
            auto gate = state_.begin(context);
            if (!gate) {
                return ApplicationServiceFakeDetail::gateFailure<
                    Domain::AgentSession>(std::move(gate));
            }
            std::size_t bytes{goal.size()};
            if (workingDirectory) {
                ApplicationServiceFakeDetail::addSaturating(
                    bytes,
                    workingDirectory->value().size());
            }
            lastCapture_.emplace(AgentSessionServiceCapture{
                AgentSessionServiceCall::Start,
                agentId,
                clientId,
                std::nullopt,
                ApplicationServiceFakeDetail::boundedText(
                    goal,
                    textBytesMaximum_),
                bytes <= textBytesMaximum_ ? workingDirectory : std::nullopt,
                std::nullopt,
                std::nullopt,
                bytes,
                false});
            if (bytes > textBytesMaximum_) {
                return Domain::Result<Domain::AgentSession>::failure(
                    boundaryPayloadTooLarge(
                        "The agent start request exceeds its capture bound."));
            }
            return boundedSession(startResult);
        } catch (...) {
            return ApplicationServiceFakeDetail::internalFailure<
                Domain::AgentSession>(
                    "The deterministic agent start could not be captured.");
        }
    }

    [[nodiscard]] Domain::Result<Domain::AgentSession> status(
        const Domain::SessionId& sessionId,
        const Contracts::WorkspaceAuthority& mutationAuthority,
        const Contracts::AuthorizedToolCall& authorization,
        const Domain::OperationContext& context) noexcept override
    {
        try {
            auto gate = state_.begin(context);
            if (!gate) {
                return ApplicationServiceFakeDetail::gateFailure<
                    Domain::AgentSession>(std::move(gate));
            }
            const auto boundedCapabilities =
                ApplicationServiceFakeDetail::statusCapabilitiesWithin(
                    mutationAuthority,
                    authorization,
                    outputItemsMaximum_,
                    textBytesMaximum_);
            lastCapture_.emplace(AgentSessionServiceCapture{
                AgentSessionServiceCall::Status,
                std::nullopt,
                std::nullopt,
                sessionId,
                {},
                std::nullopt,
                boundedCapabilities
                    ? std::optional<Contracts::WorkspaceAuthority>{
                          mutationAuthority}
                    : std::nullopt,
                boundedCapabilities
                    ? std::optional<Contracts::AuthorizedToolCall>{authorization}
                    : std::nullopt,
                authorization.canonicalRequest().size(),
                false});
            if (authorization.toolName() != StatusToolName) {
                return ApplicationServiceFakeDetail::unauthorized<
                    Domain::AgentSession>(
                        "Agent status requires the agent_run_status tool.");
            }
            if (authorization.effect() != Domain::ToolEffect::Write ||
                mutationAuthority.intent() != Domain::FileAccess::Write ||
                !ApplicationServiceFakeDetail::hasMutationGrant(
                    mutationAuthority)) {
                return ApplicationServiceFakeDetail::unauthorized<
                    Domain::AgentSession>(
                        "Agent status requires matching write authority.");
            }
            if (!authorization.matchesProject(mutationAuthority.projectId()) ||
                !authorization.matches(mutationAuthority, context)) {
                return ApplicationServiceFakeDetail::unauthorized<
                    Domain::AgentSession>(
                        "Agent status authorization binding is mismatched.");
            }
            if (!boundedCapabilities) {
                return Domain::Result<Domain::AgentSession>::failure(
                    boundaryLimitExceeded(
                        "The agent status capabilities exceed their capture bound."));
            }
            if (consumedStatusRequestId_ &&
                consumedStatusRequestId_.value() == authorization.requestId()) {
                return ApplicationServiceFakeDetail::unauthorized<
                    Domain::AgentSession>(
                        "The agent status authorization was already consumed.");
            }
            consumedStatusRequestId_ = authorization.requestId();
            consumedStatusSessionId_ = sessionId;
            return boundedSession(statusResult);
        } catch (...) {
            return ApplicationServiceFakeDetail::internalFailure<
                Domain::AgentSession>(
                    "The deterministic agent status could not be captured.");
        }
    }

    [[nodiscard]] Domain::Result<Domain::AgentSession> complete(
        const Domain::SessionId& sessionId,
        const std::string_view summary,
        const bool succeeded,
        const Domain::OperationContext& context) noexcept override
    {
        try {
            auto gate = state_.begin(context);
            if (!gate) {
                return ApplicationServiceFakeDetail::gateFailure<
                    Domain::AgentSession>(std::move(gate));
            }
            lastCapture_.emplace(AgentSessionServiceCapture{
                AgentSessionServiceCall::Complete,
                std::nullopt,
                std::nullopt,
                sessionId,
                ApplicationServiceFakeDetail::boundedText(
                    summary,
                    textBytesMaximum_),
                std::nullopt,
                std::nullopt,
                std::nullopt,
                summary.size(),
                succeeded});
            if (summary.size() > textBytesMaximum_) {
                return Domain::Result<Domain::AgentSession>::failure(
                    boundaryPayloadTooLarge(
                        "The agent completion summary exceeds its capture bound."));
            }
            return boundedSession(completeResult);
        } catch (...) {
            return ApplicationServiceFakeDetail::internalFailure<
                Domain::AgentSession>(
                    "The deterministic agent completion could not be captured.");
        }
    }

    [[nodiscard]] Domain::Result<std::size_t> pruneStale(
        const Domain::OperationContext& context) noexcept override
    {
        try {
            auto gate = state_.begin(context);
            if (!gate) {
                return ApplicationServiceFakeDetail::gateFailure<std::size_t>(
                    std::move(gate));
            }
            lastCapture_.emplace(AgentSessionServiceCapture{
                AgentSessionServiceCall::PruneStale});
            auto result = pruneStaleResult.get();
            if (result && result.value() > outputItemsMaximum_) {
                return Domain::Result<std::size_t>::failure(
                    boundaryLimitExceeded(
                        "The scripted prune count exceeds its output bound."));
            }
            return result;
        } catch (...) {
            return ApplicationServiceFakeDetail::internalFailure<std::size_t>(
                "The deterministic stale-session prune could not be recorded.");
        }
    }

    [[nodiscard]] Domain::Result<Domain::AgentRunStartOutcome> startRun(
        const Domain::AgentRunStartRequest& request,
        const Domain::OperationContext& context) noexcept override
    {
        try {
            auto gate = state_.begin(context);
            if (!gate) {
                return ApplicationServiceFakeDetail::gateFailure<
                    Domain::AgentRunStartOutcome>(std::move(gate));
            }
            lastCapture_.emplace();
            lastCapture_->call = AgentSessionServiceCall::StartRun;
            lastCapture_->agentId = request.agentId;
            lastCapture_->clientId = request.clientId;
            lastCapture_->text = ApplicationServiceFakeDetail::boundedText(
                request.goal, textBytesMaximum_);
            lastCapture_->workingDirectory = request.workingDirectory;
            lastCapture_->requestedTextBytes = request.goal.size() +
                (request.workingDirectory
                     ? request.workingDirectory->value().size()
                     : 0U);
            auto valid = Domain::validateAgentRunStartRequest(request);
            if (!valid ||
                lastCapture_->requestedTextBytes > textBytesMaximum_) {
                return Domain::Result<Domain::AgentRunStartOutcome>::failure(
                    boundaryPayloadTooLarge(
                        "The typed agent start exceeds its capture bound."));
            }
            lastCapture_->startRequest = request;
            return startRunResult.get();
        } catch (...) {
            return ApplicationServiceFakeDetail::internalFailure<
                Domain::AgentRunStartOutcome>(
                    "The deterministic typed agent start could not be captured.");
        }
    }

    [[nodiscard]] Domain::Result<Domain::AgentRunStatusOutcome> runStatus(
        const Domain::AgentRunStatusRequest& request,
        const Contracts::WorkspaceAuthority& mutationAuthority,
        const Contracts::AuthorizedToolCall& authorization,
        const Domain::OperationContext& context) noexcept override
    {
        try {
            auto gate = state_.begin(context);
            if (!gate) {
                return ApplicationServiceFakeDetail::gateFailure<
                    Domain::AgentRunStatusOutcome>(std::move(gate));
            }
            lastCapture_.emplace();
            lastCapture_->call = AgentSessionServiceCall::RunStatus;
            lastCapture_->sessionId = request.sessionId;
            lastCapture_->clientId = request.clientId;
            lastCapture_->statusRequest = request;
            const auto bounded =
                ApplicationServiceFakeDetail::statusCapabilitiesWithin(
                    mutationAuthority,
                    authorization,
                    outputItemsMaximum_,
                    textBytesMaximum_);
            if (authorization.toolName() != StatusToolName ||
                authorization.effect() != Domain::ToolEffect::Write ||
                mutationAuthority.intent() != Domain::FileAccess::Write ||
                !ApplicationServiceFakeDetail::hasMutationGrant(
                    mutationAuthority) ||
                authorization.clientId() != request.clientId ||
                !authorization.matchesProject(mutationAuthority.projectId()) ||
                !authorization.matches(mutationAuthority, context) ||
                authorization.canonicalRequest().find(request.sessionId.value()) ==
                    std::string::npos) {
                return ApplicationServiceFakeDetail::unauthorized<
                    Domain::AgentRunStatusOutcome>(
                        "Typed agent status authorization is mismatched.");
            }
            if (!bounded) {
                return Domain::Result<Domain::AgentRunStatusOutcome>::failure(
                    boundaryLimitExceeded(
                        "Typed agent status capabilities exceed their capture bound."));
            }
            if (consumedStatusRequestId_ == authorization.requestId()) {
                return ApplicationServiceFakeDetail::unauthorized<
                    Domain::AgentRunStatusOutcome>(
                        "Typed agent status authorization was already consumed.");
            }
            consumedStatusRequestId_ = authorization.requestId();
            consumedStatusSessionId_ = request.sessionId;
            return runStatusResult.get();
        } catch (...) {
            return ApplicationServiceFakeDetail::internalFailure<
                Domain::AgentRunStatusOutcome>(
                    "The deterministic typed agent status could not be captured.");
        }
    }

    [[nodiscard]] Domain::Result<Domain::AgentRunCompleteOutcome> completeRun(
        const Domain::AgentRunCompleteRequest& request,
        const Domain::OperationContext& context) noexcept override
    {
        try {
            auto gate = state_.begin(context);
            if (!gate) {
                return ApplicationServiceFakeDetail::gateFailure<
                    Domain::AgentRunCompleteOutcome>(std::move(gate));
            }
            lastCapture_.emplace();
            lastCapture_->call = AgentSessionServiceCall::CompleteRun;
            lastCapture_->sessionId = request.sessionId;
            lastCapture_->clientId = request.clientId;
            lastCapture_->requestedTextBytes = request.report.canonicalJson.size();
            auto valid = Domain::validateAgentCompletionReport(request.report);
            if (!valid ||
                request.report.canonicalJson.size() > textBytesMaximum_ ||
                request.report.fields.size() > outputItemsMaximum_) {
                return Domain::Result<Domain::AgentRunCompleteOutcome>::failure(
                    boundaryPayloadTooLarge(
                        "The typed agent completion exceeds its capture bound."));
            }
            lastCapture_->completeRequest = request;
            return completeRunResult.get();
        } catch (...) {
            return ApplicationServiceFakeDetail::internalFailure<
                Domain::AgentRunCompleteOutcome>(
                    "The deterministic typed agent completion could not be captured.");
        }
    }

    [[nodiscard]] Domain::Result<Domain::AgentRunReattachOutcome> attach(
        const Domain::AgentRunReattachRequest& request,
        const Contracts::WorkspaceAuthority& mutationAuthority,
        const Contracts::AuthorizedToolCall& authorization,
        const Domain::OperationContext& context) noexcept override
    {
        try {
            auto gate = state_.begin(context);
            if (!gate) {
                return ApplicationServiceFakeDetail::gateFailure<
                    Domain::AgentRunReattachOutcome>(std::move(gate));
            }
            lastCapture_.emplace();
            lastCapture_->call = AgentSessionServiceCall::Attach;
            lastCapture_->sessionId = request.sessionId;
            lastCapture_->clientId = request.clientId;
            lastCapture_->reattachRequest = request;
            if (authorization.toolName() != StatusToolName ||
                authorization.effect() != Domain::ToolEffect::Write ||
                mutationAuthority.intent() != Domain::FileAccess::Write ||
                !ApplicationServiceFakeDetail::hasMutationGrant(
                    mutationAuthority) ||
                authorization.clientId() != request.clientId ||
                !authorization.matchesProject(mutationAuthority.projectId()) ||
                !authorization.matches(mutationAuthority, context) ||
                authorization.canonicalRequest().find(request.sessionId.value()) ==
                    std::string::npos) {
                return ApplicationServiceFakeDetail::unauthorized<
                    Domain::AgentRunReattachOutcome>(
                        "Agent attach authorization is mismatched.");
            }
            if (consumedStatusRequestId_ == authorization.requestId()) {
                return ApplicationServiceFakeDetail::unauthorized<
                    Domain::AgentRunReattachOutcome>(
                        "Agent attach authorization was already consumed.");
            }
            consumedStatusRequestId_ = authorization.requestId();
            consumedStatusSessionId_ = request.sessionId;
            return attachResult.get();
        } catch (...) {
            return ApplicationServiceFakeDetail::internalFailure<
                Domain::AgentRunReattachOutcome>(
                    "The deterministic agent attach could not be captured.");
        }
    }

    [[nodiscard]] Domain::Result<Domain::AgentRunRecoveryOutcome> rehydrate(
        const Domain::AgentRunRecoveryRequest& request,
        const Domain::OperationContext& context) noexcept override
    {
        try {
            auto gate = state_.begin(context);
            if (!gate) {
                return ApplicationServiceFakeDetail::gateFailure<
                    Domain::AgentRunRecoveryOutcome>(std::move(gate));
            }
            lastCapture_.emplace();
            lastCapture_->call = AgentSessionServiceCall::Rehydrate;
            lastCapture_->clientId = request.clientId;
            lastCapture_->recoveryRequest = request;
            return rehydrateResult.get();
        } catch (...) {
            return ApplicationServiceFakeDetail::internalFailure<
                Domain::AgentRunRecoveryOutcome>(
                    "The deterministic agent recovery could not be captured.");
        }
    }

    [[nodiscard]] Domain::Result<std::optional<Domain::ActiveBinding>> binding(
        const Domain::ClientId& clientId,
        const Domain::OperationContext& context) noexcept override
    {
        try {
            auto gate = state_.begin(context);
            if (!gate) {
                return ApplicationServiceFakeDetail::gateFailure<
                    std::optional<Domain::ActiveBinding>>(std::move(gate));
            }
            lastCapture_.emplace();
            lastCapture_->call = AgentSessionServiceCall::Binding;
            lastCapture_->clientId = clientId;
            return bindingResult.get();
        } catch (...) {
            return ApplicationServiceFakeDetail::internalFailure<
                std::optional<Domain::ActiveBinding>>(
                    "The deterministic agent binding lookup could not be captured.");
        }
    }

    [[nodiscard]] Domain::Result<bool> touchIfActive(
        const Domain::ClientId& clientId,
        const Domain::OperationContext& context) noexcept override
    {
        try {
            auto gate = state_.begin(context);
            if (!gate) {
                return ApplicationServiceFakeDetail::gateFailure<bool>(
                    std::move(gate));
            }
            lastCapture_.emplace();
            lastCapture_->call = AgentSessionServiceCall::TouchIfActive;
            lastCapture_->clientId = clientId;
            return touchIfActiveResult.get();
        } catch (...) {
            return ApplicationServiceFakeDetail::internalFailure<bool>(
                "The deterministic active-agent touch could not be captured.");
        }
    }

    void shutdown() noexcept override { state_.shutdown(); }

    void setNow(const Domain::MonotonicTimePoint now) noexcept
    {
        state_.setNow(now);
    }

    [[nodiscard]] bool isShutdown() const noexcept { return state_.isShutdown(); }
    [[nodiscard]] std::size_t calls() const noexcept { return state_.calls(); }

    [[nodiscard]] const std::optional<Domain::OperationContext>&
    lastContext() const noexcept
    {
        return state_.lastContext();
    }

    [[nodiscard]] const std::optional<AgentSessionServiceCapture>&
    lastCapture() const noexcept
    {
        return lastCapture_;
    }

    [[nodiscard]] const std::optional<Domain::RequestId>&
    consumedStatusRequestId() const noexcept
    {
        return consumedStatusRequestId_;
    }

    [[nodiscard]] const std::optional<Domain::SessionId>&
    consumedStatusSessionId() const noexcept
    {
        return consumedStatusSessionId_;
    }

private:
    [[nodiscard]] Domain::Result<Domain::AgentSession> boundedSession(
        const DeterministicResult<Domain::AgentSession>& scripted) const
    {
        auto result = scripted.get();
        if (result &&
            ApplicationServiceFakeDetail::agentSessionTextBytes(result.value()) >
                textBytesMaximum_) {
            return Domain::Result<Domain::AgentSession>::failure(
                boundaryPayloadTooLarge(
                    "The scripted agent session exceeds its output bound."));
        }
        return result;
    }

    DeterministicBoundaryState state_;
    std::size_t outputItemsMaximum_;
    std::size_t textBytesMaximum_;
    std::optional<AgentSessionServiceCapture> lastCapture_;
    std::optional<Domain::RequestId> consumedStatusRequestId_;
    std::optional<Domain::SessionId> consumedStatusSessionId_;
};

enum class LocalModelClientCall {
    Query,
    Cancel
};

struct LocalModelClientCapture final {
    LocalModelClientCall call{LocalModelClientCall::Query};
    Domain::ProviderSessionId sessionId;
};

class RecordingLocalModelClientFake final
    : public Contracts::ILocalModelClient {
public:
    explicit RecordingLocalModelClientFake(
        const Domain::MonotonicTimePoint now = {}) noexcept
        : state_{now}
    {
    }

    DeterministicResult<Domain::HostSessionStatus> queryResult;
    DeterministicResult<void> cancelResult;

    [[nodiscard]] Domain::Result<Domain::HostSessionStatus> query(
        const Domain::ProviderSessionId& sessionId,
        const Domain::OperationContext& context) noexcept override
    {
        try {
            auto gate = state_.begin(context);
            if (!gate) {
                return ApplicationServiceFakeDetail::gateFailure<
                    Domain::HostSessionStatus>(std::move(gate));
            }
            lastCapture_.emplace(
                LocalModelClientCapture{LocalModelClientCall::Query, sessionId});
            return queryResult.get();
        } catch (...) {
            return ApplicationServiceFakeDetail::internalFailure<
                Domain::HostSessionStatus>(
                    "The deterministic local-model query could not be captured.");
        }
    }

    [[nodiscard]] Domain::Result<void> cancel(
        const Domain::ProviderSessionId& sessionId,
        const Domain::OperationContext& context) noexcept override
    {
        try {
            auto gate = state_.begin(context);
            if (!gate) {
                return gate;
            }
            lastCapture_.emplace(
                LocalModelClientCapture{LocalModelClientCall::Cancel, sessionId});
            return cancelResult.get();
        } catch (...) {
            return Domain::Result<void>::failure(Domain::makeError(
                Domain::ErrorCodes::InternalFailure,
                "The deterministic local-model cancellation could not be captured."));
        }
    }

    void shutdown() noexcept override { state_.shutdown(); }

    void setNow(const Domain::MonotonicTimePoint now) noexcept
    {
        state_.setNow(now);
    }

    [[nodiscard]] bool isShutdown() const noexcept { return state_.isShutdown(); }
    [[nodiscard]] std::size_t calls() const noexcept { return state_.calls(); }

    [[nodiscard]] const std::optional<Domain::OperationContext>&
    lastContext() const noexcept
    {
        return state_.lastContext();
    }

    [[nodiscard]] const std::optional<LocalModelClientCapture>&
    lastCapture() const noexcept
    {
        return lastCapture_;
    }

private:
    DeterministicBoundaryState state_;
    std::optional<LocalModelClientCapture> lastCapture_;
};

struct McpServerCapture final {
    Contracts::IMcpTransport* transport{};
    Domain::McpRole role{Domain::McpRole::Primary};
    std::optional<Domain::DeploymentId> deploymentId;
    std::optional<Domain::ClientId> clientId;
};

class RecordingMcpServerFake final : public Contracts::IMcpServer {
public:
    explicit RecordingMcpServerFake(
        const Domain::MonotonicTimePoint now = {}) noexcept
        : state_{now}
    {
    }

    DeterministicResult<void> runResult;

    [[nodiscard]] Domain::Result<void> run(
        Contracts::IMcpTransport& transport,
        const Domain::McpRole role,
        const Domain::DeploymentId& deploymentId,
        const Domain::ClientId& clientId,
        const Domain::OperationContext& context) noexcept override
    {
        try {
            auto gate = state_.begin(context);
            if (!gate) {
                return gate;
            }
            lastCapture_.emplace(McpServerCapture{
                &transport,
                role,
                deploymentId,
                clientId});
            if (cancelledOperation_ &&
                cancelledOperation_.value() == context.operationId) {
                return Domain::Result<void>::failure(Domain::makeError(
                    Domain::ErrorCodes::Cancelled,
                    "The deterministic MCP server run was cancelled."));
            }
            return runResult.get();
        } catch (...) {
            return Domain::Result<void>::failure(Domain::makeError(
                Domain::ErrorCodes::InternalFailure,
                "The deterministic MCP server run could not be captured."));
        }
    }

    void cancel(const Domain::OperationId& operationId) noexcept override
    {
        try {
            cancelledOperation_ = operationId;
        } catch (...) {
        }
    }

    void shutdown() noexcept override { state_.shutdown(); }

    void setNow(const Domain::MonotonicTimePoint now) noexcept
    {
        state_.setNow(now);
    }

    [[nodiscard]] bool isShutdown() const noexcept { return state_.isShutdown(); }
    [[nodiscard]] std::size_t calls() const noexcept { return state_.calls(); }

    [[nodiscard]] const std::optional<Domain::OperationContext>&
    lastContext() const noexcept
    {
        return state_.lastContext();
    }

    [[nodiscard]] const std::optional<Domain::OperationId>&
    cancelledOperation() const noexcept
    {
        return cancelledOperation_;
    }

    [[nodiscard]] const std::optional<McpServerCapture>&
    lastCapture() const noexcept
    {
        return lastCapture_;
    }

private:
    DeterministicBoundaryState state_;
    std::optional<Domain::OperationId> cancelledOperation_;
    std::optional<McpServerCapture> lastCapture_;
};

class RecordingForgeApplicationLifecycleFake final
    : public Contracts::IForgeApplicationLifecycle {
public:
    DeterministicResult<void> startResult;
    DeterministicResult<void> stopResult;

    [[nodiscard]] Domain::Result<void> start() noexcept override
    {
        try {
            ++startCalls_;
            if (stopped_) {
                return Domain::Result<void>::failure(Domain::makeError(
                    Domain::ErrorCodes::Cancelled,
                    "The deterministic application lifecycle is stopped."));
            }
            auto result = startResult.get();
            if (result) {
                started_ = true;
            }
            return result;
        } catch (...) {
            return Domain::Result<void>::failure(Domain::makeError(
                Domain::ErrorCodes::InternalFailure,
                "The deterministic application start could not be recorded."));
        }
    }

    [[nodiscard]] Domain::Result<void> stop() noexcept override
    {
        try {
            ++stopCalls_;
            auto result = stopResult.get();
            if (result) {
                started_ = false;
                stopped_ = true;
            }
            return result;
        } catch (...) {
            return Domain::Result<void>::failure(Domain::makeError(
                Domain::ErrorCodes::InternalFailure,
                "The deterministic application stop could not be recorded."));
        }
    }

    [[nodiscard]] bool isStarted() const noexcept { return started_; }
    [[nodiscard]] bool isStopped() const noexcept { return stopped_; }
    [[nodiscard]] std::size_t startCalls() const noexcept { return startCalls_; }
    [[nodiscard]] std::size_t stopCalls() const noexcept { return stopCalls_; }

private:
    std::size_t startCalls_{};
    std::size_t stopCalls_{};
    bool started_{};
    bool stopped_{};
};

} // namespace ForgeConductor::Tests::Fakes
