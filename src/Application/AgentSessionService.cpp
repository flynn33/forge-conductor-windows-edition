#include "ForgeConductor/Application/AgentSessionService.h"

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

constexpr std::string_view StatusToolName = "agent_run_status";
constexpr std::size_t MaximumAuthorizationBytes = 512U * 1024U;
constexpr std::size_t MaximumAuthorityItems = 256U;
constexpr std::size_t MaximumConsumedAuthorizations = 256U;

enum class CompletionAccess {
    AuthenticatedOwner,
    TrustedP05Compatibility
};

template <typename T, typename U>
[[nodiscard]] Domain::Result<T> propagateFailure(Domain::Result<U>&& source)
{
    return Domain::Result<T>::failure(std::move(source).error());
}

template <typename T>
[[nodiscard]] Domain::Result<T> internalFailure(const std::string_view message)
{
    return Domain::Result<T>::failure(
        Domain::makeError(Domain::ErrorCodes::InternalFailure, std::string{message}));
}

template <typename T>
[[nodiscard]] Domain::Result<T> integrityFailure(const std::string_view message)
{
    return Domain::Result<T>::failure(
        Domain::makeError(Domain::ErrorCodes::IntegrityFailure, std::string{message}));
}

template <typename T>
[[nodiscard]] Domain::Result<T> sessionNotFound(
    const Domain::SessionId& sessionId)
{
    return Domain::Result<T>::failure(Domain::makeError(
        Domain::ErrorCodes::SessionNotFound,
        "Unknown agent session '" + sessionId.value() + "'.",
        true));
}

template <typename T>
[[nodiscard]] Domain::Result<T> ownershipConflict(
    const Domain::SessionId& sessionId,
    const std::string_view message)
{
    return Domain::Result<T>::failure(Domain::makeError(
        Domain::ErrorCodes::OwnershipConflict,
        std::string{message} + " Session: " + sessionId.value() + "."));
}

[[nodiscard]] Domain::Result<void> validateContext(
    const Domain::OperationContext& context,
    const Contracts::IClock& clock) noexcept
{
    try {
        if (context.isCancellationRequested()) {
            return Domain::Result<void>::failure(Domain::makeError(
                Domain::ErrorCodes::Cancelled,
                "The agent-session operation was cancelled before admission."));
        }
        if (context.isExpired(clock.monotonicNow())) {
            return Domain::Result<void>::failure(Domain::makeError(
                Domain::ErrorCodes::DeadlineExceeded,
                "The agent-session operation deadline expired before admission."));
        }
        return Domain::Result<void>::success();
    } catch (...) {
        return Domain::Result<void>::failure(Domain::makeError(
            Domain::ErrorCodes::InternalFailure,
            "The agent-session operation context could not be validated."));
    }
}

[[nodiscard]] bool hasWriteGrant(
    const Contracts::WorkspaceAuthority& authority) noexcept
{
    const auto granted = std::find(
        authority.grants().begin(),
        authority.grants().end(),
        Domain::FileAccess::Write) != authority.grants().end();
    const auto denied = std::find(
        authority.denials().begin(),
        authority.denials().end(),
        Domain::FileAccess::Write) != authority.denials().end();
    return granted && !denied;
}

[[nodiscard]] bool canonicalRequestBindsSession(
    const std::string_view canonicalRequest,
    const Domain::SessionId& sessionId)
{
    // P14 canonicalizes the one-key status argument before authorization.
    // SessionId is already a validated canonical UUID and needs no JSON
    // escaping, so exact capability equality avoids implementing JSON in the
    // Application layer and rejects every extra, duplicate, or decoy member.
    return canonicalRequest ==
        std::string{"{\"session_id\":\""} + sessionId.value() + "\"}";
}

[[nodiscard]] Domain::Result<void> validateStatusAuthorization(
    const Domain::AgentRunStatusRequest& request,
    const Contracts::WorkspaceAuthority& authority,
    const Contracts::AuthorizedToolCall& authorization,
    const Domain::OperationContext& context) noexcept
{
    try {
        if (authorization.toolName() != StatusToolName ||
            authorization.effect() != Domain::ToolEffect::Write ||
            authority.intent() != Domain::FileAccess::Write ||
            !hasWriteGrant(authority)) {
            return Domain::Result<void>::failure(Domain::makeError(
                Domain::ErrorCodes::Unauthorized,
                "Agent status ownership transfer requires agent_run_status write authority."));
        }
        if (authorization.clientId() != request.clientId ||
            !authorization.matchesProject(authority.projectId()) ||
            !authorization.matches(authority, context)) {
            return Domain::Result<void>::failure(Domain::makeError(
                Domain::ErrorCodes::Unauthorized,
                "Agent status authorization is not bound to this client, project, or operation."));
        }
        if (authorization.canonicalRequest().size() > MaximumAuthorizationBytes ||
            !canonicalRequestBindsSession(
                authorization.canonicalRequest(), request.sessionId)) {
            return Domain::Result<void>::failure(Domain::makeError(
                Domain::ErrorCodes::Unauthorized,
                "Agent status authorization is not bounded and bound to the requested session."));
        }
        if (authority.trustedRoots().size() + authority.grants().size() +
                authority.denials().size() >
            MaximumAuthorityItems) {
            return Domain::Result<void>::failure(Domain::makeError(
                Domain::ErrorCodes::LimitExceeded,
                "Agent status authority exceeds its capability-item limit."));
        }
        return Domain::Result<void>::success();
    } catch (...) {
        return Domain::Result<void>::failure(Domain::makeError(
            Domain::ErrorCodes::InternalFailure,
            "Agent status authorization could not be validated."));
    }
}

[[nodiscard]] Domain::Result<void> validateTargetAuthority(
    const Domain::AgentRunRecord& run,
    const Contracts::WorkspaceAuthority& authority,
    const Contracts::AuthorizedToolCall& authorization,
    Contracts::IWorkspaceAuthority& workspaceAuthority,
    const Domain::OperationContext& context) noexcept
{
    try {
        if (run.projectId &&
            (run.projectId.value() != authority.projectId() ||
             !authorization.matchesProject(run.projectId.value()))) {
            return Domain::Result<void>::failure(Domain::makeError(
                Domain::ErrorCodes::Unauthorized,
                "Agent status authority does not match the durable run project."));
        }
        // Retained P05 start has no project parameter.  Such legacy rows are
        // compatible only when their durable working directory independently
        // anchors them inside this immutable authority.  A projectless and
        // pathless row has no trustworthy project-scope evidence.
        if (!run.projectId && !run.workingDirectory) {
            return Domain::Result<void>::failure(Domain::makeError(
                Domain::ErrorCodes::Unauthorized,
                "A legacy projectless agent run requires an authorized durable working directory."));
        }
        if (run.workingDirectory) {
            auto authorizedPath = workspaceAuthority.authorize(
                authority,
                Domain::PathAuthorizationRequest{
                    *run.workingDirectory,
                    std::nullopt,
                    Domain::FileAccess::Write,
                    false},
                context);
            if (!authorizedPath) {
                auto error = std::move(authorizedPath).error();
                if (error.code != Domain::ErrorCodes::Unauthorized &&
                    error.code != Domain::ErrorCodes::PathOutsideAuthority) {
                    return Domain::Result<void>::failure(std::move(error));
                }
                return Domain::Result<void>::failure(Domain::makeError(
                    Domain::ErrorCodes::PathOutsideAuthority,
                    "The durable agent working directory does not resolve inside an authorized project root."));
            }
            const auto& path = authorizedPath.value();
            if (path.authorityId() != authority.authorityId() ||
                path.access() != Domain::FileAccess::Write ||
                std::find(
                    authority.trustedRoots().begin(),
                    authority.trustedRoots().end(),
                    path.authorityRoot()) == authority.trustedRoots().end()) {
                return Domain::Result<void>::failure(Domain::makeError(
                    Domain::ErrorCodes::IntegrityFailure,
                    "The workspace authority returned a mismatched durable-path capability."));
            }
        }
        return Domain::Result<void>::success();
    } catch (...) {
        return Domain::Result<void>::failure(Domain::makeError(
            Domain::ErrorCodes::InternalFailure,
            "The durable agent authority scope could not be validated."));
    }
}

void appendJsonString(std::string& output, const std::string_view value)
{
    constexpr char Hex[] = "0123456789abcdef";
    output.push_back('"');
    for (const unsigned char character : value) {
        switch (character) {
        case '"': output += "\\\""; break;
        case '\\': output += "\\\\"; break;
        case '\b': output += "\\b"; break;
        case '\f': output += "\\f"; break;
        case '\n': output += "\\n"; break;
        case '\r': output += "\\r"; break;
        case '\t': output += "\\t"; break;
        default:
            if (character < 0x20U) {
                output += "\\u00";
                output.push_back(Hex[(character >> 4U) & 0x0fU]);
                output.push_back(Hex[character & 0x0fU]);
            } else {
                output.push_back(static_cast<char>(character));
            }
            break;
        }
    }
    output.push_back('"');
}

[[nodiscard]] Domain::AgentCompletionReport legacyReport(
    const std::string_view summary,
    const bool succeeded)
{
    std::string json{"{\"succeeded\":"};
    json += succeeded ? "true" : "false";
    json += ",\"summary\":";
    appendJsonString(json, summary);
    json.push_back('}');
    return Domain::AgentCompletionReport{
        std::move(json),
        {
            {"summary", Domain::AgentReportValueKind::String, summary.size()},
            {"succeeded", Domain::AgentReportValueKind::Boolean, 0U},
        }};
}

[[nodiscard]] bool sameBindingIdentity(
    const Domain::ActiveBinding& binding,
    const Domain::SessionId& sessionId) noexcept
{
    return binding.sessionId == sessionId;
}

[[nodiscard]] bool sameReportFields(
    const std::vector<Domain::AgentReportField>& supplied,
    const std::vector<Domain::AgentReportField>& inspected) noexcept
{
    if (supplied.size() != inspected.size()) {
        return false;
    }
    return std::all_of(
        inspected.begin(),
        inspected.end(),
        [&](const Domain::AgentReportField& derived) {
            const auto match = std::find_if(
                supplied.begin(),
                supplied.end(),
                [&](const Domain::AgentReportField& candidate) {
                    return candidate.key == derived.key;
                });
            return match != supplied.end() &&
                match->kind == derived.kind &&
                match->logicalSize == derived.logicalSize;
        });
}

} // namespace

class AgentSessionService::Impl final {
public:
    Impl(
        Contracts::IAgentCatalog& catalog,
        Contracts::IAgentSessionRepository& repository,
        Contracts::IAgentCompletionReportInspector& reportInspector,
        Contracts::IWorkspaceAuthority& workspaceAuthority,
        Contracts::IClock& clock,
        Contracts::IUuidGenerator& uuidGenerator,
        const std::chrono::seconds idleTtl)
        : catalog_{catalog},
          repository_{repository},
          reportInspector_{reportInspector},
          workspaceAuthority_{workspaceAuthority},
          clock_{clock},
          uuidGenerator_{uuidGenerator},
          idleTtl_{idleTtl}
    {
        if (idleTtl_ <= std::chrono::seconds::zero()) {
            throw std::invalid_argument{"Agent session idle TTL must be positive."};
        }
    }

    ~Impl() noexcept { shutdown(); }

    [[nodiscard]] Domain::Result<Domain::AgentRunStartOutcome> startRun(
        const Domain::AgentRunStartRequest& request,
        const Domain::OperationContext& context) noexcept
    {
        return execute<Domain::AgentRunStartOutcome>(context, [&]() {
            auto valid = Domain::validateAgentRunStartRequest(request);
            if (!valid) {
                return propagateFailure<Domain::AgentRunStartOutcome>(
                    std::move(valid));
            }

            auto specResult = catalog_.get(request.agentId, context);
            if (!specResult) {
                return propagateFailure<Domain::AgentRunStartOutcome>(
                    std::move(specResult));
            }
            if (!specResult.value()) {
                return Domain::Result<Domain::AgentRunStartOutcome>::failure(
                    Domain::makeError(
                        Domain::ErrorCodes::AgentNotFound,
                        "Unknown agent '" + request.agentId.value() + "'.",
                        true));
            }
            auto spec = std::move(specResult).value().value();

            auto uuidResult = uuidGenerator_.next();
            if (!uuidResult) {
                return propagateFailure<Domain::AgentRunStartOutcome>(
                    std::move(uuidResult));
            }
            const auto now = clock_.utcNow();
            Domain::AgentSession session{
                Domain::SessionId{std::move(uuidResult).value()},
                request.agentId,
                request.clientId,
                Domain::SessionStatus::Open,
                std::nullopt,
                now,
                now};
            Domain::AgentRunRecord run{
                session,
                request.projectId,
                request.goal,
                request.workingDirectory,
                spec.outputSchema,
                spec.firstMoves,
                std::nullopt};
            Domain::ActiveBinding binding{
                session.id,
                request.agentId,
                request.goal,
                spec.tools,
                spec.toolsForbidden,
                spec.outputSchema,
                spec.doneDefinition,
                request.workingDirectory};

            valid = Domain::validateAgentRunRecord(run);
            if (!valid) {
                return propagateFailure<Domain::AgentRunStartOutcome>(
                    std::move(valid));
            }
            valid = Domain::validateActiveBinding(binding);
            if (!valid) {
                return propagateFailure<Domain::AgentRunStartOutcome>(
                    std::move(valid));
            }
            valid = validateContext(context, clock_);
            if (!valid) {
                return propagateFailure<Domain::AgentRunStartOutcome>(
                    std::move(valid));
            }

            // Keep the durable commit and its in-memory projection ordered as
            // one service mutation. Otherwise a slower, earlier start could
            // overwrite the cache after a later start has already committed.
            std::unique_lock mutationLock{mutationMutex_};
            auto pruned = pruneStaleCore(context);
            if (!pruned) {
                return propagateFailure<Domain::AgentRunStartOutcome>(
                    std::move(pruned));
            }
            valid = validateContext(context, clock_);
            if (!valid) {
                return propagateFailure<Domain::AgentRunStartOutcome>(
                    std::move(valid));
            }

            const auto supersedeSummary = Domain::makeAgentSupersedeSummary(
                "Closed because a new agent session started",
                request.agentId,
                std::nullopt);
            auto stored = repository_.startRun(
                Domain::AgentRunStartMutation{
                    run,
                    request.clientId
                        ? std::optional<Domain::ActiveBinding>{binding}
                        : std::nullopt,
                    supersedeSummary},
                context);
            if (!stored) {
                return propagateFailure<Domain::AgentRunStartOutcome>(
                    std::move(stored));
            }
            auto persistence = std::move(stored).value();
            if (persistence.run.session.id != session.id ||
                persistence.run.session.agentId != request.agentId ||
                persistence.run.session.clientId != request.clientId ||
                persistence.run.session.status != Domain::SessionStatus::Open ||
                persistence.activeBinding.has_value() != request.clientId.has_value()) {
                return integrityFailure<Domain::AgentRunStartOutcome>(
                    "The repository returned a mismatched committed agent start.");
            }
            if (request.clientId && persistence.activeBinding) {
                setMemoryBinding(*request.clientId, *persistence.activeBinding);
            }
            return Domain::Result<Domain::AgentRunStartOutcome>::success(
                Domain::AgentRunStartOutcome{
                    std::move(persistence.run),
                    std::move(persistence.activeBinding),
                    std::move(spec),
                    persistence.supersededSessions,
                    true});
        });
    }

    [[nodiscard]] Domain::Result<Domain::AgentRunStatusOutcome> runStatus(
        const Domain::AgentRunStatusRequest& request,
        const Contracts::WorkspaceAuthority& authority,
        const Contracts::AuthorizedToolCall& authorization,
        const Domain::OperationContext& context) noexcept
    {
        return execute<Domain::AgentRunStatusOutcome>(context, [&]() {
            auto authorized = validateStatusAuthorization(
                request, authority, authorization, context);
            if (!authorized) {
                return propagateFailure<Domain::AgentRunStatusOutcome>(
                    std::move(authorized));
            }
            std::unique_lock mutationLock{mutationMutex_};
            auto valid = validateContext(context, clock_);
            if (!valid) {
                return propagateFailure<Domain::AgentRunStatusOutcome>(
                    std::move(valid));
            }
            auto found = repository_.getRun(request.sessionId, context);
            if (!found) {
                return propagateFailure<Domain::AgentRunStatusOutcome>(
                    std::move(found));
            }
            if (!found.value()) {
                if (!consumeAuthorization(
                        authorization.requestId(), request.sessionId)) {
                    return Domain::Result<Domain::AgentRunStatusOutcome>::failure(
                        Domain::makeError(
                            Domain::ErrorCodes::Unauthorized,
                            "The agent status authorization was already consumed."));
                }
                Domain::AgentRunStatusOutcome outcome;
                outcome.activeBinding = getMemoryBinding(request.clientId);
                return Domain::Result<Domain::AgentRunStatusOutcome>::success(
                    std::move(outcome));
            }
            auto run = std::move(found).value().value();
            valid = Domain::validateAgentRunRecord(run);
            if (!valid) {
                return propagateFailure<Domain::AgentRunStatusOutcome>(
                    std::move(valid));
            }
            authorized = validateTargetAuthority(
                run, authority, authorization, workspaceAuthority_, context);
            if (!authorized) {
                return propagateFailure<Domain::AgentRunStatusOutcome>(
                    std::move(authorized));
            }
            if (!consumeAuthorization(
                    authorization.requestId(), request.sessionId)) {
                return Domain::Result<Domain::AgentRunStatusOutcome>::failure(
                    Domain::makeError(
                        Domain::ErrorCodes::Unauthorized,
                        "The agent status authorization was already consumed."));
            }

            auto observedUpdatedAt = run.session.updatedAt;
            bool reattached{};
            if (Domain::isOpen(run.session.status)) {
                const auto current = getMemoryBinding(request.clientId);
                if (run.session.clientId != request.clientId || !current ||
                    !sameBindingIdentity(*current, request.sessionId)) {
                    auto attached = attachCore(
                        Domain::AgentRunReattachRequest{
                            request.sessionId, request.clientId},
                        std::move(run),
                        context);
                    if (!attached) {
                        return propagateFailure<Domain::AgentRunStatusOutcome>(
                            std::move(attached));
                    }
                    auto attachedValue = std::move(attached).value();
                    reattached = attachedValue.ownershipChanged;
                    run = std::move(attachedValue.run);
                    // The pinned source refetches after every attach attempt.
                    // Reconstructed bindings therefore reset the idle baseline
                    // even when durable ownership itself did not change.
                    observedUpdatedAt = run.session.updatedAt;
                }
            }

            constexpr std::size_t MaximumTouchAttempts = 2U;
            for (std::size_t attempt{}; attempt < MaximumTouchAttempts; ++attempt) {
                if (!Domain::isOpen(run.session.status)) {
                    removeMemoryBinding(request.clientId, request.sessionId);
                    if (run.session.clientId) {
                        removeMemoryBinding(
                            *run.session.clientId, request.sessionId);
                    }
                    Domain::AgentRunStatusOutcome closedOutcome;
                    closedOutcome.run = std::move(run);
                    closedOutcome.reattached = reattached;
                    return Domain::Result<Domain::AgentRunStatusOutcome>::success(
                        std::move(closedOutcome));
                }

                const auto now = clock_.utcNow();
                const auto touchedAt = (std::max)(now, run.session.updatedAt);
                auto touched = repository_.touchRun(
                    request.sessionId, touchedAt, context);
                if (!touched) {
                    return propagateFailure<Domain::AgentRunStatusOutcome>(
                        std::move(touched));
                }
                if (touched.value()) {
                    Domain::AgentRunStatusOutcome outcome;
                    outcome.run = run;
                    outcome.mustComplete = true;
                    outcome.reattached = reattached;
                    outcome.activeBinding = getMemoryBinding(request.clientId);

                    auto idle = std::chrono::duration_cast<std::chrono::seconds>(
                        now - observedUpdatedAt);
                    if (idle < std::chrono::seconds::zero()) {
                        idle = std::chrono::seconds::zero();
                    }
                    outcome.idleSeconds = idle.count();
                    outcome.abandonRisk =
                        idle > Domain::AgentSessionLimits::AbandonRiskThreshold;
                    return Domain::Result<Domain::AgentRunStatusOutcome>::success(
                        std::move(outcome));
                }

                // A false touch means the open snapshot lost its durable
                // compare point (for example, another process completed the
                // run).  Refetch and retry once; never return the stale open
                // snapshot that preceded the failed touch.
                auto refreshed = repository_.getRun(request.sessionId, context);
                if (!refreshed) {
                    return propagateFailure<Domain::AgentRunStatusOutcome>(
                        std::move(refreshed));
                }
                if (!refreshed.value()) {
                    removeMemoryBinding(request.clientId, request.sessionId);
                    return Domain::Result<Domain::AgentRunStatusOutcome>::success(
                        Domain::AgentRunStatusOutcome{});
                }
                run = std::move(refreshed).value().value();
                valid = Domain::validateAgentRunRecord(run);
                if (!valid) {
                    return propagateFailure<Domain::AgentRunStatusOutcome>(
                        std::move(valid));
                }
                authorized = validateTargetAuthority(
                    run,
                    authority,
                    authorization,
                    workspaceAuthority_,
                    context);
                if (!authorized) {
                    return propagateFailure<Domain::AgentRunStatusOutcome>(
                        std::move(authorized));
                }
                if (Domain::isOpen(run.session.status)) {
                    const auto current = getMemoryBinding(request.clientId);
                    if (run.session.clientId != request.clientId || !current ||
                        !sameBindingIdentity(*current, request.sessionId)) {
                        auto attached = attachCore(
                            Domain::AgentRunReattachRequest{
                                request.sessionId, request.clientId},
                            std::move(run),
                            context);
                        if (!attached) {
                            return propagateFailure<Domain::AgentRunStatusOutcome>(
                                std::move(attached));
                        }
                        auto attachedValue = std::move(attached).value();
                        reattached = reattached ||
                            attachedValue.ownershipChanged;
                        run = std::move(attachedValue.run);
                        observedUpdatedAt = run.session.updatedAt;
                    }
                }
            }

            if (!Domain::isOpen(run.session.status)) {
                removeMemoryBinding(request.clientId, request.sessionId);
                Domain::AgentRunStatusOutcome closedOutcome;
                closedOutcome.run = std::move(run);
                closedOutcome.reattached = reattached;
                return Domain::Result<Domain::AgentRunStatusOutcome>::success(
                    std::move(closedOutcome));
            }
            return ownershipConflict<Domain::AgentRunStatusOutcome>(
                request.sessionId,
                "The durable agent status changed during bounded refresh.");
        });
    }

    [[nodiscard]] Domain::Result<Domain::AgentRunCompleteOutcome> completeRun(
        const Domain::AgentRunCompleteRequest& request,
        const Domain::OperationContext& context) noexcept
    {
        return execute<Domain::AgentRunCompleteOutcome>(context, [&]() {
            return completeRunCore(
                request, CompletionAccess::AuthenticatedOwner, context);
        });
    }

    [[nodiscard]] Domain::Result<Domain::AgentRunCompleteOutcome>
    completeRunCore(
        const Domain::AgentRunCompleteRequest& request,
        const CompletionAccess access,
        const Domain::OperationContext& context)
    {
        auto valid = Domain::validateAgentCompletionReport(request.report);
        if (!valid) {
            return propagateFailure<Domain::AgentRunCompleteOutcome>(
                std::move(valid));
        }
        std::unique_lock mutationLock{mutationMutex_};
        auto found = repository_.getRun(request.sessionId, context);
        if (!found) {
            return propagateFailure<Domain::AgentRunCompleteOutcome>(
                std::move(found));
        }
        if (!found.value()) {
            return sessionNotFound<Domain::AgentRunCompleteOutcome>(
                request.sessionId);
        }
        auto run = std::move(found).value().value();
        const auto observedOwner = run.session.clientId;
        valid = Domain::validateAgentRunRecord(run);
        if (!valid) {
            return propagateFailure<Domain::AgentRunCompleteOutcome>(
                std::move(valid));
        }
        if (!request.clientId) {
            const auto message =
                access == CompletionAccess::TrustedP05Compatibility
                ? "The trusted P05 compatibility path requires a durable owning client."
                : "Agent completion requires an authenticated owning client.";
            return ownershipConflict<Domain::AgentRunCompleteOutcome>(
                request.sessionId, message);
        }
        if (run.session.clientId != request.clientId) {
            return ownershipConflict<Domain::AgentRunCompleteOutcome>(
                request.sessionId,
                "The authenticated client does not own the agent session.");
        }

        auto inspected = reportInspector_.inspect(
            request.report.canonicalJson, context);
        if (!inspected) {
            return propagateFailure<Domain::AgentRunCompleteOutcome>(
                std::move(inspected));
        }
        Domain::AgentCompletionReport verifiedReport{
            request.report.canonicalJson,
            std::move(inspected).value()};
        valid = Domain::validateAgentCompletionReport(verifiedReport);
        if (!valid) {
            return propagateFailure<Domain::AgentRunCompleteOutcome>(
                std::move(valid));
        }
        if (!sameReportFields(request.report.fields, verifiedReport.fields)) {
            return Domain::Result<Domain::AgentRunCompleteOutcome>::failure(
                Domain::makeError(
                    Domain::ErrorCodes::InvalidRequest,
                    "Agent completion field metadata does not match the canonical report object."));
        }

        auto schema = run.outputSchema;
        if (schema.empty()) {
            auto spec = catalog_.get(run.session.agentId, context);
            if (!spec) {
                return propagateFailure<Domain::AgentRunCompleteOutcome>(
                    std::move(spec));
            }
            if (spec.value()) {
                schema = spec.value()->outputSchema;
            }
        }
        valid = validateSchema(schema);
        if (!valid) {
            return propagateFailure<Domain::AgentRunCompleteOutcome>(
                std::move(valid));
        }

        std::vector<std::string> missing;
        for (const auto& key : schema) {
            const auto field = std::find_if(
                verifiedReport.fields.begin(),
                verifiedReport.fields.end(),
                [&](const Domain::AgentReportField& value) {
                    return value.key == key;
                });
            if (field == verifiedReport.fields.end() ||
                Domain::isMissingAgentReportField(*field)) {
                missing.push_back(key);
            }
        }
        auto summary = Domain::makeAgentCompletionSummary(
            run.goal.value_or(""), verifiedReport, missing);
        if (!summary) {
            return propagateFailure<Domain::AgentRunCompleteOutcome>(
                std::move(summary));
        }
        valid = validateContext(context, clock_);
        if (!valid) {
            return propagateFailure<Domain::AgentRunCompleteOutcome>(
                std::move(valid));
        }
        auto completed = repository_.completeRun(
            Domain::AgentRunCompleteMutation{
                request.sessionId,
                request.clientId,
                verifiedReport.canonicalJson,
                std::move(summary).value(),
                missing,
                clock_.utcNow()},
            context);
        if (!completed) {
            return propagateFailure<Domain::AgentRunCompleteOutcome>(
                std::move(completed));
        }
        auto persistence = std::move(completed).value();
        if (persistence.run.session.id != request.sessionId ||
            persistence.run.session.status != Domain::SessionStatus::Closed) {
            return integrityFailure<Domain::AgentRunCompleteOutcome>(
                "The repository returned a mismatched committed agent completion.");
        }
        const auto committedOwner = persistence.run.session.clientId;
        if (committedOwner) {
            removeMemoryBinding(*committedOwner, request.sessionId);
        }
        if (observedOwner && observedOwner != committedOwner) {
            removeMemoryBinding(*observedOwner, request.sessionId);
        }
        return Domain::Result<Domain::AgentRunCompleteOutcome>::success(
            Domain::AgentRunCompleteOutcome{
                std::move(persistence.run),
                std::move(verifiedReport),
                missing.empty(),
                std::move(missing)});
    }

    [[nodiscard]] Domain::Result<Domain::AgentRunReattachOutcome> attach(
        const Domain::AgentRunReattachRequest& request,
        const Contracts::WorkspaceAuthority& authority,
        const Contracts::AuthorizedToolCall& authorization,
        const Domain::OperationContext& context) noexcept
    {
        return execute<Domain::AgentRunReattachOutcome>(context, [&]() {
            auto validRequest = Domain::validateAgentRunReattachRequest(request);
            if (!validRequest) {
                return propagateFailure<Domain::AgentRunReattachOutcome>(
                    std::move(validRequest));
            }
            const auto statusRequest = Domain::AgentRunStatusRequest{
                request.sessionId, request.clientId};
            auto authorized = validateStatusAuthorization(
                statusRequest, authority, authorization, context);
            if (!authorized) {
                return propagateFailure<Domain::AgentRunReattachOutcome>(
                    std::move(authorized));
            }
            std::unique_lock mutationLock{mutationMutex_};
            auto found = repository_.getRun(request.sessionId, context);
            if (!found) {
                return propagateFailure<Domain::AgentRunReattachOutcome>(
                    std::move(found));
            }
            if (!found.value()) {
                return sessionNotFound<Domain::AgentRunReattachOutcome>(
                    request.sessionId);
            }
            auto run = std::move(found).value().value();
            validRequest = Domain::validateAgentRunRecord(run);
            if (!validRequest) {
                return propagateFailure<Domain::AgentRunReattachOutcome>(
                    std::move(validRequest));
            }
            authorized = validateTargetAuthority(
                run, authority, authorization, workspaceAuthority_, context);
            if (!authorized) {
                return propagateFailure<Domain::AgentRunReattachOutcome>(
                    std::move(authorized));
            }
            if (!consumeAuthorization(
                    authorization.requestId(), request.sessionId)) {
                return Domain::Result<Domain::AgentRunReattachOutcome>::failure(
                    Domain::makeError(
                        Domain::ErrorCodes::Unauthorized,
                        "The agent status authorization was already consumed."));
            }
            return attachCore(request, std::move(run), context);
        });
    }

    [[nodiscard]] Domain::Result<Domain::AgentRunRecoveryOutcome> rehydrate(
        const Domain::AgentRunRecoveryRequest& request,
        const Domain::OperationContext& context) noexcept
    {
        return execute<Domain::AgentRunRecoveryOutcome>(context, [&]() {
            auto valid = Domain::validateAgentRunRecoveryRequest(request);
            if (!valid) {
                return propagateFailure<Domain::AgentRunRecoveryOutcome>(
                    std::move(valid));
            }
            std::unique_lock mutationLock{mutationMutex_};
            if (const auto cached = getMemoryBinding(request.clientId)) {
                auto found = repository_.getRun(cached->sessionId, context);
                if (!found) {
                    return propagateFailure<Domain::AgentRunRecoveryOutcome>(
                        std::move(found));
                }
                if (found.value() &&
                    Domain::isOpen(found.value()->session.status) &&
                    found.value()->session.clientId == request.clientId) {
                    return Domain::Result<Domain::AgentRunRecoveryOutcome>::success(
                        Domain::AgentRunRecoveryOutcome{
                            std::move(found).value(),
                            cached,
                            true,
                            false});
                }
                removeMemoryBinding(request.clientId, cached->sessionId);
            }

            auto recovered = repository_.recoverRun(request, context);
            if (!recovered) {
                return propagateFailure<Domain::AgentRunRecoveryOutcome>(
                    std::move(recovered));
            }
            auto outcome = std::move(recovered).value();
            if (!outcome.run) {
                auto latest = repository_.latestOpenRun(request.clientId, context);
                if (!latest) {
                    return propagateFailure<Domain::AgentRunRecoveryOutcome>(
                        std::move(latest));
                }
                outcome.run = std::move(latest).value();
                outcome.projectionNeedsRepair = outcome.run.has_value();
            }
            if (!outcome.run) {
                return Domain::Result<Domain::AgentRunRecoveryOutcome>::success(
                    std::move(outcome));
            }
            if (!Domain::isOpen(outcome.run->session.status) ||
                outcome.run->session.clientId != request.clientId) {
                return integrityFailure<Domain::AgentRunRecoveryOutcome>(
                    "Agent recovery returned a row outside the requested open ownership scope.");
            }
            valid = Domain::validateAgentRunRecord(*outcome.run);
            if (!valid) {
                return propagateFailure<Domain::AgentRunRecoveryOutcome>(
                    std::move(valid));
            }
            if (!outcome.binding && !outcome.run->goal) {
                // Imported v5 rows may legitimately contain a NULL goal.  An
                // empty string is not equivalent evidence, so recovery must
                // expose the durable run without inventing and persisting a
                // binding.  A binding returned by the repository's trusted
                // projection remains usable.
                return Domain::Result<Domain::AgentRunRecoveryOutcome>::success(
                    std::move(outcome));
            }
            if (outcome.run->goal &&
                (!outcome.binding || outcome.projectionNeedsRepair)) {
                // The repository owns durable rows and projections, but only
                // the catalog can supply current tool and completion policy.
                // Rebuild every missing or stale binding here instead of
                // persisting a repository fallback with degraded metadata.
                auto constructed = bindingForRun(*outcome.run, context);
                if (!constructed) {
                    return propagateFailure<Domain::AgentRunRecoveryOutcome>(
                        std::move(constructed));
                }
                outcome.binding = std::move(constructed).value();
                outcome.projectionNeedsRepair = true;
            }
            valid = Domain::validateActiveBinding(*outcome.binding);
            if (!valid || outcome.binding->sessionId != outcome.run->session.id ||
                outcome.binding->agentId != outcome.run->session.agentId) {
                if (!valid) {
                    return propagateFailure<Domain::AgentRunRecoveryOutcome>(
                        std::move(valid));
                }
                return integrityFailure<Domain::AgentRunRecoveryOutcome>(
                    "Agent recovery returned a mismatched active binding.");
            }
            if (outcome.projectionNeedsRepair && outcome.run->goal) {
                auto repair = repository_.repairProjection(
                    Domain::AgentProjectionRepairRequest{
                        request.clientId,
                        *outcome.run,
                        *outcome.binding},
                    context);
                if (!repair) {
                    return propagateFailure<Domain::AgentRunRecoveryOutcome>(
                        std::move(repair));
                }
                outcome.binding = std::move(repair).value().binding;
                outcome.projectionNeedsRepair = false;
            }
            setMemoryBinding(request.clientId, *outcome.binding);
            return Domain::Result<Domain::AgentRunRecoveryOutcome>::success(
                std::move(outcome));
        });
    }

    [[nodiscard]] Domain::Result<std::optional<Domain::ActiveBinding>> binding(
        const Domain::ClientId& clientId,
        const Domain::OperationContext& context) noexcept
    {
        return execute<std::optional<Domain::ActiveBinding>>(context, [&]() {
            return Domain::Result<std::optional<Domain::ActiveBinding>>::success(
                getMemoryBinding(clientId));
        });
    }

    [[nodiscard]] Domain::Result<bool> touchIfActive(
        const Domain::ClientId& clientId,
        const Domain::OperationContext& context) noexcept
    {
        return execute<bool>(context, [&]() {
            std::unique_lock mutationLock{mutationMutex_};
            const auto cached = getMemoryBinding(clientId);
            if (!cached) {
                return Domain::Result<bool>::success(false);
            }
            auto touched = repository_.touchRun(
                cached->sessionId, clock_.utcNow(), context);
            if (!touched) {
                return touched;
            }
            if (!touched.value()) {
                removeMemoryBinding(clientId, cached->sessionId);
            }
            return touched;
        });
    }

    [[nodiscard]] Domain::Result<std::size_t> pruneStale(
        const Domain::OperationContext& context) noexcept
    {
        return execute<std::size_t>(context, [&]() {
            std::unique_lock mutationLock{mutationMutex_};
            return pruneStaleCore(context);
        });
    }

    [[nodiscard]] Domain::Result<Domain::AgentSession> completeLegacy(
        const Domain::SessionId& sessionId,
        const std::string_view summary,
        const bool succeeded,
        const Domain::OperationContext& context) noexcept
    {
        return execute<Domain::AgentSession>(context, [&]() {
            auto found = repository_.getRun(sessionId, context);
            if (!found) {
                return propagateFailure<Domain::AgentSession>(std::move(found));
            }
            if (!found.value()) {
                return sessionNotFound<Domain::AgentSession>(sessionId);
            }
            // This retained P05 method has no caller parameter.  It is the one
            // explicitly trusted compatibility path and is not used by the
            // P14 MCP surface.  It may adopt only a non-null owner observed in
            // durable state; completeRunCore re-reads and compare-and-swaps
            // that same owner so a session identifier alone never reaches the
            // repository's nullable-owner bypass.
            if (!found.value()->session.clientId) {
                return ownershipConflict<Domain::AgentSession>(
                    sessionId,
                    "The trusted P05 compatibility path cannot complete an ownerless session.");
            }
            auto completed = completeRunCore(
                Domain::AgentRunCompleteRequest{
                    sessionId,
                    found.value()->session.clientId,
                    legacyReport(summary, succeeded)},
                CompletionAccess::TrustedP05Compatibility,
                context);
            if (!completed) {
                return propagateFailure<Domain::AgentSession>(
                    std::move(completed));
            }
            return Domain::Result<Domain::AgentSession>::success(
                std::move(completed).value().run.session);
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
            lifecycleChanged_.wait(
                lock, [&]() { return activeOperations_ == 0U; });
            lock.unlock();
            {
                std::lock_guard bindingLock{bindingMutex_};
                memoryBindings_.clear();
                consumedAuthorizations_.clear();
            }
            repository_.close();
            shutdownComplete_.store(true, std::memory_order_release);
            lifecycleChanged_.notify_all();
        } catch (...) {
            if (leader) {
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
            if (!valid) {
                return propagateFailure<Admission>(std::move(valid));
            }
            std::lock_guard lock{lifecycleMutex_};
            valid = validateContext(context, clock_);
            if (!valid) {
                return propagateFailure<Admission>(std::move(valid));
            }
            if (!accepting_) {
                return Domain::Result<Admission>::failure(Domain::makeError(
                    Domain::ErrorCodes::Cancelled,
                    "The agent-session service is shutting down."));
            }
            if (activeOperations_ == (std::numeric_limits<std::size_t>::max)()) {
                return Domain::Result<Admission>::failure(Domain::makeError(
                    Domain::ErrorCodes::LimitExceeded,
                    "The agent-session operation admission counter overflowed."));
            }
            ++activeOperations_;
            return Domain::Result<Admission>::success(Admission{*this});
        } catch (...) {
            return internalFailure<Admission>(
                "The agent-session operation could not be admitted.");
        }
    }

    void releaseOperation() noexcept
    {
        std::lock_guard lock{lifecycleMutex_};
        if (activeOperations_ != 0U) {
            --activeOperations_;
        }
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
            if (!admitted) {
                return propagateFailure<T>(std::move(admitted));
            }
            [[maybe_unused]] auto admission = std::move(admitted).value();
            return std::forward<Function>(operation)();
        } catch (...) {
            return internalFailure<T>(
                "The agent-session application boundary failed internally.");
        }
    }

    [[nodiscard]] Domain::Result<void> validateSchema(
        const std::vector<std::string>& schema) const noexcept
    {
        try {
            if (schema.size() > Domain::AgentSessionLimits::MaximumSchemaItems) {
                return Domain::Result<void>::failure(Domain::makeError(
                    Domain::ErrorCodes::PayloadTooLarge,
                    "Agent output schema exceeds its key-count limit."));
            }
            for (const auto& key : schema) {
                if (key.empty() ||
                    key.size() > Domain::AgentSessionLimits::MaximumItemBytes ||
                    key.find('\0') != std::string::npos) {
                    return Domain::Result<void>::failure(Domain::makeError(
                        Domain::ErrorCodes::InvalidRequest,
                        "Agent output schema contains an invalid key."));
                }
            }
            return Domain::Result<void>::success();
        } catch (...) {
            return Domain::Result<void>::failure(Domain::makeError(
                Domain::ErrorCodes::InternalFailure,
                "Agent output schema could not be validated."));
        }
    }

    [[nodiscard]] Domain::Result<Domain::ActiveBinding> bindingForRun(
        const Domain::AgentRunRecord& run,
        const Domain::OperationContext& context)
    {
        if (!run.goal) {
            return integrityFailure<Domain::ActiveBinding>(
                "A durable agent goal is required to reconstruct an active binding.");
        }
        std::vector<std::string> tools;
        std::vector<std::string> forbidden;
        auto schema = run.outputSchema;
        std::vector<std::string> done;
        auto spec = catalog_.get(run.session.agentId, context);
        if (!spec) {
            return propagateFailure<Domain::ActiveBinding>(std::move(spec));
        }
        if (spec.value()) {
            tools = spec.value()->tools;
            forbidden = spec.value()->toolsForbidden;
            if (schema.empty()) {
                schema = spec.value()->outputSchema;
            }
            done = spec.value()->doneDefinition;
        }
        Domain::ActiveBinding value{
            run.session.id,
            run.session.agentId,
            *run.goal,
            std::move(tools),
            std::move(forbidden),
            std::move(schema),
            std::move(done),
            run.workingDirectory};
        auto valid = Domain::validateActiveBinding(value);
        if (!valid) {
            return propagateFailure<Domain::ActiveBinding>(std::move(valid));
        }
        return Domain::Result<Domain::ActiveBinding>::success(
            std::move(value));
    }

    [[nodiscard]] Domain::Result<Domain::AgentRunReattachOutcome> attachCore(
        const Domain::AgentRunReattachRequest& request,
        Domain::AgentRunRecord run,
        const Domain::OperationContext& context)
    {
        // Callers hold mutationMutex_ across the durable read, authorization,
        // compare-and-swap reattach, and cache projection update.
        if (!Domain::isOpen(run.session.status)) {
            return ownershipConflict<Domain::AgentRunReattachOutcome>(
                request.sessionId,
                "Closed agent sessions cannot be reattached.");
        }
        auto binding = bindingForRun(run, context);
        if (!binding) {
            return propagateFailure<Domain::AgentRunReattachOutcome>(
                std::move(binding));
        }
        const auto previousClient = run.session.clientId;
        auto valid = validateContext(context, clock_);
        if (!valid) {
            return propagateFailure<Domain::AgentRunReattachOutcome>(
                std::move(valid));
        }
        auto attached = repository_.reattachRun(
            Domain::AgentRunReattachMutation{
                request.sessionId,
                previousClient,
                request.clientId,
                std::move(binding).value(),
                Domain::makeAgentSupersedeSummary(
                    "Closed because an existing agent session was reattached",
                    std::nullopt,
                    request.sessionId),
                clock_.utcNow()},
            context);
        if (!attached) {
            return attached;
        }
        auto outcome = std::move(attached).value();
        if (outcome.run.session.id != request.sessionId ||
            outcome.run.session.clientId != request.clientId ||
            !Domain::isOpen(outcome.run.session.status) ||
            outcome.binding.sessionId != request.sessionId) {
            return integrityFailure<Domain::AgentRunReattachOutcome>(
                "The repository returned a mismatched committed agent reattach.");
        }
        if (previousClient && previousClient != request.clientId) {
            removeMemoryBinding(*previousClient, request.sessionId);
        }
        setMemoryBinding(request.clientId, outcome.binding);
        return Domain::Result<Domain::AgentRunReattachOutcome>::success(
            std::move(outcome));
    }

    [[nodiscard]] std::optional<Domain::ActiveBinding> getMemoryBinding(
        const Domain::ClientId& clientId) const
    {
        std::lock_guard lock{bindingMutex_};
        const auto found = memoryBindings_.find(clientId.value());
        return found == memoryBindings_.end()
            ? std::nullopt
            : std::optional<Domain::ActiveBinding>{found->second};
    }

    void setMemoryBinding(
        const Domain::ClientId& clientId,
        const Domain::ActiveBinding& binding)
    {
        std::lock_guard lock{bindingMutex_};
        const auto key = clientId.value();
        if (!memoryBindings_.contains(key) &&
            memoryBindings_.size() >=
                Domain::AgentSessionLimits::MaximumMemoryBindings) {
            memoryBindings_.erase(memoryBindings_.begin());
        }
        memoryBindings_.insert_or_assign(key, binding);
    }

    void removeMemoryBinding(
        const Domain::ClientId& clientId,
        const Domain::SessionId& sessionId)
    {
        std::lock_guard lock{bindingMutex_};
        const auto found = memoryBindings_.find(clientId.value());
        if (found != memoryBindings_.end() &&
            found->second.sessionId == sessionId) {
            memoryBindings_.erase(found);
        }
    }

    [[nodiscard]] bool consumeAuthorization(
        const Domain::RequestId& requestId,
        const Domain::SessionId& sessionId)
    {
        std::lock_guard lock{bindingMutex_};
        if (consumedAuthorizations_.contains(requestId.value())) {
            return false;
        }
        if (consumedAuthorizations_.size() >= MaximumConsumedAuthorizations) {
            consumedAuthorizations_.erase(consumedAuthorizations_.begin());
        }
        consumedAuthorizations_.emplace(requestId.value(), sessionId);
        return true;
    }

    [[nodiscard]] Domain::Result<std::size_t> pruneStaleCore(
        const Domain::OperationContext& context)
    {
        const auto now = clock_.utcNow();
        auto closed = repository_.closeStale(
            Domain::AgentStaleCloseRequest{
                now,
                now - idleTtl_,
                Domain::AgentSessionLimits::MaximumSessionQueryRows},
            context);
        if (!closed) {
            return propagateFailure<std::size_t>(std::move(closed));
        }
        auto outcome = std::move(closed).value();
        if (outcome.closedRuns.size() >
            Domain::AgentSessionLimits::MaximumSessionQueryRows) {
            return Domain::Result<std::size_t>::failure(Domain::makeError(
                Domain::ErrorCodes::LimitExceeded,
                "The stale-session repository returned more rows than requested."));
        }
        for (const auto& run : outcome.closedRuns) {
            if (run.session.clientId) {
                removeMemoryBinding(*run.session.clientId, run.session.id);
            }
        }
        return Domain::Result<std::size_t>::success(
            outcome.closedRuns.size());
    }

    Contracts::IAgentCatalog& catalog_;
    Contracts::IAgentSessionRepository& repository_;
    Contracts::IAgentCompletionReportInspector& reportInspector_;
    Contracts::IWorkspaceAuthority& workspaceAuthority_;
    Contracts::IClock& clock_;
    Contracts::IUuidGenerator& uuidGenerator_;
    const std::chrono::seconds idleTtl_;

    mutable std::mutex bindingMutex_;
    std::map<std::string, Domain::ActiveBinding> memoryBindings_;
    std::map<std::string, Domain::SessionId> consumedAuthorizations_;

    // Serializes repository mutations with their cache projection update.
    // Repository implementations still own cross-process atomicity.
    std::mutex mutationMutex_;

    std::mutex lifecycleMutex_;
    std::condition_variable lifecycleChanged_;
    std::size_t activeOperations_{};
    bool accepting_{true};
    std::atomic<bool> shutdownComplete_{};
};

AgentSessionService::AgentSessionService(
    Contracts::IAgentCatalog& catalog,
    Contracts::IAgentSessionRepository& repository,
    Contracts::IAgentCompletionReportInspector& reportInspector,
    Contracts::IWorkspaceAuthority& workspaceAuthority,
    Contracts::IClock& clock,
    Contracts::IUuidGenerator& uuidGenerator,
    const std::chrono::seconds idleTtl)
    : implementation_{std::make_unique<Impl>(
          catalog,
          repository,
          reportInspector,
          workspaceAuthority,
          clock,
          uuidGenerator,
          idleTtl)}
{
}

AgentSessionService::~AgentSessionService() noexcept = default;

Domain::Result<Domain::AgentSession> AgentSessionService::start(
    const Domain::AgentId& agentId,
    const std::optional<Domain::ClientId>& clientId,
    const std::string_view goal,
    const std::optional<Domain::PathText>& workingDirectory,
    const Domain::OperationContext& context) noexcept
{
    auto result = startRun(
        Domain::AgentRunStartRequest{
            agentId, clientId, std::nullopt, std::string{goal}, workingDirectory},
        context);
    if (!result) {
        return propagateFailure<Domain::AgentSession>(std::move(result));
    }
    return Domain::Result<Domain::AgentSession>::success(
        std::move(result).value().run.session);
}

Domain::Result<Domain::AgentSession> AgentSessionService::status(
    const Domain::SessionId& sessionId,
    const Contracts::WorkspaceAuthority& mutationAuthority,
    const Contracts::AuthorizedToolCall& authorization,
    const Domain::OperationContext& context) noexcept
{
    auto result = runStatus(
        Domain::AgentRunStatusRequest{sessionId, authorization.clientId()},
        mutationAuthority,
        authorization,
        context);
    if (!result) {
        return propagateFailure<Domain::AgentSession>(std::move(result));
    }
    if (!result.value().run) {
        return sessionNotFound<Domain::AgentSession>(sessionId);
    }
    return Domain::Result<Domain::AgentSession>::success(
        std::move(result).value().run->session);
}

Domain::Result<Domain::AgentSession> AgentSessionService::complete(
    const Domain::SessionId& sessionId,
    const std::string_view summary,
    const bool succeeded,
    const Domain::OperationContext& context) noexcept
{
    return implementation_->completeLegacy(
        sessionId, summary, succeeded, context);
}

Domain::Result<std::size_t> AgentSessionService::pruneStale(
    const Domain::OperationContext& context) noexcept
{
    return implementation_->pruneStale(context);
}

Domain::Result<Domain::AgentRunStartOutcome> AgentSessionService::startRun(
    const Domain::AgentRunStartRequest& request,
    const Domain::OperationContext& context) noexcept
{
    return implementation_->startRun(request, context);
}

Domain::Result<Domain::AgentRunStatusOutcome> AgentSessionService::runStatus(
    const Domain::AgentRunStatusRequest& request,
    const Contracts::WorkspaceAuthority& mutationAuthority,
    const Contracts::AuthorizedToolCall& authorization,
    const Domain::OperationContext& context) noexcept
{
    return implementation_->runStatus(
        request, mutationAuthority, authorization, context);
}

Domain::Result<Domain::AgentRunCompleteOutcome> AgentSessionService::completeRun(
    const Domain::AgentRunCompleteRequest& request,
    const Domain::OperationContext& context) noexcept
{
    return implementation_->completeRun(request, context);
}

Domain::Result<Domain::AgentRunReattachOutcome> AgentSessionService::attach(
    const Domain::AgentRunReattachRequest& request,
    const Contracts::WorkspaceAuthority& mutationAuthority,
    const Contracts::AuthorizedToolCall& authorization,
    const Domain::OperationContext& context) noexcept
{
    return implementation_->attach(
        request, mutationAuthority, authorization, context);
}

Domain::Result<Domain::AgentRunRecoveryOutcome> AgentSessionService::rehydrate(
    const Domain::AgentRunRecoveryRequest& request,
    const Domain::OperationContext& context) noexcept
{
    return implementation_->rehydrate(request, context);
}

Domain::Result<std::optional<Domain::ActiveBinding>> AgentSessionService::binding(
    const Domain::ClientId& clientId,
    const Domain::OperationContext& context) noexcept
{
    return implementation_->binding(clientId, context);
}

Domain::Result<bool> AgentSessionService::touchIfActive(
    const Domain::ClientId& clientId,
    const Domain::OperationContext& context) noexcept
{
    return implementation_->touchIfActive(clientId, context);
}

void AgentSessionService::shutdown() noexcept
{
    implementation_->shutdown();
}

} // namespace ForgeConductor::Application
