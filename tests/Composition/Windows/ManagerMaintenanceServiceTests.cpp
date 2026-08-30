#include "ManagerMaintenanceService.h"

#include "ForgeConductor/Contracts/IAgentServices.h"
#include "ForgeConductor/Contracts/IContinuityCoordinator.h"
#include "ForgeConductor/Contracts/IFoundationServices.h"
#include "ForgeConductor/Contracts/ILMStudioDeploymentService.h"
#include "ForgeConductor/Contracts/IToolServices.h"
#include "Fakes/ApplicationServiceFakes.h"
#include "Fakes/DeterministicWorkspaceAuthority.h"
#include "Fakes/ExternalServiceFakes.h"
#include "Fakes/FoundationFakes.h"
#include "Fakes/RecordingContinuityCoordinator.h"
#include "Fakes/ToolServiceFakes.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <future>
#include <iostream>
#include <mutex>
#include <optional>
#include <source_location>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

namespace {

namespace Composition = ForgeConductor::Composition::Windows;
namespace Contracts = ForgeConductor::Contracts;
namespace Domain = ForgeConductor::Domain;
namespace Fakes = ForgeConductor::Tests::Fakes;

using namespace std::chrono_literals;

static_assert(std::is_final_v<Composition::ManagerMaintenanceService>);
static_assert(std::is_base_of_v<
              Contracts::IManagerMaintenanceService,
              Composition::ManagerMaintenanceService>);
static_assert(!std::is_copy_constructible_v<
              Composition::ManagerMaintenanceService>);
static_assert(!std::is_move_constructible_v<
              Composition::ManagerMaintenanceService>);

void require(
    const bool condition,
    const std::string_view message,
    const std::source_location location = std::source_location::current())
{
    if (!condition) {
        throw std::runtime_error{
            std::string{message} + " at " + location.file_name() + ':' +
            std::to_string(location.line())};
    }
}

template <typename Value>
[[nodiscard]] Value take(Domain::Result<Value> result)
{
    if (!result) {
        throw std::runtime_error{
            result.error().code + ": " + result.error().message};
    }
    return std::move(result).value();
}

template <typename Value>
void requireError(
    const Domain::Result<Value>& result,
    const std::string_view expectedCode,
    const std::string_view message)
{
    require(!result, message);
    require(result.error().code == expectedCode, message);
}

template <typename Identifier>
[[nodiscard]] Identifier parse(const std::string_view value)
{
    return take(Identifier::parse(value));
}

template <typename Value>
[[nodiscard]] Domain::Result<Value> success(Value value)
{
    return Domain::Result<Value>::success(std::move(value));
}

template <typename Value>
[[nodiscard]] Domain::Result<Value> failure(
    const std::string_view code,
    const std::string_view message,
    const bool retryable = false)
{
    return Domain::Result<Value>::failure(
        Domain::makeError(code, std::string{message}, retryable));
}

[[nodiscard]] Domain::MonotonicTimePoint testNow() noexcept
{
    return Domain::MonotonicTimePoint{} + 100s;
}

[[nodiscard]] Domain::PathText path(const std::string_view value)
{
    return take(Domain::PathText::create(value));
}

[[nodiscard]] Domain::PathText preferredBinary()
{
    return path("C:\\Forge\\bin\\forge-conductor.exe");
}

[[nodiscard]] Domain::OperationContext context(
    const std::string_view operationId =
        "11111111-1111-4111-8111-111111111111",
    const std::string_view correlationId = "manager-maintenance",
    const Domain::MonotonicTimePoint deadline = testNow() + 1min,
    const std::stop_token cancellation = {})
{
    return Domain::OperationContext{
        parse<Domain::OperationId>(operationId),
        deadline,
        cancellation,
        parse<Domain::CorrelationId>(correlationId)};
}

[[nodiscard]] std::vector<Domain::FileAccess> grantsFor(
    const Domain::FileAccess intent)
{
    if (intent == Domain::FileAccess::Read) {
        return {Domain::FileAccess::Read};
    }
    if (intent == Domain::FileAccess::Write) {
        return {
            Domain::FileAccess::Read,
            Domain::FileAccess::Write,
            Domain::FileAccess::Create,
            Domain::FileAccess::Delete,
            Domain::FileAccess::Execute};
    }
    return {intent};
}

[[nodiscard]] std::vector<Domain::FileAccess> denialsFor(
    const Domain::FileAccess intent)
{
    std::vector<Domain::FileAccess> denials;
    const auto grants = grantsFor(intent);
    for (const auto access : {
             Domain::FileAccess::Read,
             Domain::FileAccess::Write,
             Domain::FileAccess::Create,
             Domain::FileAccess::Delete,
             Domain::FileAccess::Execute}) {
        if (std::find(grants.begin(), grants.end(), access) == grants.end()) {
            denials.push_back(access);
        }
    }
    return denials;
}

[[nodiscard]] Contracts::WorkspaceAuthority authority(
    const Domain::FileAccess intent,
    const std::string_view projectId =
        "22222222-2222-4222-8222-222222222222",
    const std::string_view callerId = "manager-maintenance",
    const bool shellEnabled = false,
    const std::string_view authorityId =
        "33333333-3333-4333-8333-333333333333",
    const std::uint64_t generation = 7U)
{
    Fakes::DeterministicWorkspaceAuthority issuer{
        parse<Domain::AuthorityId>(authorityId),
        parse<Domain::ClientId>(callerId),
        {path("C:\\Forge")},
        intent,
        grantsFor(intent),
        denialsFor(intent),
        shellEnabled,
        generation};
    issuer.setNow(testNow());
    return take(issuer.authorityFor(
        parse<Domain::ProjectId>(projectId), context()));
}

[[nodiscard]] Domain::DeploymentId deploymentId()
{
    return parse<Domain::DeploymentId>("manager-maintenance-deployment");
}

[[nodiscard]] Domain::LMStudioPluginStatus pluginStatus(
    const bool lmStudioPresent,
    const bool primaryInstalled,
    const bool fallbackInstalled,
    const bool configurationRegistered,
    const bool binaryExecutable,
    std::optional<Domain::DeploymentId> installedDeployment)
{
    return Domain::LMStudioPluginStatus{
        primaryInstalled,
        fallbackInstalled,
        configurationRegistered,
        preferredBinary(),
        binaryExecutable,
        lmStudioPresent,
        path("C:\\Forge\\lmstudio\\primary"),
        path("C:\\Forge\\lmstudio\\fallback"),
        path("C:\\Forge\\lmstudio\\mcp.json"),
        std::move(installedDeployment),
        lmStudioPresent ? "present" : "absent"};
}

[[nodiscard]] Domain::LMStudioPluginStatus healthyPluginStatus()
{
    return pluginStatus(true, true, true, true, true, deploymentId());
}

[[nodiscard]] Domain::LMStudioPluginStatus driftedPluginStatus()
{
    return pluginStatus(true, false, true, true, true, deploymentId());
}

[[nodiscard]] Domain::LMStudioPluginStatus absentPluginStatus()
{
    return pluginStatus(false, false, false, false, false, std::nullopt);
}

[[nodiscard]] Domain::LMStudioInstallResult completeInstallResult()
{
    return Domain::LMStudioInstallResult{
        true,
        preferredBinary(),
        {path("C:\\Forge\\lmstudio\\primary"),
         path("C:\\Forge\\lmstudio\\fallback")},
        path("C:\\Forge\\lmstudio\\mcp.json"),
        deploymentId(),
        "installed"};
}

[[nodiscard]] bool sameAuthority(
    const Contracts::WorkspaceAuthority& left,
    const Contracts::WorkspaceAuthority& right) noexcept
{
    return left.authorityId() == right.authorityId() &&
        left.projectId() == right.projectId() &&
        left.callerId() == right.callerId() &&
        left.intent() == right.intent() &&
        left.shellEnabled() == right.shellEnabled() &&
        left.generation() == right.generation();
}

class BlockingRecordingLMStudioDeploymentService final
    : public Contracts::ILMStudioDeploymentService {
public:
    Fakes::DeterministicResult<Domain::LMStudioPluginStatus> statusResult;
    Fakes::DeterministicResult<Domain::LMStudioInstallResult> deployResult;
    Fakes::DeterministicResult<Domain::LMStudioHostActivationResult>
        activateResult;

    [[nodiscard]] Domain::Result<Domain::LMStudioPluginStatus> status(
        const Domain::LMStudioDeploymentRequest& request,
        const Contracts::WorkspaceAuthority& readAuthority,
        const Domain::OperationContext& operationContext) noexcept override
    {
        try {
            std::stop_source* cancelSource{};
            Fakes::FakeClock* clockToAdvance{};
            std::chrono::milliseconds clockAdvance{};
            {
                std::unique_lock lock{mutex_};
                ++statusCalls_;
                lastStatusRequest_ = request;
                lastStatusAuthority_.emplace(readAuthority);
                lastStatusContext_ = operationContext;
                statusEntered_ = true;
                changed_.notify_all();
                changed_.wait(
                    lock, [this]() noexcept { return !blockStatus_; });
                cancelSource = cancelDuringStatus_;
                cancelDuringStatus_ = nullptr;
                clockToAdvance = advanceClockDuringStatus_;
                advanceClockDuringStatus_ = nullptr;
                clockAdvance = clockAdvance_;
                clockAdvance_ = 0ms;
            }
            if (cancelSource != nullptr) {
                static_cast<void>(cancelSource->request_stop());
            }
            if (clockToAdvance != nullptr) {
                clockToAdvance->advance(clockAdvance);
            }
            return statusResult.get();
        } catch (...) {
            return failure<Domain::LMStudioPluginStatus>(
                Domain::ErrorCodes::InternalFailure,
                "The recording LM Studio status failed safely.");
        }
    }

    [[nodiscard]] Domain::Result<Domain::LMStudioInstallResult> deploy(
        const Domain::LMStudioDeploymentRequest& request,
        const Contracts::WorkspaceAuthority& writeAuthority,
        const Contracts::AuthorizedToolCall& authorization,
        const Domain::OperationContext& operationContext) noexcept override
    {
        try {
            {
                std::lock_guard lock{mutex_};
                ++deployCalls_;
                lastDeployRequest_ = request;
                lastDeployAuthority_.emplace(writeAuthority);
                lastDeployAuthorization_.emplace(authorization);
                lastDeployContext_ = operationContext;
            }
            if (writeAuthority.intent() != Domain::FileAccess::Write ||
                authorization.effect() != Domain::ToolEffect::Write ||
                !authorization.matches(writeAuthority, operationContext) ||
                !authorization.matchesProject(writeAuthority.projectId())) {
                return failure<Domain::LMStudioInstallResult>(
                    Domain::ErrorCodes::Unauthorized,
                    "The recording LM Studio deployment capability was mismatched.");
            }
            return deployResult.get();
        } catch (...) {
            return failure<Domain::LMStudioInstallResult>(
                Domain::ErrorCodes::InternalFailure,
                "The recording LM Studio deployment failed safely.");
        }
    }

    [[nodiscard]] Domain::Result<Domain::LMStudioHostActivationResult> activate(
        const Domain::LMStudioHostActivationRequest&,
        const Contracts::WorkspaceAuthority&,
        const Contracts::AuthorizedToolCall&,
        const Domain::OperationContext&) noexcept override
    {
        std::lock_guard lock{mutex_};
        ++activateCalls_;
        return activateResult.get();
    }

    void cancel(const Domain::OperationId&) noexcept override {}

    void shutdown() noexcept override
    {
        try {
            {
                std::lock_guard lock{mutex_};
                blockStatus_ = false;
            }
            changed_.notify_all();
        } catch (...) {
            changed_.notify_all();
        }
    }

    void blockNextStatus()
    {
        std::lock_guard lock{mutex_};
        blockStatus_ = true;
        statusEntered_ = false;
    }

    [[nodiscard]] bool waitUntilStatusBlocked(
        const std::chrono::milliseconds timeout = 2s) const noexcept
    {
        try {
            std::unique_lock lock{mutex_};
            return changed_.wait_for(
                lock, timeout, [this]() noexcept {
                    return blockStatus_ && statusEntered_;
                });
        } catch (...) {
            return false;
        }
    }

    void releaseStatus() noexcept
    {
        try {
            {
                std::lock_guard lock{mutex_};
                blockStatus_ = false;
            }
            changed_.notify_all();
        } catch (...) {
            changed_.notify_all();
        }
    }

    void cancelOnNextStatus(std::stop_source& source) noexcept
    {
        std::lock_guard lock{mutex_};
        cancelDuringStatus_ = &source;
    }

    void advanceClockOnNextStatus(
        Fakes::FakeClock& clock,
        const std::chrono::milliseconds by) noexcept
    {
        std::lock_guard lock{mutex_};
        advanceClockDuringStatus_ = &clock;
        clockAdvance_ = by;
    }

    [[nodiscard]] std::size_t statusCalls() const noexcept
    {
        std::lock_guard lock{mutex_};
        return statusCalls_;
    }

    [[nodiscard]] std::size_t deployCalls() const noexcept
    {
        std::lock_guard lock{mutex_};
        return deployCalls_;
    }

    [[nodiscard]] std::size_t activateCalls() const noexcept
    {
        std::lock_guard lock{mutex_};
        return activateCalls_;
    }

    [[nodiscard]] std::optional<Domain::LMStudioDeploymentRequest>
    lastStatusRequest() const
    {
        std::lock_guard lock{mutex_};
        return lastStatusRequest_;
    }

    [[nodiscard]] std::optional<Domain::LMStudioDeploymentRequest>
    lastDeployRequest() const
    {
        std::lock_guard lock{mutex_};
        return lastDeployRequest_;
    }

    [[nodiscard]] std::optional<Contracts::WorkspaceAuthority>
    lastStatusAuthority() const
    {
        std::lock_guard lock{mutex_};
        return lastStatusAuthority_;
    }

    [[nodiscard]] std::optional<Contracts::WorkspaceAuthority>
    lastDeployAuthority() const
    {
        std::lock_guard lock{mutex_};
        return lastDeployAuthority_;
    }

    [[nodiscard]] std::optional<Contracts::AuthorizedToolCall>
    lastDeployAuthorization() const
    {
        std::lock_guard lock{mutex_};
        return lastDeployAuthorization_;
    }

    [[nodiscard]] std::optional<Domain::OperationContext>
    lastStatusContext() const
    {
        std::lock_guard lock{mutex_};
        return lastStatusContext_;
    }

private:
    mutable std::mutex mutex_;
    mutable std::condition_variable changed_;
    std::optional<Domain::LMStudioDeploymentRequest> lastStatusRequest_;
    std::optional<Domain::LMStudioDeploymentRequest> lastDeployRequest_;
    std::optional<Contracts::WorkspaceAuthority> lastStatusAuthority_;
    std::optional<Contracts::WorkspaceAuthority> lastDeployAuthority_;
    std::optional<Contracts::AuthorizedToolCall> lastDeployAuthorization_;
    std::optional<Domain::OperationContext> lastStatusContext_;
    std::optional<Domain::OperationContext> lastDeployContext_;
    std::stop_source* cancelDuringStatus_{};
    Fakes::FakeClock* advanceClockDuringStatus_{};
    std::chrono::milliseconds clockAdvance_{};
    std::size_t statusCalls_{};
    std::size_t deployCalls_{};
    std::size_t activateCalls_{};
    bool blockStatus_{};
    bool statusEntered_{};
};

struct Fixture final {
    Fixture()
        : clock{Domain::UtcTimePoint{} + 100s, now},
          agentSessions{
              Fakes::DefaultBoundaryCaptureItemsMaximum,
              Fakes::DefaultBoundaryCaptureTextBytesMaximum,
              now},
          toolAuthorizer{
              std::string{Composition::ManagerMaintenanceService::
                              DeploymentToolName},
              Domain::ToolEffect::Write,
              now},
          uuidGenerator{{parse<Domain::Uuid>(
              "44444444-4444-4444-8444-444444444444")}},
          readAuthority{authority(
              Domain::FileAccess::Read,
              "22222222-2222-4222-8222-222222222222",
              "manager-maintenance",
              false,
              "55555555-5555-4555-8555-555555555555",
              5U)},
          writeAuthority{authority(
              Domain::FileAccess::Write,
              "22222222-2222-4222-8222-222222222222",
              "manager-maintenance",
              true,
              "66666666-6666-4666-8666-666666666666",
              8U)},
          service{
              agentSessions,
              continuity,
              lmStudio,
              toolAuthorizer,
              uuidGenerator,
              clock,
              Composition::ManagerMaintenanceServiceConfiguration{
                  preferredBinary(), readAuthority, writeAuthority}}
    {
        continuity.setNow(now);
        agentSessions.pruneStaleResult.set(success<std::size_t>(3U));
        continuity.recoveryResult.set(
            success(Domain::ContinuityRecoveryReport{4U, 4U, 0U, 0U, {}}));
        lmStudio.statusResult.set(success(healthyPluginStatus()));
        lmStudio.deployResult.set(success(completeInstallResult()));
    }

    [[nodiscard]] Domain::OperationContext activeContext(
        const std::string_view operationId =
            "11111111-1111-4111-8111-111111111111",
        const std::string_view correlationId =
            "manager-maintenance") const
    {
        return context(operationId, correlationId, now + 1min);
    }

    const Domain::MonotonicTimePoint now{testNow()};
    Fakes::FakeClock clock;
    Fakes::RecordingAgentSessionServiceFake agentSessions;
    Fakes::RecordingContinuityCoordinator continuity;
    BlockingRecordingLMStudioDeploymentService lmStudio;
    Fakes::DeterministicToolAuthorizerFake toolAuthorizer;
    Fakes::SequenceUuidGenerator uuidGenerator;
    Contracts::WorkspaceAuthority readAuthority;
    Contracts::WorkspaceAuthority writeAuthority;
    Composition::ManagerMaintenanceService service;
};

void healthyPassPrunesRecoversAndInspectsWithoutDeployment()
{
    Fixture fixture;
    const auto operationContext = fixture.activeContext();
    const auto result = fixture.service.reconcile(operationContext);
    require(result.hasValue(), "healthy maintenance pass failed");

    require(
        fixture.agentSessions.calls() == 1U &&
            fixture.agentSessions.lastCapture() &&
            fixture.agentSessions.lastCapture()->call ==
                Fakes::AgentSessionServiceCall::PruneStale,
        "healthy maintenance did not prune stale sessions exactly once");
    require(
        fixture.continuity.callCount(
            Fakes::ContinuityCall::RecoverIncompleteOperations) == 1U &&
            !fixture.continuity.lastProjectId(),
        "healthy maintenance did not recover all-project continuity exactly once");
    require(
        fixture.lmStudio.statusCalls() == 1U &&
            fixture.lmStudio.deployCalls() == 0U &&
            fixture.lmStudio.activateCalls() == 0U,
        "healthy LM Studio maintenance deployed or activated unexpectedly");
    require(
        fixture.toolAuthorizer.calls() == 0U &&
            fixture.uuidGenerator.consumed() == 0U,
        "healthy LM Studio maintenance minted an unused authorization");

    const auto statusRequest = fixture.lmStudio.lastStatusRequest();
    require(
        statusRequest && statusRequest->preferredBinary &&
            statusRequest->preferredBinary.value() == preferredBinary() &&
            statusRequest->preserveForeignEntries,
        "healthy LM Studio inspection changed its preferred binary or preservation policy");
    const auto read = fixture.lmStudio.lastStatusAuthority();
    require(
        read && sameAuthority(read.value(), fixture.readAuthority),
        "healthy LM Studio inspection did not use the configured read authority");
    const auto statusContext = fixture.lmStudio.lastStatusContext();
    require(
        statusContext &&
            statusContext->operationId == operationContext.operationId &&
            statusContext->correlationId == operationContext.correlationId,
        "healthy LM Studio inspection changed the operation context");
}

void exactDriftAuthorizationDeploysTwoPlugins()
{
    Fixture fixture;
    fixture.lmStudio.statusResult.set(success(driftedPluginStatus()));
    const auto operationContext = fixture.activeContext();
    const auto result = fixture.service.reconcile(operationContext);
    require(result.hasValue(), "repairable LM Studio drift was not deployed");

    require(
        fixture.lmStudio.statusCalls() == 1U &&
            fixture.lmStudio.deployCalls() == 1U &&
            fixture.lmStudio.activateCalls() == 0U,
        "LM Studio drift did not perform one exact status/deploy sequence");
    require(
        fixture.uuidGenerator.consumed() == 1U &&
            fixture.toolAuthorizer.calls() == 1U,
        "LM Studio drift did not mint exactly one authorization");

    const auto authorizationRequest = fixture.toolAuthorizer.lastRequest();
    require(authorizationRequest.has_value(), "deployment authorization was not captured");
    const auto& request = authorizationRequest.value();
    require(
        request.call.metadata.requestId == parse<Domain::RequestId>(
                                               "44444444-4444-4444-8444-444444444444") &&
            request.call.metadata.correlationId == operationContext.correlationId &&
            request.call.metadata.clientId == fixture.writeAuthority.callerId() &&
            request.call.metadata.projectId &&
            request.call.metadata.projectId.value() ==
                fixture.writeAuthority.projectId() &&
            request.call.metadata.protocolVersion ==
                Composition::ManagerMaintenanceService::ProtocolVersion,
        "deployment authorization metadata was not exact");
    require(
        request.call.toolName ==
                Composition::ManagerMaintenanceService::DeploymentToolName &&
            request.call.canonicalArguments ==
                Composition::ManagerMaintenanceService::
                    CanonicalDeploymentArguments &&
            request.effect == Domain::ToolEffect::Write &&
            request.authority.authorityId ==
                fixture.writeAuthority.authorityId() &&
            request.authority.generation ==
                fixture.writeAuthority.generation(),
        "deployment tool, arguments, effect, or authority reference changed");

    const auto statusRequest = fixture.lmStudio.lastStatusRequest();
    const auto deployRequest = fixture.lmStudio.lastDeployRequest();
    require(
        statusRequest && deployRequest && statusRequest->preferredBinary &&
            deployRequest->preferredBinary &&
            statusRequest->preferredBinary.value() == preferredBinary() &&
            deployRequest->preferredBinary.value() == preferredBinary() &&
            statusRequest->preserveForeignEntries &&
            deployRequest->preserveForeignEntries,
        "status and deploy did not share the exact preserved deployment request");
    const auto write = fixture.lmStudio.lastDeployAuthority();
    const auto authorized = fixture.lmStudio.lastDeployAuthorization();
    require(
        write && sameAuthority(write.value(), fixture.writeAuthority) &&
            authorized &&
            authorized->matches(fixture.writeAuthority, operationContext) &&
            authorized->effect() == Domain::ToolEffect::Write &&
            authorized->toolName() ==
                Composition::ManagerMaintenanceService::DeploymentToolName,
        "LM Studio deployment did not receive the exact write capability");
    require(
        completeInstallResult().pluginsWritten.size() == 2U,
        "the scripted successful deployment did not contain exactly two plugins");
}

void absentLmStudioDoesNotDeploy()
{
    Fixture fixture;
    fixture.lmStudio.statusResult.set(success(absentPluginStatus()));
    const auto result = fixture.service.reconcile(fixture.activeContext());
    require(result.hasValue(), "absent LM Studio failed the maintenance pass");
    require(
        fixture.lmStudio.statusCalls() == 1U &&
            fixture.lmStudio.deployCalls() == 0U &&
            fixture.toolAuthorizer.calls() == 0U &&
            fixture.uuidGenerator.consumed() == 0U,
        "absent LM Studio triggered authorization or deployment");
}

void earlierFailureDoesNotSuppressLaterIndependentStages()
{
    Fixture fixture;
    fixture.agentSessions.pruneStaleResult.set(failure<std::size_t>(
        Domain::ErrorCodes::StorageFull,
        "stale-session pruning failed first",
        false));
    fixture.continuity.recoveryResult.set(
        failure<Domain::ContinuityRecoveryReport>(
            Domain::ErrorCodes::DatabaseBusy,
            "continuity failed second",
            true));
    fixture.lmStudio.statusResult.set(failure<Domain::LMStudioPluginStatus>(
        Domain::ErrorCodes::HostCapabilityUnavailable,
        "LM Studio failed third",
        true));

    const auto result = fixture.service.reconcile(fixture.activeContext());
    requireError(
        result,
        Domain::ErrorCodes::StorageFull,
        "maintenance did not return the first independent-stage failure");
    require(
        result.error().message == "stale-session pruning failed first" &&
            !result.error().retryable,
        "maintenance changed the first failure payload");
    require(
        fixture.agentSessions.calls() == 1U &&
            fixture.continuity.callCount(
                Fakes::ContinuityCall::RecoverIncompleteOperations) == 1U &&
            fixture.lmStudio.statusCalls() == 1U,
        "an early independent-stage failure suppressed a later stage");
    require(
        fixture.lmStudio.deployCalls() == 0U &&
            fixture.toolAuthorizer.calls() == 0U,
        "failed LM Studio inspection advanced to deployment");
}

void partialContinuityFailureIsRetryableAndLmStillRuns()
{
    Fixture fixture;
    fixture.continuity.recoveryResult.set(success(
        Domain::ContinuityRecoveryReport{6U, 3U, 1U, 2U, {}}));

    const auto result = fixture.service.reconcile(fixture.activeContext());
    requireError(
        result,
        Domain::ErrorCodes::InternalFailure,
        "partial continuity recovery was reported as healthy");
    require(
        result.error().retryable,
        "partial continuity recovery was not marked retryable");
    require(
        fixture.continuity.callCount(
            Fakes::ContinuityCall::RecoverIncompleteOperations) == 1U &&
            fixture.lmStudio.statusCalls() == 1U &&
            fixture.lmStudio.deployCalls() == 0U,
        "partial continuity recovery suppressed healthy LM inspection");
}

void cancellationAndDeadlineAreCheckedBeforeAndBetweenStages()
{
    {
        Fixture fixture;
        std::stop_source cancellation;
        cancellation.request_stop();
        const auto result = fixture.service.reconcile(context(
            "77777777-7777-4777-8777-777777777777",
            "maintenance-pre-cancelled",
            fixture.now,
            cancellation.get_token()));
        requireError(
            result,
            Domain::ErrorCodes::Cancelled,
            "pre-cancelled maintenance entered a stage");
        require(
            fixture.agentSessions.calls() == 0U &&
                fixture.continuity.callCount(
                    Fakes::ContinuityCall::RecoverIncompleteOperations) == 0U &&
                fixture.lmStudio.statusCalls() == 0U,
            "pre-cancelled maintenance invoked a dependency");
    }
    {
        Fixture fixture;
        const auto result = fixture.service.reconcile(context(
            "88888888-8888-4888-8888-888888888888",
            "maintenance-pre-expired",
            fixture.now));
        requireError(
            result,
            Domain::ErrorCodes::DeadlineExceeded,
            "expired maintenance entered a stage");
        require(
            fixture.agentSessions.calls() == 0U &&
                fixture.lmStudio.statusCalls() == 0U,
            "expired maintenance invoked a dependency");
    }
    {
        Fixture fixture;
        std::stop_source cancellation;
        fixture.lmStudio.statusResult.set(success(driftedPluginStatus()));
        fixture.lmStudio.cancelOnNextStatus(cancellation);
        const auto result = fixture.service.reconcile(context(
            "99999999-9999-4999-8999-999999999999",
            "maintenance-mid-cancelled",
            fixture.now + 1min,
            cancellation.get_token()));
        requireError(
            result,
            Domain::ErrorCodes::Cancelled,
            "mid-pass cancellation advanced to LM deployment");
        require(
            fixture.lmStudio.statusCalls() == 1U &&
                fixture.lmStudio.deployCalls() == 0U &&
                fixture.toolAuthorizer.calls() == 0U &&
                fixture.uuidGenerator.consumed() == 0U,
            "mid-pass cancellation minted or consumed a deployment capability");
    }
    {
        Fixture fixture;
        fixture.lmStudio.statusResult.set(success(driftedPluginStatus()));
        fixture.lmStudio.advanceClockOnNextStatus(fixture.clock, 2min);
        const auto result = fixture.service.reconcile(context(
            "aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa",
            "maintenance-mid-expired",
            fixture.now + 1min));
        requireError(
            result,
            Domain::ErrorCodes::DeadlineExceeded,
            "mid-pass deadline advanced to LM deployment");
        require(
            fixture.lmStudio.statusCalls() == 1U &&
                fixture.lmStudio.deployCalls() == 0U &&
                fixture.toolAuthorizer.calls() == 0U &&
                fixture.uuidGenerator.consumed() == 0U,
            "mid-pass deadline minted or consumed a deployment capability");
    }
}

void capacityOneAdmissionRejectsOverlapAndReleases()
{
    Fixture fixture;
    fixture.lmStudio.blockNextStatus();
    std::promise<Domain::Result<void>> firstPromise;
    auto firstFuture = firstPromise.get_future();
    const auto firstContext = fixture.activeContext(
        "bbbbbbbb-bbbb-4bbb-8bbb-bbbbbbbbbbbb",
        "maintenance-first");
    std::jthread firstCaller{
        [&fixture, &firstPromise, firstContext]() mutable {
            firstPromise.set_value(
                fixture.service.reconcile(firstContext));
        }};

    if (!fixture.lmStudio.waitUntilStatusBlocked()) {
        fixture.lmStudio.releaseStatus();
        firstCaller.join();
        throw std::runtime_error{
            "the first maintenance pass did not block in LM inspection"};
    }
    const auto overlapping = fixture.service.reconcile(fixture.activeContext(
        "cccccccc-cccc-4ccc-8ccc-cccccccccccc",
        "maintenance-overlap"));
    fixture.lmStudio.releaseStatus();
    auto firstResult = firstFuture.get();
    firstCaller.join();

    requireError(
        overlapping,
        Domain::ErrorCodes::LimitExceeded,
        "an overlapping maintenance pass was admitted");
    require(
        overlapping.error().retryable,
        "overlapping maintenance rejection was not retryable");
    require(firstResult.hasValue(), "the admitted maintenance pass failed");
    require(
        fixture.agentSessions.calls() == 1U &&
            fixture.continuity.callCount(
                Fakes::ContinuityCall::RecoverIncompleteOperations) == 1U &&
            fixture.lmStudio.statusCalls() == 1U,
        "the rejected overlapping pass invoked a dependency");

    const auto successor = fixture.service.reconcile(fixture.activeContext(
        "dddddddd-dddd-4ddd-8ddd-dddddddddddd",
        "maintenance-successor"));
    require(successor.hasValue(), "admission was not released after the first pass");
    require(
        fixture.agentSessions.calls() == 2U &&
            fixture.lmStudio.statusCalls() == 2U,
        "the successor maintenance pass did not run exactly once");
}

void constructorRejectsInvalidAuthorityPairs()
{
    const auto now = testNow();
    Fakes::FakeClock clock{Domain::UtcTimePoint{} + 100s, now};
    Fakes::RecordingAgentSessionServiceFake agentSessions{
        Fakes::DefaultBoundaryCaptureItemsMaximum,
        Fakes::DefaultBoundaryCaptureTextBytesMaximum,
        now};
    Fakes::RecordingContinuityCoordinator continuity;
    BlockingRecordingLMStudioDeploymentService lmStudio;
    Fakes::DeterministicToolAuthorizerFake toolAuthorizer{
        std::string{Composition::ManagerMaintenanceService::DeploymentToolName},
        Domain::ToolEffect::Write,
        now};
    Fakes::SequenceUuidGenerator uuidGenerator{{parse<Domain::Uuid>(
        "eeeeeeee-eeee-4eee-8eee-eeeeeeeeeeee")}};

    const auto validRead = authority(
        Domain::FileAccess::Read,
        "22222222-2222-4222-8222-222222222222",
        "manager-maintenance",
        false,
        "11111111-2222-4333-8444-555555555555",
        3U);
    const auto validWrite = authority(
        Domain::FileAccess::Write,
        "22222222-2222-4222-8222-222222222222",
        "manager-maintenance",
        true,
        "99999999-8888-4777-8666-555555555555",
        19U);

    // The read and deployment capabilities have independent identities and
    // generations while retaining one maintenance project and caller.
    {
        Composition::ManagerMaintenanceService valid{
            agentSessions,
            continuity,
            lmStudio,
            toolAuthorizer,
            uuidGenerator,
            clock,
            Composition::ManagerMaintenanceServiceConfiguration{
                preferredBinary(), validRead, validWrite}};
    }

    const auto expectInvalid = [&](const Contracts::WorkspaceAuthority& read,
                                   const Contracts::WorkspaceAuthority& write,
                                   const std::string_view label) {
        bool rejected{};
        try {
            Composition::ManagerMaintenanceService invalid{
                agentSessions,
                continuity,
                lmStudio,
                toolAuthorizer,
                uuidGenerator,
                clock,
                Composition::ManagerMaintenanceServiceConfiguration{
                    preferredBinary(), read, write}};
        } catch (const std::invalid_argument&) {
            rejected = true;
        }
        require(rejected, label);
    };

    expectInvalid(
        authority(Domain::FileAccess::Write),
        validWrite,
        "constructor accepted a non-read LM Studio read authority");
    expectInvalid(
        validRead,
        authority(Domain::FileAccess::Read),
        "constructor accepted a non-write LM Studio write authority");
    expectInvalid(
        validRead,
        authority(
            Domain::FileAccess::Write,
            "12121212-1212-4212-8212-121212121212",
            "manager-maintenance",
            true),
        "constructor accepted mismatched authority projects");
    expectInvalid(
        validRead,
        authority(
            Domain::FileAccess::Write,
            "22222222-2222-4222-8222-222222222222",
            "different-manager-caller",
            true),
        "constructor accepted mismatched authority callers");
    expectInvalid(
        authority(
            Domain::FileAccess::Read,
            "22222222-2222-4222-8222-222222222222",
            "manager-maintenance",
            true),
        validWrite,
        "constructor accepted a shell-enabled read authority");
    expectInvalid(
        validRead,
        authority(
            Domain::FileAccess::Write,
            "22222222-2222-4222-8222-222222222222",
            "manager-maintenance",
            false),
        "constructor accepted a shell-disabled deployment authority");
    expectInvalid(
        validRead,
        authority(
            Domain::FileAccess::Write,
            "22222222-2222-4222-8222-222222222222",
            "manager-maintenance",
            true,
            validRead.authorityId().value(),
            19U),
        "constructor accepted one capability identity for read and deployment");
    expectInvalid(
        authority(
            Domain::FileAccess::Read,
            "22222222-2222-4222-8222-222222222222",
            "manager-maintenance",
            false,
            "11111111-2222-4333-8444-555555555555",
            0U),
        validWrite,
        "constructor accepted a zero-generation read capability");
    expectInvalid(
        validRead,
        authority(
            Domain::FileAccess::Write,
            "22222222-2222-4222-8222-222222222222",
            "manager-maintenance",
            true,
            "99999999-8888-4777-8666-555555555555",
            0U),
        "constructor accepted a zero-generation deployment capability");
}

} // namespace

int main()
{
    try {
        healthyPassPrunesRecoversAndInspectsWithoutDeployment();
        exactDriftAuthorizationDeploysTwoPlugins();
        absentLmStudioDoesNotDeploy();
        earlierFailureDoesNotSuppressLaterIndependentStages();
        partialContinuityFailureIsRetryableAndLmStillRuns();
        cancellationAndDeadlineAreCheckedBeforeAndBetweenStages();
        capacityOneAdmissionRejectsOverlapAndReleases();
        constructorRejectsInvalidAuthorityPairs();
        std::cout << "Manager maintenance service tests passed.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Manager maintenance service tests failed: "
                  << error.what() << '\n';
        return 1;
    } catch (...) {
        std::cerr << "Manager maintenance service tests failed with an unknown error.\n";
        return 1;
    }
}
