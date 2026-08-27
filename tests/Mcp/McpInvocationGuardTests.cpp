#include "ForgeConductor/Mcp/McpInvocationGuard.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <mutex>
#include <optional>
#include <span>
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
namespace Contracts = ForgeConductor::Contracts;
namespace Domain = ForgeConductor::Domain;
namespace Mcp = ForgeConductor::Mcp;
using Json = nlohmann::json;

#define REQUIRE(condition)                                                        \
    do {                                                                          \
        if (!(condition)) {                                                       \
            throw std::runtime_error{                                             \
                std::string{"Requirement failed: "} + #condition};              \
        }                                                                         \
    } while (false)

template <typename T>
[[nodiscard]] T take(Domain::Result<T> result)
{
    if (!result) {
        throw std::runtime_error{
            result.error().code + ": " + result.error().message};
    }
    return std::move(result).value();
}

template <typename T>
[[nodiscard]] T id(const std::string_view value)
{
    return take(T::parse(value));
}

class FixedClock final : public Contracts::IClock {
public:
    [[nodiscard]] Domain::UtcTimePoint utcNow() const noexcept override
    {
        return Domain::UtcTimePoint{1'735'789'855s};
    }

    [[nodiscard]] Domain::MonotonicTimePoint monotonicNow() const noexcept override
    {
        return now_;
    }

    void setNow(const Domain::MonotonicTimePoint now) noexcept { now_ = now; }

private:
    Domain::MonotonicTimePoint now_{1s};
};

class FixedHasher final : public Contracts::IHasher {
public:
    [[nodiscard]] Domain::Result<Domain::Sha256Digest> sha256(
        const std::span<const std::byte> bytes) noexcept override
    {
        try {
            unsigned int sum{};
            for (const auto value : bytes) {
                sum = (sum + std::to_integer<unsigned int>(value)) % 16U;
            }
            const char digit = sum < 10U
                ? static_cast<char>('0' + sum)
                : static_cast<char>('a' + (sum - 10U));
            return Domain::Sha256Digest::parse(std::string(64U, digit));
        } catch (...) {
            return Domain::Result<Domain::Sha256Digest>::failure(
                Domain::makeError(
                    Domain::ErrorCodes::InternalFailure,
                    "The fixed hash could not be produced."));
        }
    }
};

class LegacyContinuityFake final
    : public Contracts::ILegacyContextContinuityService {
public:
    bool failBudget{};
    bool failAutomatic{};

    [[nodiscard]] Domain::Result<Domain::LegacyContinuityPersistOutcome>
    checkpoint(
        const Domain::LegacyContinuityWriteRequest&,
        const Domain::ClientId&,
        Domain::LegacyHandoffSource,
        const Domain::OperationContext&) noexcept override
    {
        return unsupported<Domain::LegacyContinuityPersistOutcome>();
    }

    [[nodiscard]] Domain::Result<Domain::LegacyContinuityPersistOutcome>
    handoff(
        const Domain::LegacyContinuityWriteRequest&,
        const Domain::ClientId&,
        Domain::LegacyHandoffSource,
        const Domain::OperationContext&) noexcept override
    {
        return unsupported<Domain::LegacyContinuityPersistOutcome>();
    }

    [[nodiscard]] Domain::Result<Domain::LegacyContinuityPersistOutcome>
    automaticPersist(
        const Domain::LegacyContinuityAutomaticRequest& request,
        const Domain::ClientId& clientId,
        const Domain::OperationContext&) noexcept override
    {
        try {
            automaticCalls_.fetch_add(1U, std::memory_order_relaxed);
            {
                std::unique_lock lock{mutex_};
                automaticRequests_.push_back(request);
                if (blockAutomatic_) {
                    ++automaticEnteredCount_;
                    changed_.notify_all();
                    if (!changed_.wait_for(
                            lock,
                            30s,
                            [&] { return releaseAutomatic_; })) {
                        return failed<
                            Domain::LegacyContinuityPersistOutcome>();
                    }
                }
            }
            if (failAutomatic) {
                return failed<Domain::LegacyContinuityPersistOutcome>();
            }
            return Domain::Result<
                Domain::LegacyContinuityPersistOutcome>::success(
                    outcome(clientId, request.finalize));
        } catch (...) {
            return failed<Domain::LegacyContinuityPersistOutcome>();
        }
    }

    [[nodiscard]] Domain::Result<Domain::LegacyContinuityPersistOutcome>
    budgetHandoff(
        const Domain::ClientId& clientId,
        const std::string_view reason,
        const Domain::OperationContext&) noexcept override
    {
        try {
            budgetCalls_.fetch_add(1U, std::memory_order_relaxed);
            {
                std::lock_guard lock{mutex_};
                budgetReasons_.emplace_back(reason);
            }
            if (failBudget) {
                return failed<Domain::LegacyContinuityPersistOutcome>();
            }
            return Domain::Result<
                Domain::LegacyContinuityPersistOutcome>::success(
                    outcome(clientId, true));
        } catch (...) {
            return failed<Domain::LegacyContinuityPersistOutcome>();
        }
    }

    [[nodiscard]] Domain::Result<Domain::LegacyContinuityGetOutcome> get(
        const Domain::LegacyContinuityGetRequest&,
        const Domain::OperationContext&) noexcept override
    {
        return unsupported<Domain::LegacyContinuityGetOutcome>();
    }

    [[nodiscard]] Domain::Result<Domain::LegacyContinuityListOutcome> list(
        const Domain::LegacyContinuityListRequest&,
        const Domain::OperationContext&) noexcept override
    {
        return unsupported<Domain::LegacyContinuityListOutcome>();
    }

    [[nodiscard]] Domain::Result<
        Domain::LegacyContinuityProjectionRepairOutcome>
    repairProjections(const Domain::OperationContext&) noexcept override
    {
        return unsupported<
            Domain::LegacyContinuityProjectionRepairOutcome>();
    }

    [[nodiscard]] Domain::Result<Domain::LegacyContinuityResetOutcome> reset(
        const Domain::DestructiveConfirmation&,
        const Domain::OperationContext&) noexcept override
    {
        return unsupported<Domain::LegacyContinuityResetOutcome>();
    }

    void shutdown() noexcept override { shutdown_ = true; }

    [[nodiscard]] std::size_t budgetCalls() const noexcept
    {
        return budgetCalls_.load(std::memory_order_relaxed);
    }

    [[nodiscard]] std::size_t automaticCalls() const noexcept
    {
        return automaticCalls_.load(std::memory_order_relaxed);
    }

    [[nodiscard]] const std::vector<std::string>& budgetReasons() const noexcept
    {
        return budgetReasons_;
    }

    [[nodiscard]] const std::vector<Domain::LegacyContinuityAutomaticRequest>&
    automaticRequests() const noexcept
    {
        return automaticRequests_;
    }

    void blockAutomaticPersistence()
    {
        std::lock_guard lock{mutex_};
        blockAutomatic_ = true;
        automaticEnteredCount_ = 0U;
        releaseAutomatic_ = false;
    }

    [[nodiscard]] bool waitForAutomaticPersistence(
        const std::size_t expectedCount = 1U)
    {
        std::unique_lock lock{mutex_};
        return changed_.wait_for(
            lock,
            30s,
            [&] { return automaticEnteredCount_ >= expectedCount; });
    }

    void releaseAutomaticPersistence()
    {
        {
            std::lock_guard lock{mutex_};
            releaseAutomatic_ = true;
            blockAutomatic_ = false;
        }
        changed_.notify_all();
    }

private:
    template <typename T>
    [[nodiscard]] static Domain::Result<T> unsupported()
    {
        return Domain::Result<T>::failure(Domain::makeError(
            Domain::ErrorCodes::HostCapabilityUnavailable,
            "The test operation is not used."));
    }

    template <typename T>
    [[nodiscard]] static Domain::Result<T> failed()
    {
        return Domain::Result<T>::failure(Domain::makeError(
            Domain::ErrorCodes::StorageFull,
            "The scripted continuity persistence failed.",
            true));
    }

    [[nodiscard]] Domain::LegacyContinuityPersistOutcome outcome(
        const Domain::ClientId& clientId,
        const bool finalize)
    {
        const auto sequence = sequence_.fetch_add(
            1U, std::memory_order_relaxed) + 1U;
        Domain::LegacyHandoffPacket packet{
            id<Domain::LegacyHandoffId>(
                "handoff-" + std::to_string(sequence)),
            Domain::LegacyContinuityLimits::SchemaVersion,
            Domain::UtcTimePoint{1'735'789'855s},
            Domain::UtcTimePoint{1'735'789'855s},
            finalize ? Domain::LegacyHandoffSource::Budget
                     : Domain::LegacyHandoffSource::Automatic,
            finalize,
            std::nullopt,
            clientId,
            "Preserve MCP work",
            finalize ? "handoff_ready" : "in_progress",
            std::nullopt,
            std::nullopt,
            {},
            {"Continue work"},
            {},
            {},
            {},
            "Runtime continuity",
            "resume-seed-" + std::to_string(sequence),
            false};
        return Domain::LegacyContinuityPersistOutcome{
            Domain::LegacyContinuityRecord{
                std::move(packet), sequence, {}},
            finalize,
            true,
            false,
            std::nullopt,
            {}};
    }

    std::atomic_uint64_t sequence_{};
    std::atomic_size_t budgetCalls_{};
    std::atomic_size_t automaticCalls_{};
    mutable std::mutex mutex_;
    std::condition_variable changed_;
    std::vector<std::string> budgetReasons_;
    std::vector<Domain::LegacyContinuityAutomaticRequest> automaticRequests_;
    bool blockAutomatic_{};
    std::size_t automaticEnteredCount_{};
    bool releaseAutomatic_{};
    bool shutdown_{};
};

[[nodiscard]] Domain::ClientId client(const std::string_view value)
{
    return id<Domain::ClientId>(value);
}

[[nodiscard]] Domain::ToolCallRequest request(
    const Domain::ClientId& caller,
    const std::string_view tool,
    std::string arguments,
    const std::uint64_t sequence)
{
    return Domain::ToolCallRequest{
        Domain::McpRequestMetadata{
            id<Domain::RequestId>("request-" + std::to_string(sequence)),
            id<Domain::CorrelationId>(
                "correlation-" + std::to_string(sequence)),
            caller,
            std::nullopt,
            "2025-03-26"},
        std::string{tool},
        std::move(arguments)};
}

[[nodiscard]] Domain::OperationContext context(
    const Domain::ToolCallRequest& call,
    const std::uint64_t sequence,
    const std::stop_token cancellation = {})
{
    std::string operation = "10000000-0000-4000-8000-";
    auto suffix = std::to_string(sequence);
    operation.append(12U - suffix.size(), '0');
    operation += suffix;
    return Domain::OperationContext{
        id<Domain::OperationId>(operation),
        Domain::MonotonicTimePoint{1'000s},
        cancellation,
        call.metadata.correlationId};
}

[[nodiscard]] Domain::ToolDescriptor descriptor(
    const Domain::ToolCallRequest& call)
{
    return Domain::ToolDescriptor{
        call.toolName,
        "Test tool",
        "TestPack",
        Domain::ToolEffect::Read,
        Domain::ToolAvailability::Available,
        false,
        false};
}

[[nodiscard]] Domain::ToolCallOutcome successOutcome(
    const Domain::ToolCallRequest& call,
    std::optional<Domain::ContextRecoveryReceipt> contextRecovery = std::nullopt,
    std::optional<Domain::ToolContinuityObservation> continuityObservation =
        std::nullopt)
{
    return Domain::ToolCallOutcome{
        Domain::ToolExecutionReceipt{
            call.metadata.requestId,
            call.toolName,
            true,
            std::nullopt,
            1ms},
        R"json({"ok":true})json",
        std::move(contextRecovery),
        std::move(continuityObservation)};
}

[[nodiscard]] Domain::ToolContinuityObservation observedPath(
    const std::string_view path,
    const std::optional<std::string_view> workingDirectory = std::nullopt)
{
    Domain::ToolContinuityObservation result;
    result.path = take(Domain::PathText::create(path));
    if (workingDirectory) {
        result.workingDirectory = take(Domain::PathText::create(
            *workingDirectory));
    }
    return result;
}

[[nodiscard]] Domain::ToolContinuityObservation observedWorkingDirectory(
    const std::string_view workingDirectory)
{
    Domain::ToolContinuityObservation result;
    result.workingDirectory = take(Domain::PathText::create(
        workingDirectory));
    return result;
}

[[nodiscard]] Json payload(const Domain::ToolCallOutcome& outcome)
{
    return Json::parse(outcome.canonicalPayload);
}

[[nodiscard]] Domain::Result<Domain::ToolCallOutcome> execute(
    Mcp::McpInvocationGuard& guard,
    const Domain::ToolCallRequest& call,
    const Domain::OperationContext& operation,
    std::optional<Domain::ContextRecoveryReceipt> contextRecovery = std::nullopt,
    std::optional<Domain::ToolContinuityObservation> continuityObservation =
        std::nullopt)
{
    auto admission = guard.beforeInvoke(call, descriptor(call), operation);
    if (!admission) {
        return Domain::Result<Domain::ToolCallOutcome>::failure(
            std::move(admission).error());
    }
    if (admission.value().immediateOutcome) {
        return Domain::Result<Domain::ToolCallOutcome>::success(
            std::move(admission).value().immediateOutcome.value());
    }
    return guard.afterInvoke(
        call,
        descriptor(call),
        Domain::Result<Domain::ToolCallOutcome>::success(
            successOutcome(
                call,
                std::move(contextRecovery),
                std::move(continuityObservation))),
        operation);
}

void identicalCallsSoftHandoffHardBlockAndResume()
{
    LegacyContinuityFake continuity;
    FixedHasher hasher;
    FixedClock clock;
    auto guard = take(Mcp::McpInvocationGuard::create(
        continuity, hasher, clock));
    const auto caller = client("loop-client");

    for (std::uint64_t index = 1U; index <= 8U; ++index) {
        const auto call = request(
            caller, "fs_read", R"json({"path":"same.txt"})json", index);
        const auto result = take(execute(*guard, call, context(call, index)));
        if (index == 4U) {
            const auto value = payload(result);
            REQUIRE(value.at("handoff_required") == true);
            REQUIRE(value.at("handoff_id") == "handoff-1");
            REQUIRE(continuity.budgetCalls() == 1U);
        }
    }

    const auto ninth = request(
        caller, "fs_read", R"json({"path":"same.txt"})json", 9U);
    const auto blockedLoop = take(guard->beforeInvoke(
        ninth, descriptor(ninth), context(ninth, 9U)));
    REQUIRE(blockedLoop.immediateOutcome.has_value());
    REQUIRE(payload(*blockedLoop.immediateOutcome).at("code") ==
            "identical_call_loop");
    REQUIRE(continuity.budgetCalls() == 2U);

    const auto changed = request(
        caller, "fs_write", R"json({"content":"x","path":"new.txt"})json", 10U);
    const auto blockedBudget = take(guard->beforeInvoke(
        changed, descriptor(changed), context(changed, 10U)));
    REQUIRE(blockedBudget.immediateOutcome.has_value());
    REQUIRE(payload(*blockedBudget.immediateOutcome).at("code") ==
            "context_budget_exceeded");
    REQUIRE(payload(*blockedBudget.immediateOutcome).at("handoff_id") ==
            "handoff-2");
    REQUIRE(payload(*blockedBudget.immediateOutcome).at("resume_seed") ==
            "resume-seed-2");

    const auto missingResume = request(
        caller, "context_get", R"json({})json", 11U);
    const auto missingResumeOutcome = take(execute(
        *guard,
        missingResume,
        context(missingResume, 11U)));
    REQUIRE(missingResumeOutcome.receipt.ok);
    REQUIRE(!payload(missingResumeOutcome).contains(
        "context_budget_cleared"));

    const auto stillBlocked = request(
        caller, "fs_write", R"json({"content":"x","path":"new.txt"})json", 12U);
    const auto stillBlockedAdmission = take(guard->beforeInvoke(
        stillBlocked, descriptor(stillBlocked), context(stillBlocked, 12U)));
    REQUIRE(stillBlockedAdmission.immediateOutcome.has_value());
    REQUIRE(payload(*stillBlockedAdmission.immediateOutcome).at("code") ==
            "context_budget_exceeded");

    const auto staleResume = request(
        caller, "context_get", R"json({})json", 13U);
    const auto staleOutcome = take(execute(
        *guard,
        staleResume,
        context(staleResume, 13U),
        Domain::ContextRecoveryReceipt{
            caller,
            id<Domain::LegacyHandoffId>("handoff-1"),
            take(Domain::PathText::create("D:/stale/recovery")),
            {take(Domain::PathText::create(
                "D:/stale/recovery/file.txt"))}}));
    REQUIRE(staleOutcome.receipt.ok);
    REQUIRE(payload(staleOutcome).at("context_budget_cleared") == false);
    REQUIRE(guard->snapshot(caller).implicitRoots.empty());

    const auto blockedAfterStale = request(
        caller, "fs_write", R"json({"content":"x","path":"new.txt"})json", 14U);
    REQUIRE(take(guard->beforeInvoke(
        blockedAfterStale,
        descriptor(blockedAfterStale),
        context(blockedAfterStale, 14U))).immediateOutcome.has_value());

    const auto expiredResume = request(
        caller, "context_get", R"json({})json", 15U);
    const auto expiredContext = context(expiredResume, 15U);
    const auto expiredAdmission = take(guard->beforeInvoke(
        expiredResume, descriptor(expiredResume), expiredContext));
    REQUIRE(!expiredAdmission.immediateOutcome.has_value());
    clock.setNow(Domain::MonotonicTimePoint{1'001s});
    const auto expiredOutcome = take(guard->afterInvoke(
        expiredResume,
        descriptor(expiredResume),
        Domain::Result<Domain::ToolCallOutcome>::success(successOutcome(
            expiredResume,
            Domain::ContextRecoveryReceipt{
                caller,
                id<Domain::LegacyHandoffId>("handoff-2")})),
        expiredContext));
    REQUIRE(!payload(expiredOutcome).contains("context_budget_cleared"));
    clock.setNow(Domain::MonotonicTimePoint{1s});

    const auto blockedAfterExpiry = request(
        caller, "fs_write", R"json({"content":"x","path":"new.txt"})json", 16U);
    REQUIRE(take(guard->beforeInvoke(
        blockedAfterExpiry,
        descriptor(blockedAfterExpiry),
        context(blockedAfterExpiry, 16U))).immediateOutcome.has_value());

    const auto malformedResume = request(
        caller, "context_get", R"json({})json", 19U);
    const auto malformedContext = context(malformedResume, 19U);
    REQUIRE(!take(guard->beforeInvoke(
        malformedResume,
        descriptor(malformedResume),
        malformedContext)).immediateOutcome);
    auto malformedOutcome = successOutcome(
        malformedResume,
        Domain::ContextRecoveryReceipt{
            caller,
            id<Domain::LegacyHandoffId>("handoff-2"),
            take(Domain::PathText::create("D:/malformed/recovery")),
            {}});
    malformedOutcome.canonicalPayload = R"json({"ok":true})json";
    malformedOutcome.canonicalPayload.push_back('\0');
    const auto rejectedMalformed = guard->afterInvoke(
        malformedResume,
        descriptor(malformedResume),
        Domain::Result<Domain::ToolCallOutcome>::success(
            std::move(malformedOutcome)),
        malformedContext);
    REQUIRE(!rejectedMalformed);
    REQUIRE(rejectedMalformed.error().code ==
            Domain::ErrorCodes::IntegrityFailure);
    REQUIRE(guard->snapshot(caller).blocked);
    REQUIRE(guard->snapshot(caller).implicitRoots.empty());

    const auto resume = request(
        caller, "context_get", R"json({})json", 17U);
    const auto resumeOutcome = take(execute(
        *guard,
        resume,
        context(resume, 17U),
        Domain::ContextRecoveryReceipt{
            caller,
            id<Domain::LegacyHandoffId>("handoff-2")}));
    REQUIRE(resumeOutcome.receipt.ok);
    REQUIRE(payload(resumeOutcome).at("context_budget_cleared") == true);

    const auto resumed = request(
        caller, "fs_write", R"json({"content":"x","path":"new.txt"})json", 18U);
    REQUIRE(take(execute(*guard, resumed, context(resumed, 18U))).receipt.ok);
    REQUIRE(guard->pendingCallCount() == 0U);
}

void successfulProgressCheckpointsThenHandsOff()
{
    LegacyContinuityFake continuity;
    FixedHasher hasher;
    FixedClock clock;
    Mcp::McpInvocationGuardPolicy policy;
    policy.checkpointProgressCount = 2U;
    policy.handoffProgressCount = 4U;
    auto guard = take(Mcp::McpInvocationGuard::create(
        continuity, hasher, clock, policy));
    const auto caller = client("progress-client");
    const auto root = take(Domain::PathText::create("D:/workspace/progress"));

    const auto initialStatus = guard->snapshot(caller);
    REQUIRE(initialStatus.enabled);
    REQUIRE(initialStatus.checkpointEveryTools == 2U);
    REQUIRE(initialStatus.handoffEveryTools == 4U);
    REQUIRE(initialStatus.progressCount == 0U);
    REQUIRE(!initialStatus.blocked);
    REQUIRE(!initialStatus.handoffId);
    REQUIRE(initialStatus.implicitRoots.empty());

    for (std::uint64_t index = 1U; index <= 4U; ++index) {
        const auto call = request(
            caller,
            "fs_read",
            "{\"path\":\"D:/workspace/progress/file-" +
                std::to_string(index) + ".txt\"}",
            100U + index);
        const auto result = take(execute(
            *guard,
            call,
            context(call, 100U + index),
            std::nullopt,
            observedPath(
                "D:/workspace/progress/file-" +
                std::to_string(index) + ".txt")));
        if (index == 2U) {
            REQUIRE(payload(result).at("auto_continuity") == "checkpoint");
        }
        if (index == 4U) {
            REQUIRE(payload(result).at("auto_continuity") == "handoff");
            REQUIRE(payload(result).at("handoff_required") == true);
        }
    }
    REQUIRE(continuity.automaticCalls() == 2U);
    REQUIRE(!continuity.automaticRequests().front().finalize);
    REQUIRE(continuity.automaticRequests().back().finalize);
    REQUIRE(continuity.automaticRequests().front().inferred.keyFiles.has_value());
    REQUIRE(continuity.automaticRequests().front().inferred.narrative.has_value());

    const auto blockedStatus = guard->snapshot(caller);
    REQUIRE(blockedStatus.enabled);
    REQUIRE(blockedStatus.checkpointEveryTools == 2U);
    REQUIRE(blockedStatus.handoffEveryTools == 4U);
    REQUIRE(blockedStatus.progressCount == 4U);
    REQUIRE(blockedStatus.blocked);
    REQUIRE(blockedStatus.handoffId == std::optional<std::string>{"handoff-2"});
    REQUIRE(blockedStatus.implicitRoots == std::vector<Domain::PathText>{root});

    const auto blocked = request(
        caller, "git_status", R"json({})json", 105U);
    const auto admission = take(guard->beforeInvoke(
        blocked, descriptor(blocked), context(blocked, 105U)));
    REQUIRE(admission.immediateOutcome.has_value());
    REQUIRE(payload(*admission.immediateOutcome).at("code") ==
            "context_budget_exceeded");

    const auto allowedResumeTool = request(
        caller, "memory_get", R"json({"key":"continuity/latest"})json", 106U);
    REQUIRE(take(execute(
        *guard,
        allowedResumeTool,
        context(allowedResumeTool, 106U))).receipt.ok);

    const auto resume = request(
        caller, "context_get", R"json({})json", 107U);
    const auto resumeOutcome = take(execute(
        *guard,
        resume,
        context(resume, 107U),
        Domain::ContextRecoveryReceipt{
            caller,
            id<Domain::LegacyHandoffId>("handoff-2"),
            take(Domain::PathText::create("D:/recovered/project")),
            {take(Domain::PathText::create(
                "D:/recovered/project/src/main.cpp"))}}));
    REQUIRE(payload(resumeOutcome).at("context_budget_cleared") == true);

    const auto resumedStatus = guard->snapshot(caller);
    REQUIRE(resumedStatus.progressCount == 0U);
    REQUIRE(!resumedStatus.blocked);
    REQUIRE(resumedStatus.handoffId == std::optional<std::string>{"handoff-2"});
    const std::vector<Domain::PathText> recoveredRoots{
        root,
        take(Domain::PathText::create("D:/recovered/project")),
        take(Domain::PathText::create("D:/recovered/project/src"))};
    REQUIRE(resumedStatus.implicitRoots == recoveredRoots);

    for (std::uint64_t index = 1U; index <= 2U; ++index) {
        const auto memory = request(
            caller,
            "memory_set",
            "{\"key\":\"resumed-" + std::to_string(index) +
                "\",\"value\":\"ready\"}",
            107U + index);
        REQUIRE(take(execute(
            *guard,
            memory,
            context(memory, 107U + index))).receipt.ok);
    }
    REQUIRE(continuity.automaticCalls() == 3U);
    REQUIRE(continuity.automaticRequests().back().inferred.workingDirectory ==
            std::optional<std::string>{"D:/recovered/project"});
    const auto& recoveredKeyFiles =
        continuity.automaticRequests().back().inferred.keyFiles;
    REQUIRE(recoveredKeyFiles.has_value());
    REQUIRE(std::find(
                recoveredKeyFiles->begin(),
                recoveredKeyFiles->end(),
                "D:/recovered/project/src/main.cpp") !=
            recoveredKeyFiles->end());
}

void failuresCancellationBoundsAndShutdownAreSafe()
{
    LegacyContinuityFake continuity;
    continuity.failBudget = true;
    FixedHasher hasher;
    FixedClock clock;
    Mcp::McpInvocationGuardPolicy policy;
    policy.softIdenticalCallCount = 2U;
    policy.hardIdenticalCallCount = 3U;
    policy.checkpointProgressCount = 1U;
    policy.handoffProgressCount = 2U;
    auto guard = take(Mcp::McpInvocationGuard::create(
        continuity, hasher, clock, policy));
    const auto caller = client("failure-client");

    for (std::uint64_t index = 1U; index <= 2U; ++index) {
        const auto call = request(
            caller, "forge_status", R"json({})json", 200U + index);
        const auto result = take(execute(
            *guard, call, context(call, 200U + index)));
        REQUIRE(result.receipt.ok);
    }
    const auto third = request(
        caller, "forge_status", R"json({})json", 203U);
    const auto hard = take(guard->beforeInvoke(
        third, descriptor(third), context(third, 203U)));
    REQUIRE(hard.immediateOutcome.has_value());
    REQUIRE(payload(*hard.immediateOutcome).at("code") ==
            "continuity_persistence_failed");

    const auto changed = request(
        caller, "git_diff", R"json({"staged":false})json", 204U);
    const auto changedAdmission = take(guard->beforeInvoke(
        changed, descriptor(changed), context(changed, 204U)));
    REQUIRE(!changedAdmission.immediateOutcome.has_value());
    guard->cancel(context(changed, 204U).operationId);
    REQUIRE(guard->pendingCallCount() == 0U);

    const auto mismatchedCall = request(
        caller,
        "fs_read",
        R"json({"path":"D:/mismatched/file.txt"})json",
        207U);
    const auto mismatchedContext = context(mismatchedCall, 207U);
    REQUIRE(!take(guard->beforeInvoke(
        mismatchedCall,
        descriptor(mismatchedCall),
        mismatchedContext)).immediateOutcome);
    auto mismatchedOutcome = successOutcome(
        mismatchedCall,
        std::nullopt,
        observedPath("D:/mismatched/file.txt"));
    mismatchedOutcome.receipt.requestId =
        id<Domain::RequestId>("request-not-this-call");
    const auto rejectedMismatch = guard->afterInvoke(
        mismatchedCall,
        descriptor(mismatchedCall),
        Domain::Result<Domain::ToolCallOutcome>::success(
            std::move(mismatchedOutcome)),
        mismatchedContext);
    REQUIRE(!rejectedMismatch);
    REQUIRE(rejectedMismatch.error().code ==
            Domain::ErrorCodes::IntegrityFailure);
    REQUIRE(guard->snapshot(caller).progressCount == 0U);
    REQUIRE(guard->snapshot(caller).implicitRoots.empty());

    std::stop_source cancelled;
    cancelled.request_stop();
    const auto cancelledCall = request(
        caller, "git_log", R"json({})json", 205U);
    const auto cancelledResult = guard->beforeInvoke(
        cancelledCall,
        descriptor(cancelledCall),
        context(cancelledCall, 205U, cancelled.get_token()));
    REQUIRE(!cancelledResult);
    REQUIRE(cancelledResult.error().code == Domain::ErrorCodes::Cancelled);

    guard->shutdown();
    const auto afterShutdown = guard->beforeInvoke(
        changed, descriptor(changed), context(changed, 206U));
    REQUIRE(!afterShutdown);
    REQUIRE(afterShutdown.error().code == Domain::ErrorCodes::TransportClosed);

    policy.softIdenticalCallCount = 9U;
    policy.hardIdenticalCallCount = 4U;
    REQUIRE(!Mcp::McpInvocationGuard::create(
        continuity, hasher, clock, policy));
}

void concurrentThresholdCrossingCoalescesPersistence()
{
    LegacyContinuityFake continuity;
    FixedHasher hasher;
    FixedClock clock;
    Mcp::McpInvocationGuardPolicy policy;
    policy.checkpointProgressCount = 2U;
    policy.handoffProgressCount = 100U;
    auto guard = take(Mcp::McpInvocationGuard::create(
        continuity, hasher, clock, policy));
    const auto caller = client("concurrent-progress-client");

    const auto first = request(
        caller, "fs_read", R"json({"path":"first.txt"})json", 300U);
    REQUIRE(take(execute(
        *guard, first, context(first, 300U))).receipt.ok);
    REQUIRE(continuity.automaticCalls() == 0U);

    continuity.blockAutomaticPersistence();
    const auto threshold = request(
        caller, "fs_read", R"json({"path":"threshold.txt"})json", 301U);
    const auto overlapping = request(
        caller, "fs_read", R"json({"path":"overlapping.txt"})json", 302U);
    std::optional<Domain::Result<Domain::ToolCallOutcome>> thresholdResult;
    std::optional<Domain::Result<Domain::ToolCallOutcome>> overlappingResult;
    std::jthread thresholdWorker{[&] {
        thresholdResult.emplace(execute(
            *guard, threshold, context(threshold, 301U)));
    }};
    REQUIRE(continuity.waitForAutomaticPersistence());
    std::jthread overlappingWorker{[&] {
        overlappingResult.emplace(execute(
            *guard, overlapping, context(overlapping, 302U)));
    }};
    overlappingWorker.join();
    REQUIRE(overlappingResult.has_value());
    REQUIRE(overlappingResult->hasValue());
    REQUIRE(continuity.automaticCalls() == 1U);

    continuity.releaseAutomaticPersistence();
    thresholdWorker.join();
    REQUIRE(thresholdResult.has_value());
    REQUIRE(thresholdResult->hasValue());
    REQUIRE(continuity.automaticCalls() == 1U);
    REQUIRE(payload(thresholdResult->value()).at("auto_continuity") ==
            "checkpoint");
    REQUIRE(!payload(overlappingResult->value()).contains(
        "auto_continuity"));
    REQUIRE(guard->pendingCallCount() == 0U);
}

void implicitRootsAreAuthorizedAndBounded()
{
    LegacyContinuityFake continuity;
    FixedHasher hasher;
    FixedClock clock;
    Mcp::McpInvocationGuardPolicy policy;
    policy.softIdenticalCallCount = 999'999U;
    policy.hardIdenticalCallCount = 1'000'000U;
    policy.checkpointProgressCount = 1'000U;
    policy.handoffProgressCount = 1'000U;
    auto guard = take(Mcp::McpInvocationGuard::create(
        continuity, hasher, clock, policy));
    const auto caller = client("implicit-root-client");

    for (std::uint64_t index = 1U; index <= 18U; ++index) {
        const auto call = request(
            caller,
            "fs_read",
            "{\"path\":\"D:/bounded/root-" +
                std::to_string(index) + "/file.txt\"}",
            400U + index);
        REQUIRE(take(execute(
            *guard,
            call,
            context(call, 400U + index),
            std::nullopt,
            observedPath(
                "D:/bounded/root-" + std::to_string(index) +
                "/file.txt"))).receipt.ok);
    }

    auto status = guard->snapshot(caller);
    REQUIRE(status.progressCount == 18U);
    REQUIRE(status.implicitRoots.size() ==
            Domain::MaximumContinuityAutomationImplicitRoots);
    REQUIRE(status.implicitRoots.front() ==
            take(Domain::PathText::create("D:/bounded/root-3")));
    REQUIRE(status.implicitRoots.back() ==
            take(Domain::PathText::create("D:/bounded/root-18")));

    const auto duplicate = request(
        caller,
        "fs_read",
        R"json({"path":"D:/bounded/root-18/another.txt"})json",
        419U);
    REQUIRE(take(execute(
        *guard,
        duplicate,
        context(duplicate, 419U),
        std::nullopt,
        observedPath(
            "D:/bounded/root-18/another.txt"))).receipt.ok);
    status = guard->snapshot(caller);
    REQUIRE(status.progressCount == 19U);
    REQUIRE(status.implicitRoots.size() ==
            Domain::MaximumContinuityAutomationImplicitRoots);
    REQUIRE(status.implicitRoots.front() ==
            take(Domain::PathText::create("D:/bounded/root-3")));

    const auto failed = request(
        caller,
        "fs_read",
        R"json({"path":"D:/bounded/root-19/file.txt"})json",
        420U);
    const auto failedContext = context(failed, 420U);
    REQUIRE(!take(guard->beforeInvoke(
        failed, descriptor(failed), failedContext)).immediateOutcome);
    auto failedValue = successOutcome(
        failed,
        std::nullopt,
        observedPath("D:/bounded/root-19/file.txt"));
    failedValue.receipt.ok = false;
    failedValue.receipt.error = Domain::makeError(
        Domain::ErrorCodes::RecordNotFound,
        "The scripted tool failed.");
    const auto failedOutcome = guard->afterInvoke(
        failed,
        descriptor(failed),
        Domain::Result<Domain::ToolCallOutcome>::success(
            std::move(failedValue)),
        failedContext);
    REQUIRE(failedOutcome);
    REQUIRE(!failedOutcome.value().receipt.ok);

    status = guard->snapshot(caller);
    REQUIRE(status.progressCount == 19U);
    REQUIRE(status.implicitRoots.size() ==
            Domain::MaximumContinuityAutomationImplicitRoots);
    REQUIRE(status.implicitRoots.front() ==
            take(Domain::PathText::create("D:/bounded/root-4")));
    REQUIRE(status.implicitRoots.back() ==
            take(Domain::PathText::create("D:/bounded/root-19")));

    const auto continuityCall = request(
        caller,
        "session_checkpoint",
        R"json({"cwd":"D:/ignored/root"})json",
        421U);
    REQUIRE(take(execute(
        *guard,
        continuityCall,
        context(continuityCall, 421U),
        std::nullopt,
        observedWorkingDirectory("D:/ignored/root"))).receipt.ok);
    REQUIRE(guard->snapshot(caller).implicitRoots == status.implicitRoots);
}

void continuityCapacityRejectsSaturationAndRetainsBlockedStates()
{
    LegacyContinuityFake continuity;
    continuity.blockAutomaticPersistence();
    FixedHasher hasher;
    FixedClock clock;
    Mcp::McpInvocationGuardPolicy policy;
    policy.softIdenticalCallCount = 999'999U;
    policy.hardIdenticalCallCount = 1'000'000U;
    policy.checkpointProgressCount = 1U;
    policy.handoffProgressCount = 1U;
    auto guard = take(Mcp::McpInvocationGuard::create(
        continuity, hasher, clock, policy));

    const auto capacity =
        Mcp::McpInvocationGuard::MaximumTrackedContinuityClients;
    std::vector<Domain::ClientId> clients;
    std::vector<std::optional<Domain::Result<Domain::ToolCallOutcome>>>
        results(capacity);
    std::vector<std::jthread> workers;
    clients.reserve(capacity);
    workers.reserve(capacity);

    for (std::size_t index = 0U; index < capacity; ++index) {
        clients.push_back(client(
            "saturation-client-" + std::to_string(index + 1U)));
        const auto call = request(
            clients.back(),
            "fs_read",
            "{\"path\":\"D:/saturation/file-" +
                std::to_string(index + 1U) + ".txt\"}",
            2'000U + index);
        workers.emplace_back([&, index, call] {
            results[index].emplace(execute(
                *guard,
                call,
                context(call, 2'000U + index),
                std::nullopt,
                observedPath(
                    "D:/saturation/file-" +
                    std::to_string(index + 1U) + ".txt")));
        });
        REQUIRE(continuity.waitForAutomaticPersistence(index + 1U));
        REQUIRE(guard->trackedContinuityClientCount() == index + 1U);
    }

    const auto overflowClient = client("saturation-client-129");
    const auto overflowCall = request(
        overflowClient,
        "fs_read",
        R"json({"path":"D:/saturation/overflow.txt"})json",
        2'200U);
    const auto overflow = execute(
        *guard,
        overflowCall,
        context(overflowCall, 2'200U),
        std::nullopt,
        observedPath("D:/saturation/overflow.txt"));
    REQUIRE(!overflow);
    REQUIRE(overflow.error().code == Domain::ErrorCodes::LimitExceeded);
    REQUIRE(overflow.error().retryable);
    REQUIRE(guard->trackedContinuityClientCount() == capacity);
    REQUIRE(continuity.automaticCalls() == capacity);

    continuity.releaseAutomaticPersistence();
    for (auto& worker : workers) {
        worker.join();
    }
    for (const auto& result : results) {
        REQUIRE(result.has_value());
        REQUIRE(result->hasValue());
        REQUIRE(result->value().receipt.ok);
    }
    REQUIRE(guard->trackedContinuityClientCount() == capacity);

    const auto firstStatus = guard->snapshot(clients.front());
    REQUIRE(firstStatus.blocked);
    REQUIRE(firstStatus.handoffId.has_value());
    const auto blockedCall = request(
        clients.front(), "git_status", R"json({})json", 2'201U);
    const auto blockedAdmission = take(guard->beforeInvoke(
        blockedCall,
        descriptor(blockedCall),
        context(blockedCall, 2'201U)));
    REQUIRE(blockedAdmission.immediateOutcome.has_value());
    REQUIRE(payload(*blockedAdmission.immediateOutcome).at("code") ==
            "context_budget_exceeded");

    const auto stillFull = execute(
        *guard,
        overflowCall,
        context(overflowCall, 2'202U),
        std::nullopt,
        observedPath("D:/saturation/overflow.txt"));
    REQUIRE(!stillFull);
    REQUIRE(stillFull.error().code == Domain::ErrorCodes::LimitExceeded);
    REQUIRE(guard->trackedContinuityClientCount() == capacity);

    const auto recovery = request(
        clients.front(), "context_get", R"json({})json", 2'203U);
    const auto recoveryOutcome = take(execute(
        *guard,
        recovery,
        context(recovery, 2'203U),
        Domain::ContextRecoveryReceipt{
            clients.front(),
            id<Domain::LegacyHandoffId>(*firstStatus.handoffId)}));
    REQUIRE(payload(recoveryOutcome).at("context_budget_cleared") == true);
    REQUIRE(!guard->snapshot(clients.front()).blocked);

    const auto admitted = take(execute(
        *guard,
        overflowCall,
        context(overflowCall, 2'204U),
        std::nullopt,
        observedPath("D:/saturation/overflow.txt")));
    REQUIRE(admitted.receipt.ok);
    REQUIRE(guard->trackedContinuityClientCount() == capacity);
}

void clientStateIsDeterministicallyBounded()
{
    LegacyContinuityFake continuity;
    FixedHasher hasher;
    FixedClock clock;
    Mcp::McpInvocationGuardPolicy policy;
    policy.checkpointProgressCount = 1U;
    policy.handoffProgressCount = 1'000U;
    auto guard = take(Mcp::McpInvocationGuard::create(
        continuity, hasher, clock, policy));

    for (std::uint64_t index = 1U; index <= 257U; ++index) {
        const auto caller = client("bounded-client-" + std::to_string(index));
        const auto call = request(
            caller,
            "fs_read",
            "{\"path\":\"file-" + std::to_string(index) + ".txt\"}",
            1'000U + index);
        REQUIRE(take(execute(
            *guard, call, context(call, 1'000U + index))).receipt.ok);
    }
    REQUIRE(guard->trackedLoopClientCount() ==
            Mcp::McpInvocationGuard::MaximumTrackedLoopClients);
    REQUIRE(guard->trackedContinuityClientCount() ==
            Mcp::McpInvocationGuard::MaximumTrackedContinuityClients);
    REQUIRE(continuity.automaticCalls() == 257U);
}

} // namespace

int main()
{
    try {
        static_assert(std::is_final_v<Mcp::McpInvocationGuard>);
        static_assert(std::is_base_of_v<
                      Contracts::IContinuityAutomationStatusSource,
                      Mcp::McpInvocationGuard>);
        identicalCallsSoftHandoffHardBlockAndResume();
        successfulProgressCheckpointsThenHandsOff();
        failuresCancellationBoundsAndShutdownAreSafe();
        concurrentThresholdCrossingCoalescesPersistence();
        implicitRootsAreAuthorizedAndBounded();
        continuityCapacityRejectsSaturationAndRetainsBlockedStates();
        clientStateIsDeterministicallyBounded();
        std::cout << "MCP invocation guard tests passed.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
