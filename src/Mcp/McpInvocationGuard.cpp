#include "ForgeConductor/Mcp/McpInvocationGuard.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ForgeConductor::Mcp {
namespace {

using Json = nlohmann::json;
using ToolOutcomeResult = Domain::Result<Domain::ToolCallOutcome>;

constexpr std::size_t MaximumRecentTools = 12U;
constexpr std::size_t MaximumRecentPaths = 16U;
constexpr std::size_t MaximumInferredKeyFiles = 12U;
constexpr std::size_t MaximumNarrativeTools = 8U;
constexpr std::size_t MaximumCanonicalPayloadBytes = 1'048'576U;
constexpr std::uint32_t MaximumConfiguredCount = 1'000'000U;
constexpr std::uint32_t MaximumConfiguredIntervalSeconds = 604'800U;

template <typename T>
[[nodiscard]] Domain::Result<T> failure(
    const std::string_view code,
    const char* const message,
    const bool retryable = false)
{
    return Domain::Result<T>::failure(
        Domain::makeError(code, message, retryable));
}

[[nodiscard]] Domain::Result<void> validatePolicy(
    const McpInvocationGuardPolicy& policy) noexcept
{
    if (policy.softIdenticalCallCount < 2U ||
        policy.hardIdenticalCallCount <= policy.softIdenticalCallCount ||
        policy.hardIdenticalCallCount > MaximumConfiguredCount ||
        policy.checkpointProgressCount == 0U ||
        policy.handoffProgressCount < policy.checkpointProgressCount ||
        policy.handoffProgressCount > MaximumConfiguredCount ||
        policy.checkpointIntervalSeconds == 0U ||
        policy.handoffIntervalSeconds < policy.checkpointIntervalSeconds ||
        policy.handoffIntervalSeconds > MaximumConfiguredIntervalSeconds) {
        return failure<void>(
            Domain::ErrorCodes::InvalidRequest,
            "The MCP invocation-guard policy is invalid.");
    }
    return Domain::Result<void>::success();
}

[[nodiscard]] bool matches(
    const std::string_view candidate,
    const std::span<const std::string_view> values) noexcept
{
    return std::find(values.begin(), values.end(), candidate) != values.end();
}

[[nodiscard]] bool isContinuityTool(const std::string_view tool) noexcept
{
    constexpr std::string_view Values[]{
        "context_get",
        "context_list",
        "session_checkpoint",
        "session_handoff",
        "continuity.acknowledge_handoff",
        "continuity.checkpoint",
        "continuity.get_pending_handoff",
        "continuity.prepare_handoff",
        "continuity.request_rollover",
        "continuity.resume",
        "continuity.status"};
    return matches(tool, Values);
}

[[nodiscard]] bool isResumeTool(const std::string_view tool) noexcept
{
    constexpr std::string_view Values[]{
        "forge_status",
        "context_get",
        "context_list",
        "session_checkpoint",
        "session_handoff",
        "memory_get",
        "memory_list",
        "memory_search",
        "memory_set",
        "memory_delete",
        "agent_list",
        "agent_get",
        "agent_context",
        "agent_recommend",
        "agent_run_status",
        "agent_run_complete"};
    return matches(tool, Values);
}

[[nodiscard]] bool isProgressTool(const std::string_view tool) noexcept
{
    constexpr std::string_view Values[]{
        "fs_read",
        "fs_write",
        "fs_edit",
        "fs_list",
        "fs_glob",
        "fs_mkdir",
        "fs_delete",
        "fs_move",
        "shell_exec",
        "git_status",
        "git_diff",
        "git_log",
        "git_add",
        "git_commit",
        "memory_set",
        "agent_run_start",
        "agent_run_complete",
        "search_text",
        "pdf_write",
        "pdf_from_file"};
    return matches(tool, Values);
}

[[nodiscard]] bool isForcePersistTool(const std::string_view tool) noexcept
{
    return tool == "agent_run_start" || tool == "agent_run_complete";
}

[[nodiscard]] Domain::Result<void> validateContext(
    const Domain::OperationContext& context,
    const Contracts::IClock& clock) noexcept
{
    if (context.isCancellationRequested()) {
        return failure<void>(
            Domain::ErrorCodes::Cancelled,
            "The MCP invocation guard operation was cancelled.");
    }
    if (context.isExpired(clock.monotonicNow())) {
        return failure<void>(
            Domain::ErrorCodes::DeadlineExceeded,
            "The MCP invocation guard operation exceeded its deadline.");
    }
    return Domain::Result<void>::success();
}

[[nodiscard]] Domain::ToolCallOutcome errorOutcome(
    const Domain::ToolCallRequest& request,
    Domain::Error error,
    Json payload)
{
    return Domain::ToolCallOutcome{
        Domain::ToolExecutionReceipt{
            request.metadata.requestId,
            request.toolName,
            false,
            error,
            std::chrono::milliseconds{}},
        payload.dump()};
}

[[nodiscard]] Domain::ToolCallOutcome policyErrorOutcome(
    const Domain::ToolCallRequest& request,
    const std::string_view code,
    std::string message,
    const bool retryable,
    Json additions = Json::object())
{
    Domain::Error error = Domain::makeError(code, std::move(message), retryable);
    Json payload{
        {"code", error.code},
        {"message", error.message},
        {"ok", false},
        {"retryable", error.retryable}};
    for (auto& [key, value] : additions.items()) {
        payload[key] = std::move(value);
    }
    return errorOutcome(request, std::move(error), std::move(payload));
}

[[nodiscard]] ToolOutcomeResult materializeOutcome(
    const Domain::ToolCallRequest& request,
    ToolOutcomeResult outcome)
{
    if (outcome) {
        return outcome;
    }
    auto error = outcome.error();
    Json payload{
        {"code", error.code},
        {"message", error.message},
        {"ok", false},
        {"retryable", error.retryable}};
    return ToolOutcomeResult::success(
        errorOutcome(request, std::move(error), std::move(payload)));
}

[[nodiscard]] ToolOutcomeResult annotate(
    const Domain::ToolCallRequest& request,
    ToolOutcomeResult outcome,
    Json additions)
{
    auto materialized = materializeOutcome(request, std::move(outcome));
    if (!materialized) {
        return materialized;
    }
    try {
        auto value = std::move(materialized).value();
        auto payload = Json::parse(
            value.canonicalPayload, nullptr, false, true);
        if (payload.is_discarded() || !payload.is_object()) {
            return failure<Domain::ToolCallOutcome>(
                Domain::ErrorCodes::IntegrityFailure,
                "The MCP tool outcome is not a JSON object.");
        }
        for (auto& [key, addition] : additions.items()) {
            payload[key] = std::move(addition);
        }
        value.canonicalPayload = payload.dump();
        return ToolOutcomeResult::success(std::move(value));
    } catch (...) {
        return failure<Domain::ToolCallOutcome>(
            Domain::ErrorCodes::InternalFailure,
            "The MCP continuity annotation could not be encoded.");
    }
}

[[nodiscard]] std::filesystem::path filesystemPath(
    const Domain::PathText& value)
{
    std::u8string utf8;
    utf8.reserve(value.value().size());
    for (const auto byte : value.value()) {
        utf8.push_back(static_cast<char8_t>(
            static_cast<unsigned char>(byte)));
    }
    return std::filesystem::path{utf8};
}

[[nodiscard]] std::optional<Domain::PathText> implicitRootFor(
    const Domain::ToolContinuityObservation& observation) noexcept
{
    try {
        const auto& observed = observation.path
            ? observation.path
            : observation.workingDirectory;
        if (!observed) {
            return std::nullopt;
        }

        auto candidate = filesystemPath(*observed);
        std::error_code pathError;
        if (candidate.is_relative()) {
            const Domain::PathText* baseText = nullptr;
            if (observation.path && observation.workingDirectory) {
                baseText = &*observation.workingDirectory;
            } else if (observation.baseDirectory) {
                baseText = &*observation.baseDirectory;
            }
            if (baseText == nullptr) {
                return std::nullopt;
            }
            auto base = filesystemPath(*baseText);
            if (base.is_relative()) {
                if (!observation.baseDirectory ||
                    baseText == &*observation.baseDirectory) {
                    return std::nullopt;
                }
                auto authority = filesystemPath(
                    *observation.baseDirectory);
                if (authority.is_relative()) {
                    return std::nullopt;
                }
                base = authority / base;
            }
            candidate = base / candidate;
        }
        candidate = candidate.lexically_normal();

        const bool observedDirectory = !observation.path;
        if (!observedDirectory &&
            !std::filesystem::is_directory(candidate, pathError)) {
            candidate = candidate.parent_path();
        }
        if (candidate.empty()) {
            return std::nullopt;
        }

        const auto utf8 = candidate.generic_u8string();
        const std::string text{
            reinterpret_cast<const char*>(utf8.data()), utf8.size()};
        auto root = Domain::PathText::create(text);
        if (!root) {
            return std::nullopt;
        }
        return std::move(root).value();
    } catch (...) {
        return std::nullopt;
    }
}

[[nodiscard]] Domain::Result<std::string> resumeSeed(
    const Domain::LegacyHandoffPacket& packet)
{
    if (!packet.resumeSeed.empty()) {
        return Domain::Result<std::string>::success(packet.resumeSeed);
    }
    return Domain::makeLegacyDefaultResumeSeed(packet);
}

struct HandoffReceipt final {
    std::string id;
    std::string resumeSeed;
};

[[nodiscard]] Domain::Result<HandoffReceipt> validatePersistedHandoff(
    const Domain::LegacyContinuityPersistOutcome& outcome,
    const bool finalize)
{
    if (outcome.handoffRequired != finalize ||
        (finalize && !outcome.record.packet.resumeReady)) {
        return failure<HandoffReceipt>(
            Domain::ErrorCodes::IntegrityFailure,
            "The legacy continuity result does not match the requested mode.");
    }
    auto seed = resumeSeed(outcome.record.packet);
    if (!seed) {
        return Domain::Result<HandoffReceipt>::failure(
            std::move(seed).error());
    }
    return Domain::Result<HandoffReceipt>::success(HandoffReceipt{
        outcome.record.packet.id.value(),
        std::move(seed).value()});
}

[[nodiscard]] std::string joinRecentTools(
    const std::vector<std::string>& tools)
{
    std::vector<std::string> unique;
    unique.reserve(tools.size());
    for (const auto& tool : tools) {
        if (std::find(unique.begin(), unique.end(), tool) == unique.end()) {
            unique.push_back(tool);
        }
    }
    if (unique.size() > MaximumNarrativeTools) {
        unique.erase(
            unique.begin(),
            unique.end() - static_cast<std::ptrdiff_t>(MaximumNarrativeTools));
    }
    std::string joined;
    for (const auto& tool : unique) {
        if (!joined.empty()) {
            joined += ", ";
        }
        joined += tool;
    }
    return joined;
}

[[nodiscard]] std::vector<std::string> inferredKeyFiles(
    const std::vector<std::string>& paths)
{
    std::set<std::string> sorted{paths.begin(), paths.end()};
    std::vector<std::string> result{sorted.begin(), sorted.end()};
    if (result.size() > MaximumInferredKeyFiles) {
        result.erase(
            result.begin(),
            result.end() -
                static_cast<std::ptrdiff_t>(MaximumInferredKeyFiles));
    }
    return result;
}

} // namespace

class McpInvocationGuard::Implementation final {
    struct StateToken final {
        std::string clientKey;
        std::uint64_t generation{};
        bool recoveryLease{};
    };

    class StateLease final {
    public:
        StateLease(
            Implementation& owner,
            std::optional<StateToken> token) noexcept
            : owner_{owner}, token_{std::move(token)}
        {
        }

        ~StateLease() noexcept
        {
            if (token_) {
                owner_.releaseContinuityState(*token_);
            }
        }

        StateLease(const StateLease&) = delete;
        StateLease& operator=(const StateLease&) = delete;
        StateLease(StateLease&&) = delete;
        StateLease& operator=(StateLease&&) = delete;

        [[nodiscard]] const StateToken* token() const noexcept
        {
            return token_ ? &*token_ : nullptr;
        }

    private:
        Implementation& owner_;
        std::optional<StateToken> token_;
    };

public:
    Implementation(
        Contracts::ILegacyContextContinuityService& continuity,
        Contracts::IHasher& hasher,
        Contracts::IClock& clock,
        McpInvocationGuardPolicy policy)
        : continuity_{continuity},
          hasher_{hasher},
          clock_{clock},
          policy_{policy}
    {
    }

    [[nodiscard]] Domain::Result<Domain::ToolInvocationAdmission>
    beforeInvoke(
        const Domain::ToolCallRequest& request,
        const Domain::ToolDescriptor& descriptor,
        const Domain::OperationContext& context) noexcept
    {
        try {
            auto current = validateContext(context, clock_);
            if (!current) {
                return Domain::Result<Domain::ToolInvocationAdmission>::failure(
                    std::move(current).error());
            }
            if (descriptor.name != request.toolName ||
                request.metadata.correlationId != context.correlationId) {
                return failure<Domain::ToolInvocationAdmission>(
                    Domain::ErrorCodes::IntegrityFailure,
                    "The MCP invocation guard received a mismatched request.");
            }

            const bool continuityTool = isContinuityTool(request.toolName);
            const bool progressTool = isProgressTool(request.toolName);

            std::string fingerprint;
            if (!continuityTool) {
                const auto characters = std::span{
                    request.canonicalArguments.data(),
                    request.canonicalArguments.size()};
                auto digest = hasher_.sha256(std::as_bytes(characters));
                if (!digest) {
                    return Domain::Result<
                        Domain::ToolInvocationAdmission>::failure(
                            std::move(digest).error());
                }
                fingerprint = request.toolName + "|" + digest.value().value();
            }

            std::optional<BlockSnapshot> blocked;
            std::optional<StateToken> hardLoopState;
            std::uint64_t loopCount{};
            {
                std::lock_guard lock{mutex_};
                if (stopping_) {
                    return failure<Domain::ToolInvocationAdmission>(
                        Domain::ErrorCodes::TransportClosed,
                        "The MCP invocation guard is shutting down.");
                }
                const auto clientKey = request.metadata.clientId.value();
                if (!continuityTool && !isResumeTool(request.toolName)) {
                    const auto state = continuityStates_.find(clientKey);
                    if (state != continuityStates_.end() &&
                        state->second.blocked) {
                        blocked = BlockSnapshot{
                            state->second.handoffId,
                            state->second.resumeSeed};
                    }
                }
                if (blocked) {
                    return Domain::Result<
                        Domain::ToolInvocationAdmission>::success(
                            Domain::ToolInvocationAdmission{
                                contextBudgetOutcome(request, *blocked)});
                }

                if (!continuityTool) {
                    makeRoom(loopStates_, MaximumTrackedLoopClients, clientKey);
                    auto& loop = loopStates_[clientKey];
                    if (loop.fingerprint == fingerprint) {
                        if (loop.count !=
                            (std::numeric_limits<std::uint64_t>::max)()) {
                            ++loop.count;
                        }
                    } else {
                        loop.fingerprint = std::move(fingerprint);
                        loop.count = 1U;
                    }
                    loopCount = loop.count;
                }

                if (loopCount >= policy_.hardIdenticalCallCount) {
                    auto reserved = reserveContinuityStateLocked(
                        request.metadata.clientId);
                    if (!reserved) {
                        return Domain::Result<
                            Domain::ToolInvocationAdmission>::failure(
                                std::move(reserved).error());
                    }
                    hardLoopState = std::move(reserved).value();
                } else {
                    const auto operationKey = context.operationId.value();
                    if (pending_.contains(operationKey)) {
                        return failure<Domain::ToolInvocationAdmission>(
                            Domain::ErrorCodes::OwnershipConflict,
                            "The MCP invocation guard operation is already pending.");
                    }
                    if (pending_.size() >= MaximumPendingCalls) {
                        return failure<Domain::ToolInvocationAdmission>(
                            Domain::ErrorCodes::LimitExceeded,
                            "The MCP invocation guard pending-call bound was reached.",
                            true);
                    }
                    std::optional<StateToken> continuityState;
                    if (progressTool || request.toolName == "context_get") {
                        auto reserved = reserveContinuityStateLocked(
                            request.metadata.clientId,
                            request.toolName == "context_get");
                        if (!reserved) {
                            return Domain::Result<
                                Domain::ToolInvocationAdmission>::failure(
                                    std::move(reserved).error());
                        }
                        continuityState = std::move(reserved).value();
                    }
                    try {
                        pending_.emplace(
                            operationKey,
                            PendingCall{
                                request.metadata.clientId,
                                request.toolName,
                                loopCount,
                                continuityTool,
                                progressTool,
                                isForcePersistTool(request.toolName),
                                continuityState});
                    } catch (...) {
                        if (continuityState) {
                            releaseContinuityStateLocked(*continuityState);
                        }
                        throw;
                    }
                    return Domain::Result<
                        Domain::ToolInvocationAdmission>::success(
                            Domain::ToolInvocationAdmission{});
                }
            }

            try {
                auto outcome = hardLoopOutcome(
                    request, loopCount, *hardLoopState, context);
                releaseContinuityState(*hardLoopState);
                return Domain::Result<
                    Domain::ToolInvocationAdmission>::success(
                        Domain::ToolInvocationAdmission{std::move(outcome)});
            } catch (...) {
                releaseContinuityState(*hardLoopState);
                throw;
            }
        } catch (...) {
            return failure<Domain::ToolInvocationAdmission>(
                Domain::ErrorCodes::InternalFailure,
                "The MCP invocation guard failed safely.");
        }
    }

    [[nodiscard]] ToolOutcomeResult afterInvoke(
        const Domain::ToolCallRequest& request,
        const Domain::ToolDescriptor& descriptor,
        ToolOutcomeResult outcome,
        const Domain::OperationContext& context) noexcept
    {
        try {
            auto pending = takePending(context.operationId);
            if (!pending) {
                return outcome;
            }
            StateLease stateLease{
                *this, std::move(pending->continuityState)};
            if (pending->toolName != request.toolName ||
                descriptor.name != request.toolName ||
                pending->clientId != request.metadata.clientId) {
                return failure<Domain::ToolCallOutcome>(
                    Domain::ErrorCodes::IntegrityFailure,
                    "The MCP invocation completion does not match its admission.");
            }
            if (outcome) {
                const auto& receipt = outcome.value().receipt;
                if (receipt.requestId != request.metadata.requestId ||
                    receipt.toolName != request.toolName ||
                    receipt.ok == receipt.error.has_value() ||
                    outcome.value().canonicalPayload.empty() ||
                    outcome.value().canonicalPayload.size() >
                        MaximumCanonicalPayloadBytes ||
                    outcome.value().canonicalPayload.find('\0') !=
                        std::string::npos ||
                    !Domain::isValidUtf8(
                        outcome.value().canonicalPayload)) {
                    return failure<Domain::ToolCallOutcome>(
                        Domain::ErrorCodes::IntegrityFailure,
                        "The MCP invocation receipt does not match its request or result state.");
                }
            }

            const bool succeeded = outcome && outcome.value().receipt.ok;
            auto current = validateContext(context, clock_);
            if (!current || isStopping()) {
                return outcome;
            }

            auto finalOutcome = std::move(outcome);
            if (pending->toolName == "context_get" && succeeded) {
                const auto& recovery = finalOutcome.value().contextRecovery;
                if (recovery && recovery->clientId != pending->clientId) {
                    return failure<Domain::ToolCallOutcome>(
                        Domain::ErrorCodes::IntegrityFailure,
                        "The recovered context receipt belongs to another client.");
                }
                if (recovery) {
                    if (!stateLease.token()) {
                        return failure<Domain::ToolCallOutcome>(
                            Domain::ErrorCodes::IntegrityFailure,
                            "The recovered context has no continuity-state lease.");
                    }
                    const auto recoveryDecision = recordRecoveredContext(
                        *stateLease.token(), *recovery);
                    finalOutcome = annotate(
                        request,
                        std::move(finalOutcome),
                        Json{{"context_budget_cleared",
                              recoveryDecision.cleared}});
                }
            }
            const auto observation = finalOutcome
                ? finalOutcome.value().continuityObservation
                : std::optional<Domain::ToolContinuityObservation>{};
            if (!pending->continuityTool && observation &&
                stateLease.token()) {
                recordImplicitRoot(*stateLease.token(), *observation);
            }
            if (!pending->continuityTool &&
                pending->loopCount == policy_.softIdenticalCallCount) {
                auto persisted = continuity_.budgetHandoff(
                    pending->clientId,
                    "soft_budget identical " + pending->toolName +
                        " count=" + std::to_string(pending->loopCount),
                    context);
                if (persisted) {
                    auto receipt = validatePersistedHandoff(
                        persisted.value(), true);
                    if (receipt) {
                        Json additions{
                            {"continuity_note",
                             "Repeated identical tool calls detected. Handoff " +
                                 receipt.value().id +
                                 " saved. Prefer new chat + context_get, or change arguments."},
                            {"handoff_id", receipt.value().id},
                            {"handoff_required", true},
                            {"resume_seed", receipt.value().resumeSeed}};
                        finalOutcome = annotate(
                            request,
                            std::move(finalOutcome),
                            std::move(additions));
                    }
                }
            }

            if (succeeded && pending->progressTool && stateLease.token()) {
                auto progress = recordProgress(
                    *pending, *stateLease.token(), observation);
                if (progress.persist) {
                    try {
                        Domain::LegacyContinuityPatch patch;
                        if (progress.workingDirectory) {
                            patch.workingDirectory = progress.workingDirectory;
                        }
                        patch.keyFiles = inferredKeyFiles(progress.recentPaths);
                        patch.narrative =
                            "Auto-saved after tools: " +
                            joinRecentTools(progress.recentTools) + ".";
                        patch.nextActions = std::vector<std::string>{
                            "Call context_get if this is a new chat",
                            "Continue from the workspace in this packet"};
                        const std::string reason = progress.finalize
                            ? "auto_handoff progress=" +
                                std::to_string(progress.progressCount)
                            : "auto_checkpoint progress=" +
                                std::to_string(progress.progressCount);
                        auto persisted = continuity_.automaticPersist(
                            Domain::LegacyContinuityAutomaticRequest{
                                std::move(patch), reason, progress.finalize},
                            pending->clientId,
                            context);
                        bool persistedProgress{};
                        if (persisted) {
                            auto receipt = validatePersistedHandoff(
                                persisted.value(), progress.finalize);
                            if (receipt) {
                                recordPersistedProgress(
                                    *stateLease.token(),
                                    progress,
                                    receipt.value());
                                persistedProgress = true;
                                Json additions{
                                    {"auto_continuity",
                                     progress.finalize
                                         ? "handoff"
                                         : "checkpoint"},
                                    {"auto_handoff_id", receipt.value().id}};
                                if (progress.finalize) {
                                    additions["continuity_note"] =
                                        "Context budget: Forge auto-saved handoff " +
                                        receipt.value().id +
                                        ". Further project tools are blocked on this client until context_get in a new chat.";
                                    additions["handoff_id"] = receipt.value().id;
                                    additions["handoff_required"] = true;
                                    additions["resume_seed"] =
                                        receipt.value().resumeSeed;
                                }
                                finalOutcome = annotate(
                                    request,
                                    std::move(finalOutcome),
                                    std::move(additions));
                            }
                        }
                        if (!persistedProgress) {
                            releasePersistenceReservation(
                                *stateLease.token(), progress);
                        }
                    } catch (...) {
                        releasePersistenceReservation(
                            *stateLease.token(), progress);
                        throw;
                    }
                }
            }
            return finalOutcome;
        } catch (...) {
            return failure<Domain::ToolCallOutcome>(
                Domain::ErrorCodes::InternalFailure,
                "The MCP invocation completion guard failed safely.");
        }
    }

    void cancel(const Domain::OperationId& operationId) noexcept
    {
        try {
            std::lock_guard lock{mutex_};
            const auto found = pending_.find(operationId.value());
            if (found == pending_.end()) {
                return;
            }
            if (found->second.continuityState) {
                releaseContinuityStateLocked(
                    *found->second.continuityState);
            }
            pending_.erase(found);
        } catch (...) {
        }
    }

    void shutdown() noexcept
    {
        try {
            std::lock_guard lock{mutex_};
            if (stopping_) {
                return;
            }
            stopping_ = true;
            pending_.clear();
            loopStates_.clear();
            continuityStates_.clear();
        } catch (...) {
        }
    }

    [[nodiscard]] std::size_t trackedLoopClientCount() const noexcept
    {
        try {
            std::lock_guard lock{mutex_};
            return loopStates_.size();
        } catch (...) {
            return 0U;
        }
    }

    [[nodiscard]] Domain::ContinuityAutomationStatusSnapshot snapshot(
        const Domain::ClientId& clientId) const noexcept
    {
        Domain::ContinuityAutomationStatusSnapshot result;
        result.checkpointEveryTools = policy_.checkpointProgressCount;
        result.handoffEveryTools = policy_.handoffProgressCount;
        try {
            std::lock_guard lock{mutex_};
            const auto found = continuityStates_.find(clientId.value());
            if (found == continuityStates_.end()) {
                return result;
            }
            result.progressCount = found->second.progressCount;
            result.blocked = found->second.blocked;
            result.handoffId = found->second.handoffId;
            result.implicitRoots = found->second.implicitRoots;
        } catch (...) {
            result.progressCount = 0U;
            result.blocked = false;
            result.handoffId.reset();
            result.implicitRoots.clear();
        }
        return result;
    }

    [[nodiscard]] std::size_t trackedContinuityClientCount() const noexcept
    {
        try {
            std::lock_guard lock{mutex_};
            return continuityStates_.size();
        } catch (...) {
            return 0U;
        }
    }

    [[nodiscard]] std::size_t pendingCallCount() const noexcept
    {
        try {
            std::lock_guard lock{mutex_};
            return pending_.size();
        } catch (...) {
            return 0U;
        }
    }

private:
    struct LoopState final {
        std::string fingerprint;
        std::uint64_t count{};
    };

    struct ClientContinuityState final {
        std::uint64_t stateGeneration{};
        std::size_t activeUseCount{};
        std::uint64_t progressCount{};
        std::uint64_t lastCheckpointCount{};
        std::uint64_t lastHandoffCount{};
        std::optional<Domain::MonotonicTimePoint> lastCheckpointAt;
        std::optional<Domain::MonotonicTimePoint> lastHandoffAt;
        std::vector<std::string> recentTools;
        std::vector<std::string> recentPaths;
        std::optional<std::string> workingDirectory;
        std::vector<Domain::PathText> implicitRoots;
        bool blocked{};
        std::optional<std::string> handoffId;
        std::optional<std::string> resumeSeed;
        bool persistenceInFlight{};
        std::uint64_t persistenceGeneration{};
        bool recoveryInFlight{};
    };

    struct PendingCall final {
        Domain::ClientId clientId;
        std::string toolName;
        std::uint64_t loopCount{};
        bool continuityTool{};
        bool progressTool{};
        bool forcePersist{};
        std::optional<StateToken> continuityState;
    };

    struct BlockSnapshot final {
        std::optional<std::string> handoffId;
        std::optional<std::string> resumeSeed;
    };

    struct RecoveryDecision final {
        bool accepted{};
        bool cleared{};
    };

    struct ProgressDecision final {
        bool persist{};
        bool finalize{};
        std::uint64_t progressCount{};
        Domain::MonotonicTimePoint observedAt;
        std::vector<std::string> recentTools;
        std::vector<std::string> recentPaths;
        std::optional<std::string> workingDirectory;
        std::uint64_t persistenceGeneration{};
    };

    template <typename Map>
    static void makeRoom(
        Map& values,
        const std::size_t maximum,
        const std::string& key)
    {
        if (!values.contains(key) && values.size() >= maximum) {
            values.erase(values.begin());
        }
    }

    [[nodiscard]] Domain::Result<StateToken> reserveContinuityStateLocked(
        const Domain::ClientId& clientId,
        const bool recoveryLease = false)
    {
        const auto& key = clientId.value();
        const auto existing = continuityStates_.find(key);
        if (existing != continuityStates_.end()) {
            if (recoveryLease &&
                (existing->second.activeUseCount != 0U ||
                 existing->second.persistenceInFlight ||
                 existing->second.recoveryInFlight)) {
                return failure<StateToken>(
                    Domain::ErrorCodes::OwnershipConflict,
                    "The MCP client has another continuity mutation in flight.",
                    true);
            }
            if (!recoveryLease && existing->second.recoveryInFlight) {
                return failure<StateToken>(
                    Domain::ErrorCodes::OwnershipConflict,
                    "The MCP client has a context recovery in flight.",
                    true);
            }
            if (existing->second.activeUseCount ==
                (std::numeric_limits<std::size_t>::max)()) {
                return failure<StateToken>(
                    Domain::ErrorCodes::LimitExceeded,
                    "The MCP continuity-state use count was exhausted.",
                    true);
            }
            ++existing->second.activeUseCount;
            if (recoveryLease) {
                existing->second.recoveryInFlight = true;
            }
            return Domain::Result<StateToken>::success(StateToken{
                key, existing->second.stateGeneration, recoveryLease});
        }

        if (continuityStates_.size() >=
            MaximumTrackedContinuityClients) {
            const auto evictable = std::find_if(
                continuityStates_.begin(),
                continuityStates_.end(),
                [](const auto& entry) {
                    return entry.second.activeUseCount == 0U &&
                        !entry.second.persistenceInFlight &&
                        !entry.second.blocked;
                });
            if (evictable == continuityStates_.end()) {
                return failure<StateToken>(
                    Domain::ErrorCodes::LimitExceeded,
                    "The MCP continuity client bound is fully occupied.",
                    true);
            }
            continuityStates_.erase(evictable);
        }

        if (nextContinuityStateGeneration_ ==
            (std::numeric_limits<std::uint64_t>::max)()) {
            return failure<StateToken>(
                Domain::ErrorCodes::LimitExceeded,
                "The MCP continuity-state generation was exhausted.",
                true);
        }
        ++nextContinuityStateGeneration_;
        ClientContinuityState state;
        state.stateGeneration = nextContinuityStateGeneration_;
        state.activeUseCount = 1U;
        state.recoveryInFlight = recoveryLease;
        continuityStates_.emplace(key, std::move(state));
        return Domain::Result<StateToken>::success(StateToken{
            key, nextContinuityStateGeneration_, recoveryLease});
    }

    [[nodiscard]] ClientContinuityState* findContinuityStateLocked(
        const StateToken& token) noexcept
    {
        const auto found = continuityStates_.find(token.clientKey);
        if (found == continuityStates_.end() ||
            found->second.stateGeneration != token.generation) {
            return nullptr;
        }
        return &found->second;
    }

    void releaseContinuityStateLocked(const StateToken& token) noexcept
    {
        auto* const state = findContinuityStateLocked(token);
        if (state != nullptr && state->activeUseCount != 0U) {
            --state->activeUseCount;
            if (token.recoveryLease) {
                state->recoveryInFlight = false;
            }
        }
    }

    void releaseContinuityState(const StateToken& token) noexcept
    {
        try {
            std::lock_guard lock{mutex_};
            releaseContinuityStateLocked(token);
        } catch (...) {
        }
    }

    [[nodiscard]] Domain::ToolCallOutcome contextBudgetOutcome(
        const Domain::ToolCallRequest& request,
        const BlockSnapshot& blocked) const
    {
        Json additions{
            {"handoff_id",
             blocked.handoffId ? Json(*blocked.handoffId) : Json(nullptr)},
            {"handoff_required", true},
            {"resume_seed",
             blocked.resumeSeed ? Json(*blocked.resumeSeed) : Json(nullptr)}};
        return policyErrorOutcome(
            request,
            "context_budget_exceeded",
            "This chat has been handed off. Start a new LM Studio chat with Forge MCP enabled, then call context_get. Further filesystem, shell, and Git tools are blocked here.",
            false,
            std::move(additions));
    }

    [[nodiscard]] Domain::ToolCallOutcome hardLoopOutcome(
        const Domain::ToolCallRequest& request,
        const std::uint64_t loopCount,
        const StateToken& stateToken,
        const Domain::OperationContext& context)
    {
        const std::string reason =
            "identical_call_loop tool=" + request.toolName +
            " count=" + std::to_string(loopCount);
        auto persisted = continuity_.budgetHandoff(
            request.metadata.clientId, reason, context);
        if (persisted) {
            auto receipt = validatePersistedHandoff(persisted.value(), true);
            if (receipt) {
                markBlocked(stateToken, receipt.value());
                Json additions{
                    {"handoff_id", receipt.value().id},
                    {"handoff_required", true},
                    {"resume_seed", receipt.value().resumeSeed}};
                return policyErrorOutcome(
                    request,
                    "identical_call_loop",
                    "Blocked repeated identical " + request.toolName + " (" +
                        std::to_string(loopCount) +
                        " times). Auto handoff " + receipt.value().id +
                        " written. Start a new chat and call context_get.",
                    false,
                    std::move(additions));
            }
        }

        Json additions{
            {"handoff_persisted", false},
            {"handoff_required", true},
            {"loop_code", "identical_call_loop"}};
        return policyErrorOutcome(
            request,
            "continuity_persistence_failed",
            "Blocked repeated identical " + request.toolName + " (" +
                std::to_string(loopCount) +
                " times), but the required continuity handoff could not be persisted. Resolve local continuity storage and call session_handoff before continuing.",
            true,
            std::move(additions));
    }

    void markBlocked(
        const StateToken& token,
        const HandoffReceipt& receipt)
    {
        std::lock_guard lock{mutex_};
        if (stopping_) {
            return;
        }
        auto* const state = findContinuityStateLocked(token);
        if (state == nullptr) {
            return;
        }
        state->blocked = true;
        state->handoffId = receipt.id;
        state->resumeSeed = receipt.resumeSeed;
    }

    [[nodiscard]] std::optional<PendingCall> takePending(
        const Domain::OperationId& operationId)
    {
        std::lock_guard lock{mutex_};
        const auto found = pending_.find(operationId.value());
        if (found == pending_.end()) {
            return std::nullopt;
        }
        auto result = std::move(found->second);
        pending_.erase(found);
        return result;
    }

    [[nodiscard]] bool isStopping() const
    {
        std::lock_guard lock{mutex_};
        return stopping_;
    }

    [[nodiscard]] ProgressDecision recordProgress(
        const PendingCall& pending,
        const StateToken& token,
        const std::optional<Domain::ToolContinuityObservation>& observation)
    {
        std::lock_guard lock{mutex_};
        auto* const state = findContinuityStateLocked(token);
        if (state == nullptr) {
            return ProgressDecision{};
        }
        if (state->progressCount !=
            (std::numeric_limits<std::uint64_t>::max)()) {
            ++state->progressCount;
        }
        state->recentTools.push_back(pending.toolName);
        if (state->recentTools.size() > MaximumRecentTools) {
            state->recentTools.erase(
                state->recentTools.begin(),
                state->recentTools.begin() +
                    static_cast<std::ptrdiff_t>(
                        state->recentTools.size() - MaximumRecentTools));
        }
        std::optional<std::string> observedPath;
        if (observation) {
            const auto& path = observation->path
                ? observation->path
                : observation->workingDirectory;
            if (path) {
                observedPath = path->value();
            }
        }
        if (observedPath) {
            state->recentPaths.push_back(*observedPath);
            if (state->recentPaths.size() > MaximumRecentPaths) {
                state->recentPaths.erase(
                    state->recentPaths.begin(),
                    state->recentPaths.begin() +
                        static_cast<std::ptrdiff_t>(
                            state->recentPaths.size() - MaximumRecentPaths));
            }
        }
        if (observation && observation->workingDirectory) {
            state->workingDirectory =
                observation->workingDirectory->value();
        }

        const auto now = clock_.monotonicNow();
        const auto sinceCheckpoint =
            state->progressCount - state->lastCheckpointCount;
        const auto sinceHandoff =
            state->progressCount - state->lastHandoffCount;
        const bool checkpointTimeDue = state->lastCheckpointAt &&
            now - *state->lastCheckpointAt >=
                std::chrono::seconds{policy_.checkpointIntervalSeconds};
        const bool handoffTimeDue = state->lastHandoffAt &&
            now - *state->lastHandoffAt >=
                std::chrono::seconds{policy_.handoffIntervalSeconds};
        const bool handoffDue =
            sinceHandoff >= policy_.handoffProgressCount || handoffTimeDue;
        const bool checkpointDue = pending.forcePersist ||
            sinceCheckpoint >= policy_.checkpointProgressCount ||
            checkpointTimeDue;
        ProgressDecision decision{
            false,
            handoffDue,
            state->progressCount,
            now,
            state->recentTools,
            state->recentPaths,
            state->workingDirectory,
            0U};
        if (!state->blocked && !state->persistenceInFlight &&
            (checkpointDue || handoffDue)) {
            if (nextPersistenceGeneration_ ==
                (std::numeric_limits<std::uint64_t>::max)()) {
                nextPersistenceGeneration_ = 1U;
            } else {
                ++nextPersistenceGeneration_;
            }
            state->persistenceInFlight = true;
            state->persistenceGeneration = nextPersistenceGeneration_;
            decision.persist = true;
            decision.persistenceGeneration = nextPersistenceGeneration_;
        }
        return decision;
    }

    static void appendImplicitRoot(
        ClientContinuityState& state,
        const Domain::PathText& root)
    {
        auto& roots = state.implicitRoots;
        if (std::find(roots.begin(), roots.end(), root) != roots.end()) {
            return;
        }
        roots.push_back(root);
        if (roots.size() > Domain::MaximumContinuityAutomationImplicitRoots) {
            roots.erase(
                roots.begin(),
                roots.begin() + static_cast<std::ptrdiff_t>(
                    roots.size() -
                    Domain::MaximumContinuityAutomationImplicitRoots));
        }
    }

    static void appendRecentPath(
        ClientContinuityState& state,
        const std::string& path)
    {
        state.recentPaths.push_back(path);
        if (state.recentPaths.size() > MaximumRecentPaths) {
            state.recentPaths.erase(
                state.recentPaths.begin(),
                state.recentPaths.begin() + static_cast<std::ptrdiff_t>(
                    state.recentPaths.size() - MaximumRecentPaths));
        }
    }

    void recordImplicitRoot(
        const StateToken& token,
        const Domain::ToolContinuityObservation& observation)
    {
        std::vector<Domain::PathText> roots;
        if (observation.path) {
            if (auto root = implicitRootFor(observation)) {
                roots.push_back(std::move(*root));
            }
        }
        if (observation.workingDirectory) {
            Domain::ToolContinuityObservation workingDirectory;
            workingDirectory.workingDirectory = observation.workingDirectory;
            workingDirectory.baseDirectory = observation.baseDirectory;
            if (auto root = implicitRootFor(workingDirectory)) {
                if (std::find(roots.begin(), roots.end(), *root) ==
                    roots.end()) {
                    roots.push_back(std::move(*root));
                }
            }
        }
        if (roots.empty()) {
            return;
        }

        std::lock_guard lock{mutex_};
        if (stopping_) {
            return;
        }
        auto* const state = findContinuityStateLocked(token);
        if (state != nullptr) {
            for (const auto& root : roots) {
                appendImplicitRoot(*state, root);
            }
        }
    }

    [[nodiscard]] RecoveryDecision recordRecoveredContext(
        const StateToken& token,
        const Domain::ContextRecoveryReceipt& recovery)
    {
        std::vector<Domain::PathText> roots;
        if (recovery.workingDirectory) {
            Domain::ToolContinuityObservation observation;
            observation.workingDirectory = recovery.workingDirectory;
            if (auto root = implicitRootFor(observation)) {
                roots.push_back(std::move(*root));
            }
        }
        for (const auto& keyFile : recovery.keyFiles) {
            Domain::ToolContinuityObservation observation;
            observation.path = keyFile;
            observation.workingDirectory = recovery.workingDirectory;
            if (auto root = implicitRootFor(observation)) {
                if (std::find(roots.begin(), roots.end(), *root) ==
                    roots.end()) {
                    roots.push_back(std::move(*root));
                }
            }
        }

        std::lock_guard lock{mutex_};
        if (stopping_) {
            return RecoveryDecision{};
        }
        auto* const state = findContinuityStateLocked(token);
        if (state == nullptr) {
            return RecoveryDecision{};
        }
        if (state->blocked &&
            (!state->handoffId ||
             *state->handoffId != recovery.handoffId.value())) {
            return RecoveryDecision{};
        }
        const bool cleared = state->blocked;
        if (recovery.workingDirectory) {
            state->workingDirectory = recovery.workingDirectory->value();
        }
        const auto firstKeyFile = recovery.keyFiles.size() > MaximumRecentPaths
            ? recovery.keyFiles.size() - MaximumRecentPaths
            : 0U;
        for (std::size_t index = firstKeyFile;
             index < recovery.keyFiles.size();
             ++index) {
            appendRecentPath(*state, recovery.keyFiles[index].value());
        }
        for (const auto& root : roots) {
            appendImplicitRoot(*state, root);
        }
        if (cleared) {
            state->blocked = false;
            state->progressCount = 0U;
            state->lastCheckpointCount = 0U;
            state->lastHandoffCount = 0U;
            state->lastCheckpointAt.reset();
            state->lastHandoffAt.reset();
        }
        return RecoveryDecision{true, cleared};
    }

    void recordPersistedProgress(
        const StateToken& token,
        const ProgressDecision& progress,
        const HandoffReceipt& receipt)
    {
        std::lock_guard lock{mutex_};
        if (stopping_) {
            return;
        }
        auto* const state = findContinuityStateLocked(token);
        if (state == nullptr) {
            return;
        }
        if (!state->persistenceInFlight ||
            state->persistenceGeneration != progress.persistenceGeneration) {
            return;
        }
        state->persistenceInFlight = false;
        state->lastCheckpointCount = (std::max)(
            state->lastCheckpointCount, progress.progressCount);
        if (!state->lastCheckpointAt ||
            *state->lastCheckpointAt < progress.observedAt) {
            state->lastCheckpointAt = progress.observedAt;
        }
        if (progress.finalize) {
            state->lastHandoffCount = (std::max)(
                state->lastHandoffCount, progress.progressCount);
            if (!state->lastHandoffAt ||
                *state->lastHandoffAt < progress.observedAt) {
                state->lastHandoffAt = progress.observedAt;
            }
            state->blocked = true;
            state->handoffId = receipt.id;
            state->resumeSeed = receipt.resumeSeed;
        }
    }

    void releasePersistenceReservation(
        const StateToken& token,
        const ProgressDecision& progress)
    {
        std::lock_guard lock{mutex_};
        auto* const state = findContinuityStateLocked(token);
        if (state == nullptr || !state->persistenceInFlight ||
            state->persistenceGeneration != progress.persistenceGeneration) {
            return;
        }
        state->persistenceInFlight = false;
    }

    Contracts::ILegacyContextContinuityService& continuity_;
    Contracts::IHasher& hasher_;
    Contracts::IClock& clock_;
    const McpInvocationGuardPolicy policy_;

    mutable std::mutex mutex_;
    std::map<std::string, LoopState> loopStates_;
    std::map<std::string, ClientContinuityState> continuityStates_;
    std::map<std::string, PendingCall> pending_;
    std::uint64_t nextContinuityStateGeneration_{};
    std::uint64_t nextPersistenceGeneration_{};
    bool stopping_{};
};

Domain::Result<std::unique_ptr<McpInvocationGuard>>
McpInvocationGuard::create(
    Contracts::ILegacyContextContinuityService& continuity,
    Contracts::IHasher& hasher,
    Contracts::IClock& clock,
    McpInvocationGuardPolicy policy) noexcept
{
    try {
        auto valid = validatePolicy(policy);
        if (!valid) {
            return Domain::Result<std::unique_ptr<McpInvocationGuard>>::failure(
                std::move(valid).error());
        }
        return Domain::Result<std::unique_ptr<McpInvocationGuard>>::success(
            std::unique_ptr<McpInvocationGuard>{new McpInvocationGuard{
                std::make_unique<Implementation>(
                    continuity, hasher, clock, std::move(policy))}});
    } catch (...) {
        return failure<std::unique_ptr<McpInvocationGuard>>(
            Domain::ErrorCodes::InternalFailure,
            "The MCP invocation guard could not be created.");
    }
}

McpInvocationGuard::McpInvocationGuard(
    std::unique_ptr<Implementation> implementation) noexcept
    : implementation_{std::move(implementation)}
{
}

McpInvocationGuard::~McpInvocationGuard() noexcept
{
    shutdown();
}

Domain::Result<Domain::ToolInvocationAdmission>
McpInvocationGuard::beforeInvoke(
    const Domain::ToolCallRequest& request,
    const Domain::ToolDescriptor& descriptor,
    const Domain::OperationContext& context) noexcept
{
    if (!implementation_) {
        return failure<Domain::ToolInvocationAdmission>(
            Domain::ErrorCodes::TransportClosed,
            "The MCP invocation guard has no implementation.");
    }
    return implementation_->beforeInvoke(request, descriptor, context);
}

Domain::Result<Domain::ToolCallOutcome> McpInvocationGuard::afterInvoke(
    const Domain::ToolCallRequest& request,
    const Domain::ToolDescriptor& descriptor,
    Domain::Result<Domain::ToolCallOutcome> outcome,
    const Domain::OperationContext& context) noexcept
{
    if (!implementation_) {
        return failure<Domain::ToolCallOutcome>(
            Domain::ErrorCodes::TransportClosed,
            "The MCP invocation guard has no implementation.");
    }
    return implementation_->afterInvoke(
        request, descriptor, std::move(outcome), context);
}

void McpInvocationGuard::cancel(
    const Domain::OperationId& operationId) noexcept
{
    if (implementation_) {
        implementation_->cancel(operationId);
    }
}

void McpInvocationGuard::shutdown() noexcept
{
    if (implementation_) {
        implementation_->shutdown();
    }
}

Domain::ContinuityAutomationStatusSnapshot McpInvocationGuard::snapshot(
    const Domain::ClientId& clientId) const noexcept
{
    return implementation_
        ? implementation_->snapshot(clientId)
        : Domain::ContinuityAutomationStatusSnapshot{};
}

std::size_t McpInvocationGuard::trackedLoopClientCount() const noexcept
{
    return implementation_ ? implementation_->trackedLoopClientCount() : 0U;
}

std::size_t McpInvocationGuard::trackedContinuityClientCount() const noexcept
{
    return implementation_
        ? implementation_->trackedContinuityClientCount()
        : 0U;
}

std::size_t McpInvocationGuard::pendingCallCount() const noexcept
{
    return implementation_ ? implementation_->pendingCallCount() : 0U;
}

} // namespace ForgeConductor::Mcp
