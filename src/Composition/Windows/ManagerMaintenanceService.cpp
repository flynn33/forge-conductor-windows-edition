#include "ManagerMaintenanceService.h"

#include "ForgeConductor/Contracts/IAgentServices.h"
#include "ForgeConductor/Contracts/IContinuityCoordinator.h"
#include "ForgeConductor/Contracts/IFoundationServices.h"
#include "ForgeConductor/Contracts/ILMStudioDeploymentService.h"
#include "ForgeConductor/Contracts/IToolServices.h"
#include "ForgeConductor/Domain/EnvironmentModels.h"
#include "ForgeConductor/Domain/Error.h"
#include "ForgeConductor/Domain/ToolModels.h"

#include <algorithm>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace ForgeConductor::Composition::Windows {
namespace {

[[nodiscard]] Domain::Error maintenanceError(
    const std::string_view code,
    std::string message,
    const bool retryable = false)
{
    return Domain::makeError(code, std::move(message), retryable);
}

class AdmissionLease final {
public:
    explicit AdmissionLease(std::atomic_flag& admitted) noexcept
        : admitted_{admitted}, owns_{!admitted_.test_and_set(
              std::memory_order_acq_rel)}
    {
    }

    ~AdmissionLease() noexcept
    {
        if (owns_) {
            admitted_.clear(std::memory_order_release);
        }
    }

    AdmissionLease(const AdmissionLease&) = delete;
    AdmissionLease& operator=(const AdmissionLease&) = delete;

    [[nodiscard]] bool owns() const noexcept { return owns_; }

private:
    std::atomic_flag& admitted_;
    const bool owns_;
};

[[nodiscard]] bool requiresDeployment(
    const Domain::LMStudioPluginStatus& status) noexcept
{
    return status.lmStudioPresent &&
        (!status.primaryPluginInstalled ||
         !status.fallbackPluginInstalled ||
         !status.mcpConfigurationRegistered ||
         !status.binaryExecutable ||
         !status.deploymentId.has_value());
}

[[nodiscard]] bool containsAccess(
    const std::vector<Domain::FileAccess>& values,
    const Domain::FileAccess access) noexcept
{
    return std::find(values.begin(), values.end(), access) != values.end();
}

[[nodiscard]] bool hasExactReadCapability(
    const Contracts::WorkspaceAuthority& authority) noexcept
{
    return authority.intent() == Domain::FileAccess::Read &&
        authority.grants() ==
            std::vector<Domain::FileAccess>{Domain::FileAccess::Read} &&
        containsAccess(authority.denials(), Domain::FileAccess::Write) &&
        containsAccess(authority.denials(), Domain::FileAccess::Create) &&
        containsAccess(authority.denials(), Domain::FileAccess::Delete) &&
        containsAccess(authority.denials(), Domain::FileAccess::Execute) &&
        !authority.shellEnabled();
}

[[nodiscard]] bool hasDeploymentCapability(
    const Contracts::WorkspaceAuthority& authority) noexcept
{
    const auto granted = [&](const Domain::FileAccess access) noexcept {
        return containsAccess(authority.grants(), access) &&
            !containsAccess(authority.denials(), access);
    };
    return authority.intent() == Domain::FileAccess::Write &&
        granted(Domain::FileAccess::Read) &&
        granted(Domain::FileAccess::Write) &&
        granted(Domain::FileAccess::Create) &&
        granted(Domain::FileAccess::Delete) &&
        granted(Domain::FileAccess::Execute) &&
        authority.shellEnabled();
}

} // namespace

ManagerMaintenanceService::ManagerMaintenanceService(
    Contracts::IAgentSessionService& agentSessions,
    Contracts::IContinuityCoordinator& continuity,
    Contracts::ILMStudioDeploymentService& lmStudioDeployment,
    Contracts::IToolAuthorizer& toolAuthorizer,
    Contracts::IUuidGenerator& uuidGenerator,
    const Contracts::IClock& clock,
    ManagerMaintenanceServiceConfiguration configuration)
    : agentSessions_{agentSessions},
      continuity_{continuity},
      lmStudioDeployment_{lmStudioDeployment},
      toolAuthorizer_{toolAuthorizer},
      uuidGenerator_{uuidGenerator},
      clock_{clock},
      configuration_{std::move(configuration)}
{
    const auto& read = configuration_.lmStudioReadAuthority;
    const auto& write = configuration_.lmStudioWriteAuthority;
    if (!hasExactReadCapability(read) ||
        !hasDeploymentCapability(write) ||
        read.projectId() != write.projectId() ||
        read.callerId() != write.callerId() ||
        read.authorityId() == write.authorityId() ||
        read.generation() == 0U || write.generation() == 0U) {
        throw std::invalid_argument{
            "ManagerMaintenanceService requires distinct, matching-scope LM Studio read and deployment capabilities."};
    }
}

Domain::Result<void> ManagerMaintenanceService::validateContext(
    const Domain::OperationContext& context,
    const std::string_view action) const noexcept
{
    if (context.isCancellationRequested()) {
        return Domain::Result<void>::failure(maintenanceError(
            Domain::ErrorCodes::Cancelled,
            "Manager maintenance could not " + std::string{action} +
                " because cancellation was requested."));
    }
    if (context.isExpired(clock_.monotonicNow())) {
        return Domain::Result<void>::failure(maintenanceError(
            Domain::ErrorCodes::DeadlineExceeded,
            "Manager maintenance could not " + std::string{action} +
                " because its deadline expired."));
    }
    return Domain::Result<void>::success();
}

Domain::Result<void> ManagerMaintenanceService::reconcileLmStudio(
    const Domain::OperationContext& context) noexcept
{
    const Domain::LMStudioDeploymentRequest request{
        configuration_.preferredForgeBinary, true};
    auto status = lmStudioDeployment_.status(
        request, configuration_.lmStudioReadAuthority, context);
    if (!status) {
        return Domain::Result<void>::failure(std::move(status).error());
    }
    if (!requiresDeployment(status.value())) {
        return Domain::Result<void>::success();
    }

    auto current = validateContext(context, "authorize LM Studio repair");
    if (!current) {
        return current;
    }
    auto requestUuid = uuidGenerator_.next();
    if (!requestUuid) {
        return Domain::Result<void>::failure(
            std::move(requestUuid).error());
    }
    auto requestId = Domain::RequestId::parse(requestUuid.value().value());
    if (!requestId) {
        return Domain::Result<void>::failure(std::move(requestId).error());
    }

    const auto& authority = configuration_.lmStudioWriteAuthority;
    Domain::ToolCallRequest call{
        Domain::McpRequestMetadata{
            std::move(requestId).value(),
            context.correlationId,
            authority.callerId(),
            authority.projectId(),
            std::string{ProtocolVersion}},
        std::string{DeploymentToolName},
        std::string{CanonicalDeploymentArguments}};
    auto authorized = toolAuthorizer_.authorize(
        Domain::ToolAuthorizationRequest{
            std::move(call),
            Domain::ToolEffect::Write,
            Domain::AuthorityReference{
                authority.authorityId(), authority.generation()}},
        authority,
        context);
    if (!authorized) {
        return Domain::Result<void>::failure(std::move(authorized).error());
    }

    auto deployed = lmStudioDeployment_.deploy(
        request, authority, authorized.value(), context);
    if (!deployed) {
        return Domain::Result<void>::failure(std::move(deployed).error());
    }
    if (!deployed.value().ok || deployed.value().pluginsWritten.size() != 2U) {
        return Domain::Result<void>::failure(maintenanceError(
            Domain::ErrorCodes::IntegrityFailure,
            "LM Studio maintenance returned an incomplete deployment result."));
    }
    return Domain::Result<void>::success();
}

Domain::Result<void> ManagerMaintenanceService::reconcile(
    const Domain::OperationContext& context) noexcept
{
    try {
        auto current = validateContext(context, "start a reconciliation pass");
        if (!current) {
            return current;
        }
        const AdmissionLease admission{operationAdmitted_};
        if (!admission.owns()) {
            return Domain::Result<void>::failure(maintenanceError(
                Domain::ErrorCodes::LimitExceeded,
                "A Manager maintenance pass is already active.",
                true));
        }

        std::optional<Domain::Error> firstFailure;
        const auto rememberFailure = [&firstFailure](auto result) {
            if (!result && !firstFailure.has_value()) {
                firstFailure = std::move(result).error();
            }
        };

        rememberFailure(agentSessions_.pruneStale(context));
        current = validateContext(context, "recover durable continuity");
        if (!current) {
            return current;
        }

        auto recovered = continuity_.recoverIncompleteOperations(
            Domain::ContinuityRecoveryRequest{std::nullopt, true}, context);
        if (!recovered) {
            rememberFailure(std::move(recovered));
        } else if (recovered.value().failed != 0U) {
            rememberFailure(Domain::Result<void>::failure(maintenanceError(
                Domain::ErrorCodes::InternalFailure,
                "Durable continuity maintenance could not recover every inspected operation.",
                true)));
        }

        current = validateContext(context, "reconcile LM Studio deployment");
        if (!current) {
            return current;
        }
        rememberFailure(reconcileLmStudio(context));

        current = validateContext(context, "finish a reconciliation pass");
        if (!current) {
            return current;
        }
        if (firstFailure.has_value()) {
            return Domain::Result<void>::failure(std::move(*firstFailure));
        }
        return Domain::Result<void>::success();
    } catch (...) {
        return Domain::Result<void>::failure(maintenanceError(
            Domain::ErrorCodes::InternalFailure,
            "Manager maintenance failed safely.",
            true));
    }
}

} // namespace ForgeConductor::Composition::Windows
