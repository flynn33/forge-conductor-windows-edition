#include "ForgeConductor/Manager/ManagerRequestDispatcher.h"

#include "ForgeConductor/Manager/ManagerDeadlineMapper.h"
#include "ForgeConductor/Dashboard/DashboardSessionCloseRequest.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <ctime>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <stop_token>
#include <stdexcept>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace ForgeConductor::Manager {
namespace {

[[nodiscard]] Domain::Error error(
    const std::string_view code,
    const char* const message,
    const bool retryable = false)
{
    return Domain::makeError(code, message, retryable);
}

[[nodiscard]] ManagerResponse responseWithError(
    const ManagerRequest& request,
    Domain::Error failure)
{
    return ManagerResponse{
        ManagerProtocolVersion,
        request.requestId,
        request.correlationId,
        ManagerResponseBody{std::in_place_type<Domain::Error>,
                            std::move(failure)}};
}

template <typename T>
[[nodiscard]] ManagerResponse responseWithResult(
    const ManagerRequest& request,
    T value)
{
    return ManagerResponse{
        ManagerProtocolVersion,
        request.requestId,
        request.correlationId,
        ManagerResponseBody{
            std::in_place_type<ManagerResult>,
            ManagerResult{std::in_place_type<T>, std::move(value)}}};
}

[[nodiscard]] ManagerResponse acknowledgement(const ManagerRequest& request)
{
    return responseWithResult(request, ManagerAcknowledgement{});
}

[[nodiscard]] std::chrono::milliseconds nonnegativeRemaining(
    const Domain::MonotonicTimePoint deadline,
    const Domain::MonotonicTimePoint now) noexcept
{
    if (now >= deadline) {
        return std::chrono::milliseconds::zero();
    }
    const auto remaining = deadline - now;
    auto rounded = std::chrono::ceil<std::chrono::milliseconds>(remaining);
    if (rounded < std::chrono::milliseconds::zero()) {
        rounded = std::chrono::milliseconds::zero();
    }
    return rounded;
}

} // namespace

class ManagerRequestDispatcher::Implementation final {
public:
    Implementation(
        std::shared_ptr<Contracts::IManagerController> controller,
        std::shared_ptr<Contracts::IClock> clock,
        ManagerTransportLimits limits,
        std::shared_ptr<Contracts::IManagedRunService> managedRuns,
        ManagerTelemetrySources telemetrySources)
        : controller_{std::move(controller)},
          clock_{std::move(clock)},
          limits_{std::move(limits)},
          managedRuns_{std::move(managedRuns)},
          telemetrySources_{telemetrySources}
    {
        limits_.maximumActiveRegularOperations = (std::min)(
            limits_.maximumActiveRegularOperations,
            MaximumActiveRegularOperations);
    }

    [[nodiscard]] ManagerResponse dispatch(
        const ManagerRequest& request) noexcept
    {
        try {
            if (request.version != ManagerProtocolVersion) {
                return responseWithError(
                    request,
                    error(
                        Domain::ErrorCodes::UnsupportedVersion,
                        "The manager request protocol version is unsupported."));
            }

            auto operationId = Domain::OperationId::parse(
                request.requestId.value());
            if (!operationId) {
                return responseWithError(request, std::move(operationId).error());
            }
            auto deadline = fromManagerWireDeadline(
                request.deadlineUtcMilliseconds, *clock_, limits_);
            if (!deadline) {
                return responseWithError(request, std::move(deadline).error());
            }

            if (const auto* cancelRequest =
                    std::get_if<ManagerCancelRequest>(&request.payload)) {
                cancel(cancelRequest->operationId);
                return acknowledgement(request);
            }
            if (std::holds_alternative<ManagerShutdownRequest>(request.payload)) {
                return dispatchShutdown(
                    request,
                    std::move(operationId).value(),
                    deadline.value());
            }

            auto admitted = admit(std::move(operationId).value());
            if (!admitted) {
                return responseWithError(request, std::move(admitted).error());
            }
            auto active = std::move(admitted).value();
            ActiveLease lease{*this, active};
            const Domain::OperationContext context{
                active->operationId,
                deadline.value(),
                active->cancellation.get_token(),
                request.correlationId};

            if (auto current = validateContext(context); !current) {
                return responseWithError(request, std::move(current).error());
            }
            auto response = dispatchRegular(request, context);
            if (auto current = validateContext(context); !current) {
                return responseWithError(request, std::move(current).error());
            }
            return response;
        } catch (...) {
            return responseWithError(
                request,
                error(
                    Domain::ErrorCodes::InternalFailure,
                    "The manager request dispatcher failed safely."));
        }
    }

    void beginShutdown() noexcept
    {
        try {
            std::vector<std::shared_ptr<ActiveOperation>> active;
            {
                std::lock_guard lock{stateMutex_};
                accepting_ = false;
                active.reserve(activeOperations_.size());
                for (const auto& entry : activeOperations_) {
                    active.push_back(entry.second);
                }
            }
            for (const auto& operation : active) {
                operation->cancellation.request_stop();
            }
            stateChanged_.notify_all();
        } catch (...) {
            // Shutdown remains best effort at a noexcept ownership boundary.
        }
    }

    void cancel(const Domain::OperationId& operationId) noexcept
    {
        try {
            std::shared_ptr<ActiveOperation> active;
            {
                std::lock_guard lock{stateMutex_};
                const auto found = activeOperations_.find(operationId);
                if (found != activeOperations_.end()) {
                    active = found->second;
                }
            }
            if (active) {
                active->cancellation.request_stop();
            }
        } catch (...) {
        }
    }

    [[nodiscard]] bool waitUntilIdle(
        const std::chrono::milliseconds timeout) noexcept
    {
        if (timeout < std::chrono::milliseconds::zero()) {
            return false;
        }
        try {
            std::unique_lock lock{stateMutex_};
            return stateChanged_.wait_for(
                lock,
                timeout,
                [this] { return activeOperations_.empty(); });
        } catch (...) {
            return false;
        }
    }

    [[nodiscard]] std::size_t activeOperationCount() const noexcept
    {
        try {
            std::lock_guard lock{stateMutex_};
            return activeOperations_.size();
        } catch (...) {
            return 0U;
        }
    }

    [[nodiscard]] bool isAccepting() const noexcept
    {
        try {
            std::lock_guard lock{stateMutex_};
            return accepting_;
        } catch (...) {
            return false;
        }
    }

    void shutdown() noexcept
    {
        beginShutdown();
        if (managedRuns_) {
            managedRuns_->shutdown();
        }
        static_cast<void>(waitUntilIdle(limits_.shutdownDrainTimeout));
        bool closeController{};
        {
            try {
                std::lock_guard lock{stateMutex_};
                closeControllerWhenIdle_ = true;
                if (activeOperations_.empty() && !controllerClosed_) {
                    controllerClosed_ = true;
                    closeController = true;
                }
            } catch (...) {
            }
        }
        if (closeController) {
            try {
                controller_->shutdown();
            } catch (...) {
            }
        }
    }

private:
    static constexpr std::size_t MaximumActiveRegularOperations = 3U;

    struct ActiveOperation final {
        explicit ActiveOperation(Domain::OperationId id)
            : operationId{std::move(id)}
        {
        }

        Domain::OperationId operationId;
        std::stop_source cancellation;
    };

    class ActiveLease final {
    public:
        ActiveLease(
            Implementation& owner,
            std::shared_ptr<ActiveOperation> active) noexcept
            : owner_{owner}, active_{std::move(active)}
        {
        }

        ~ActiveLease() noexcept { owner_.release(active_); }

        ActiveLease(const ActiveLease&) = delete;
        ActiveLease& operator=(const ActiveLease&) = delete;
        ActiveLease(ActiveLease&&) = delete;
        ActiveLease& operator=(ActiveLease&&) = delete;

    private:
        Implementation& owner_;
        std::shared_ptr<ActiveOperation> active_;
    };

    [[nodiscard]] Domain::Result<std::shared_ptr<ActiveOperation>> admit(
        Domain::OperationId operationId)
    {
        try {
            auto active = std::make_shared<ActiveOperation>(
                std::move(operationId));
            std::lock_guard lock{stateMutex_};
            if (!accepting_) {
                return Domain::Result<std::shared_ptr<ActiveOperation>>::failure(
                    error(
                        Domain::ErrorCodes::TransportClosed,
                        "The manager dispatcher is no longer accepting work."));
            }
            if (activeOperations_.contains(active->operationId)) {
                return Domain::Result<std::shared_ptr<ActiveOperation>>::failure(
                    error(
                        Domain::ErrorCodes::Conflict,
                        "The manager request identifier is already active."));
            }
            if (activeOperations_.size() >=
                limits_.maximumActiveRegularOperations) {
                return Domain::Result<std::shared_ptr<ActiveOperation>>::failure(
                    error(
                        Domain::ErrorCodes::LimitExceeded,
                        "The manager active-operation bound was reached.",
                        true));
            }
            activeOperations_.emplace(active->operationId, active);
            return Domain::Result<std::shared_ptr<ActiveOperation>>::success(
                std::move(active));
        } catch (...) {
            return Domain::Result<std::shared_ptr<ActiveOperation>>::failure(
                error(
                    Domain::ErrorCodes::InternalFailure,
                    "The manager request could not be admitted."));
        }
    }

    void release(const std::shared_ptr<ActiveOperation>& active) noexcept
    {
        bool closeController{};
        try {
            {
                std::lock_guard lock{stateMutex_};
                const auto found = activeOperations_.find(active->operationId);
                if (found != activeOperations_.end() &&
                    found->second == active) {
                    activeOperations_.erase(found);
                }
                if (activeOperations_.empty() && closeControllerWhenIdle_ &&
                    !controllerClosed_) {
                    controllerClosed_ = true;
                    closeController = true;
                }
            }
            stateChanged_.notify_all();
            if (closeController) {
                controller_->shutdown();
            }
        } catch (...) {
        }
    }

    [[nodiscard]] Domain::Result<void> validateContext(
        const Domain::OperationContext& context) const noexcept
    {
        if (context.isCancellationRequested()) {
            return Domain::Result<void>::failure(
                error(
                    Domain::ErrorCodes::Cancelled,
                    "The manager request was cancelled."));
        }
        if (context.isExpired(clock_->monotonicNow())) {
            return Domain::Result<void>::failure(
                error(
                    Domain::ErrorCodes::DeadlineExceeded,
                    "The manager request deadline has expired."));
        }
        return Domain::Result<void>::success();
    }

    template <typename T>
    [[nodiscard]] ManagerResponse controllerResponse(
        const ManagerRequest& request,
        Domain::Result<T> result)
    {
        if (!result) {
            return responseWithError(request, std::move(result).error());
        }
        return responseWithResult(request, std::move(result).value());
    }

    [[nodiscard]] Domain::Result<Domain::ManagerTelemetrySnapshot>
    telemetrySnapshot(
        const ManagerTelemetryRequest& request,
        const Domain::OperationContext& context)
    {
        if (telemetrySources_.telemetry == nullptr) {
            return Domain::Result<Domain::ManagerTelemetrySnapshot>::failure(
                error(
                    Domain::ErrorCodes::InvalidRequest,
                    "Manager telemetry is unavailable in this composition."));
        }

        auto sampled = telemetrySources_.telemetry->sample(false, context);
        if (!sampled) {
            return Domain::Result<Domain::ManagerTelemetrySnapshot>::failure(
                std::move(sampled).error());
        }
        auto telemetry = std::move(sampled).value();
        if (!telemetry) {
            return Domain::Result<Domain::ManagerTelemetrySnapshot>::failure(
                error(
                    Domain::ErrorCodes::IntegrityFailure,
                    "The Manager telemetry service returned no snapshot."));
        }

        auto status = controller_->status(context);
        if (!status) {
            return Domain::Result<Domain::ManagerTelemetrySnapshot>::failure(
                std::move(status).error());
        }
        auto settings = controller_->settings(context);
        if (!settings) {
            return Domain::Result<Domain::ManagerTelemetrySnapshot>::failure(
                std::move(settings).error());
        }

        std::optional<Domain::ManagedRunSnapshot> selectedRun;
        if (request.runId) {
            if (!managedRuns_) {
                return Domain::Result<Domain::ManagerTelemetrySnapshot>::failure(
                    error(
                        Domain::ErrorCodes::InvalidRequest,
                        "Managed runs are unavailable in this Manager composition."));
            }
            auto run = managedRuns_->status(*request.runId, context);
            if (!run) {
                return Domain::Result<Domain::ManagerTelemetrySnapshot>::failure(
                    std::move(run).error());
            }
            selectedRun = std::move(run).value();
        }

        std::optional<Domain::RuntimeDiagnosticSnapshot> runtimeDiagnostics;
        std::vector<std::string> tools;
        std::size_t openSessionCount{};
        std::size_t recentSessionCount{};
        std::size_t presenceCount{};
        std::vector<Domain::AuditEvent> recentEvents;
        std::optional<std::string> storeFailure;

        if (telemetrySources_.operational != nullptr) {
            auto operational = telemetrySources_.operational->status(context);
            if (operational) {
                runtimeDiagnostics = operational.value().runtimeDiagnostics;
                tools = operational.value().toolNames;
                openSessionCount = operational.value().openSessions.size();
                presenceCount = operational.value().presence.size();
                recentEvents = operational.value().recentAudit;
            } else {
                storeFailure = operational.error().message;
            }

            auto sessions = telemetrySources_.operational->sessions(context);
            if (sessions) {
                openSessionCount = sessions.value().open.size();
                recentSessionCount = sessions.value().recent.size();
            } else if (!storeFailure) {
                storeFailure = sessions.error().message;
            }
        } else {
            storeFailure = "The operational store source is unavailable.";
        }

        if (tools.empty() && telemetrySources_.tools != nullptr) {
            const auto catalog = telemetrySources_.tools->tools();
            tools.reserve(catalog.size());
            for (const auto& tool : catalog) {
                tools.push_back(tool.tool.name);
            }
        }
        if (tools.size() > Domain::MaximumManagerTelemetryTools) {
            return Domain::Result<Domain::ManagerTelemetrySnapshot>::failure(
                error(
                    Domain::ErrorCodes::LimitExceeded,
                    "The Manager telemetry tool projection exceeds its bound."));
        }

        std::vector<Domain::ProjectId> projects;
        if (telemetrySources_.projects != nullptr) {
            auto listed = telemetrySources_.projects->list(
                Domain::MaximumManagerTelemetryProjects, context);
            if (listed) {
                projects.reserve(listed.value().size());
                for (const auto& project : listed.value()) {
                    projects.push_back(project.id);
                }
            } else if (!storeFailure) {
                storeFailure = listed.error().message;
            }
        }

        const auto capturedAt = telemetry->updatedAt;
        auto storeHealthy = storeFailure
            ? Domain::makeUnavailableTelemetryMetric<bool>(
                  Domain::TelemetryMetricAvailability::TemporarilyUnavailable,
                  capturedAt,
                  "manager_operational_store",
                  *storeFailure)
            : Domain::makeAvailableTelemetryMetric<bool>(
                  true, capturedAt, "manager_operational_store");

        const auto& managerSettings = settings.value();
        Domain::ManagerProviderSnapshot provider{
            managerSettings.localModelHost,
            managerSettings.localModelPort,
            managerSettings.localModelSecure,
            managerSettings.localModelName.empty()
                ? std::optional<std::string>{}
                : std::optional<std::string>{managerSettings.localModelName},
            selectedRun ? selectedRun->record.providerResponseId : std::nullopt};

        Domain::ManagerContextSnapshot contextSnapshot{
            managerSettings.effectiveContextCapacity,
            managerSettings.nextResponseReserve,
            managerSettings.handoffReserve,
            managerSettings.estimationSafetyMargin};
        Domain::ManagerContinuitySnapshot continuity;
        if (selectedRun) {
            const auto& record = selectedRun->record;
            contextSnapshot.inputTokens = record.inputTokens;
            contextSnapshot.outputTokens = record.outputTokens;
            contextSnapshot.retainedTokens = record.retainedContextTokens;
            contextSnapshot.authoritative = record.retainedContextTokens.has_value();
            if (record.retainedContextTokens) {
                const auto reserved =
                    static_cast<std::uint64_t>(managerSettings.nextResponseReserve) +
                    managerSettings.handoffReserve +
                    managerSettings.estimationSafetyMargin;
                const auto occupied = *record.retainedContextTokens + reserved;
                contextSnapshot.headroomTokens = occupied <
                        managerSettings.effectiveContextCapacity
                    ? managerSettings.effectiveContextCapacity - occupied
                    : 0U;
            }
            continuity.managerOwned = selectedRun->managerOwned;
            continuity.runId = record.runId;
            continuity.projectId = record.projectId;
            continuity.runState = record.state;
            continuity.canonicalResponseId = record.providerResponseId;
        }

        Domain::ManagerResourceSnapshot resources{
            telemetry->system.timestamp,
            telemetry->system.host,
            telemetry->system.platform,
            telemetry->system.architecture,
            telemetry->system.cpu.percent,
            telemetry->system.ram.percent,
            telemetry->system.ram.usedBytes,
            telemetry->system.ram.totalBytes,
            telemetry->system.ram.availableBytes,
            telemetry->system.gpus,
            telemetry->system.processes,
            telemetry->history,
            telemetry->system.cpu.perLogicalProcessor,
            telemetry->system.cpu.frequencyMhz,
            telemetry->system.cpu.perCoreFrequencyMhz,
            telemetry->system.disks,
            telemetry->system.diskIoSample,
            telemetry->system.targetSampleIntervalMilliseconds,
            telemetry->system.measuredSampleIntervalMilliseconds,
            telemetry->system.samplingPolicy};

        return Domain::Result<Domain::ManagerTelemetrySnapshot>::success(
            Domain::ManagerTelemetrySnapshot{
                capturedAt,
                std::move(resources),
                std::move(status).value(),
                std::move(runtimeDiagnostics),
                std::move(provider),
                std::move(contextSnapshot),
                std::move(continuity),
                std::move(selectedRun),
                std::move(projects),
                std::move(tools),
                openSessionCount,
                recentSessionCount,
                presenceCount,
                std::move(recentEvents),
                std::move(storeHealthy),
            telemetry->runtime});
    }

    [[nodiscard]] Domain::Result<ManagerProjectWorkspaceSnapshot>
    projectWorkspace(
        const Domain::ProjectId& projectId,
        const std::string& query,
        const std::size_t maximumCount,
        std::optional<Domain::MemoryRecordId> writtenRecordId,
        const Domain::OperationContext& context)
    {
        if (telemetrySources_.projects == nullptr ||
            telemetrySources_.projectMemory == nullptr) {
            return Domain::Result<ManagerProjectWorkspaceSnapshot>::failure(
                error(
                    Domain::ErrorCodes::InvalidRequest,
                    "Project workflows are unavailable in this Manager composition."));
        }
        if (maximumCount == 0U || maximumCount > 100U) {
            return Domain::Result<ManagerProjectWorkspaceSnapshot>::failure(
                error(
                    Domain::ErrorCodes::InvalidRequest,
                    "Project memory result count must be within 1 through 100."));
        }

        auto descriptor = telemetrySources_.projects->descriptor(projectId, context);
        if (!descriptor) {
            return Domain::Result<ManagerProjectWorkspaceSnapshot>::failure(
                std::move(descriptor).error());
        }
        auto status = telemetrySources_.projectMemory->status(
            Domain::ProjectMemoryStatusRequest{projectId}, context);
        if (!status) {
            return Domain::Result<ManagerProjectWorkspaceSnapshot>::failure(
                std::move(status).error());
        }
        if (status.value().projectId != projectId) {
            return Domain::Result<ManagerProjectWorkspaceSnapshot>::failure(
                error(
                    Domain::ErrorCodes::ProjectScopeMismatch,
                    "Project memory status returned a different project identity."));
        }

        Domain::Result<Domain::MemoryPage> page = query.empty()
            ? telemetrySources_.projectMemory->listRecent(
                  Domain::ListRecentProjectMemoryRequest{
                      projectId, {}, std::nullopt, maximumCount, std::nullopt,
                      true, 256U * 1024U},
                  context)
            : telemetrySources_.projectMemory->search(
                  Domain::SearchProjectMemoryRequest{
                      projectId, query, {}, {}, std::nullopt, maximumCount,
                      std::nullopt, true, 256U * 1024U},
                  context);
        if (!page) {
            return Domain::Result<ManagerProjectWorkspaceSnapshot>::failure(
                std::move(page).error());
        }
        if (page.value().projectId != projectId ||
            std::any_of(
                page.value().records.begin(),
                page.value().records.end(),
                [&](const Domain::MemorySearchHit& hit) {
                    return hit.record.projectId != projectId;
                })) {
            return Domain::Result<ManagerProjectWorkspaceSnapshot>::failure(
                error(
                    Domain::ErrorCodes::ProjectScopeMismatch,
                    "Project memory returned records from a different project identity."));
        }

        std::vector<ManagerProjectMemoryRecord> records;
        records.reserve(page.value().records.size());
        for (auto& hit : page.value().records) {
            auto& record = hit.record;
            records.push_back(ManagerProjectMemoryRecord{
                record.id,
                record.version,
                std::move(record.kind),
                std::move(record.title),
                std::move(record.summary),
                std::move(record.body),
                std::move(record.tags),
                record.updatedAt});
        }

        const auto& memoryStatus = status.value();
        return Domain::Result<ManagerProjectWorkspaceSnapshot>::success(
            ManagerProjectWorkspaceSnapshot{
                std::move(descriptor).value(),
                memoryStatus.recordCount,
                memoryStatus.tombstoneCount,
                memoryStatus.eventCount,
                memoryStatus.databaseBytes,
                memoryStatus.writeAheadLogBytes,
                memoryStatus.fullTextSearchAvailable,
                memoryStatus.integrityOk,
                std::move(records),
                std::move(page.value().nextCursor),
                page.value().truncated,
                std::move(writtenRecordId)});
    }

    [[nodiscard]] Domain::Result<ManagerLmStudioSnapshot> lmStudioWorkflow(
        const ManagerRequest& managerRequest,
        const bool repair,
        const bool activate,
        const Domain::OperationContext& context)
    {
        const auto& sources = telemetrySources_;
        const auto trace = [&](const std::string_view event) noexcept {
            if (!repair || sources.diagnostics == nullptr) return;
            try {
                // A contested diagnostic ancestor must not consume the repair
                // deadline before native admission or connector verification.
                auto diagnosticContext = context;
                diagnosticContext.deadline = (std::min)(
                    context.deadline,
                    clock_->monotonicNow() + std::chrono::milliseconds{250});
                static_cast<void>(sources.diagnostics->record(
                    Domain::DiagnosticEnvelope{
                        clock_->utcNow(), std::string{event},
                        Domain::DiagnosticSeverity::Info, "manager",
                        ::GetCurrentProcessId(),
                        Domain::DiagnosticCategory::LmStudio, {}},
                    diagnosticContext));
            } catch (...) {
            }
        };
        trace("lmstudio_repair_request_received");
        if (sources.lmStudioDeployment == nullptr ||
            sources.lmStudioReadAuthority == nullptr ||
            sources.lmStudioWriteAuthority == nullptr ||
            sources.toolAuthorizer == nullptr) {
            return Domain::Result<ManagerLmStudioSnapshot>::failure(error(
                Domain::ErrorCodes::InvalidRequest,
                "LM Studio workflows are unavailable in this Manager composition."));
        }

        const Domain::LMStudioDeploymentRequest deploymentRequest{
            sources.preferredForgeBinary, true};
        auto inspected = sources.lmStudioDeployment->status(
            deploymentRequest, *sources.lmStudioReadAuthority, context);
        if (!inspected) {
            return Domain::Result<ManagerLmStudioSnapshot>::failure(
                std::move(inspected).error());
        }
        trace("lmstudio_repair_inspection_complete");

        std::string actionDetail{"Registration inspected without changing LM Studio."};
        if (repair) {
            const auto& authority = *sources.lmStudioWriteAuthority;
            Domain::ToolCallRequest call{
                Domain::McpRequestMetadata{
                    managerRequest.requestId,
                    context.correlationId,
                    authority.callerId(),
                    authority.projectId(),
                    "2025-11-25"},
                "install-lmstudio-plugin",
                "{\"preserve_foreign_entries\":true}"};
            auto authorized = sources.toolAuthorizer->authorize(
                Domain::ToolAuthorizationRequest{
                    call,
                    Domain::ToolEffect::Write,
                    Domain::AuthorityReference{
                        authority.authorityId(), authority.generation()}},
                authority,
                context);
            if (!authorized) {
                return Domain::Result<ManagerLmStudioSnapshot>::failure(
                    std::move(authorized).error());
            }
            trace("lmstudio_repair_authorized");
            trace("lmstudio_repair_deployment_requested");
            auto deployed = sources.lmStudioDeployment->deploy(
                deploymentRequest, authority, authorized.value(), context);
            if (!deployed) {
                return Domain::Result<ManagerLmStudioSnapshot>::failure(
                    std::move(deployed).error());
            }
            trace("lmstudio_repair_deployment_complete");
            actionDetail = deployed.value().message;
            inspected = sources.lmStudioDeployment->status(
                deploymentRequest, *sources.lmStudioReadAuthority, context);
            if (!inspected) {
                return Domain::Result<ManagerLmStudioSnapshot>::failure(
                    std::move(inspected).error());
            }
        }

        bool connectionCheckPerformed{};
        bool primaryReady{};
        bool fallbackReady{};
        bool continuityReady{};
        if (activate) {
            if (!inspected.value().deploymentId) {
                return Domain::Result<ManagerLmStudioSnapshot>::failure(error(
                    Domain::ErrorCodes::Conflict,
                    "LM Studio must have a complete Forge Conductor deployment before connector activation."));
            }
            const auto& authority = *sources.lmStudioWriteAuthority;
            const auto deploymentId = *inspected.value().deploymentId;
            Domain::ToolCallRequest call{
                Domain::McpRequestMetadata{
                    managerRequest.requestId,
                    context.correlationId,
                    authority.callerId(),
                    authority.projectId(),
                    "2025-11-25"},
                "activate-lmstudio-connectors",
                "{\"deployment_id\":\"" + deploymentId.value() + "\"}"};
            auto authorized = sources.toolAuthorizer->authorize(
                Domain::ToolAuthorizationRequest{
                    call,
                    Domain::ToolEffect::Execute,
                    Domain::AuthorityReference{
                        authority.authorityId(), authority.generation()}},
                authority,
                context);
            if (!authorized) {
                return Domain::Result<ManagerLmStudioSnapshot>::failure(
                    std::move(authorized).error());
            }
            auto activated = sources.lmStudioDeployment->activate(
                Domain::LMStudioHostActivationRequest{
                    deploymentId, std::chrono::seconds{20}},
                authority,
                authorized.value(),
                context);
            if (!activated) {
                return Domain::Result<ManagerLmStudioSnapshot>::failure(
                    std::move(activated).error());
            }
            connectionCheckPerformed = true;
            primaryReady = std::find(
                activated.value().readyRoles.begin(),
                activated.value().readyRoles.end(),
                Domain::LMStudioConnectorRole::Primary) !=
                activated.value().readyRoles.end();
            fallbackReady = std::find(
                activated.value().readyRoles.begin(),
                activated.value().readyRoles.end(),
                Domain::LMStudioConnectorRole::Fallback) !=
                activated.value().readyRoles.end();
            continuityReady = std::find(
                activated.value().readyRoles.begin(),
                activated.value().readyRoles.end(),
                Domain::LMStudioConnectorRole::Clu) !=
                activated.value().readyRoles.end();
            actionDetail = activated.value().detail;
        }

        const auto& status = inspected.value();
        return Domain::Result<ManagerLmStudioSnapshot>::success(
            ManagerLmStudioSnapshot{
                status.lmStudioPresent,
                status.primaryPluginInstalled,
                status.fallbackPluginInstalled,
                status.continuityPluginInstalled,
                status.mcpConfigurationRegistered,
                status.binaryExecutable,
                status.binaryPath.value(),
                status.primaryPluginPath.value(),
                status.fallbackPluginPath.value(),
                status.continuityPluginPath.value(),
                status.mcpConfigurationPath.value(),
                status.deploymentId,
                connectionCheckPerformed,
                primaryReady,
                fallbackReady,
                continuityReady,
                false,
                sources.continuityAutomation == nullptr
                    ? 0U
                    : sources.continuityAutomation->trackedProjectCount(),
                status.detail,
                std::move(actionDetail)});
    }

    [[nodiscard]] Domain::Result<ManagerToolsSnapshot> toolsSnapshot() const
    {
        if (telemetrySources_.tools == nullptr) {
            return Domain::Result<ManagerToolsSnapshot>::failure(error(
                Domain::ErrorCodes::InvalidRequest,
                "The native tool catalog is unavailable in this Manager composition."));
        }
        const auto catalog = telemetrySources_.tools->tools();
        if (catalog.size() > 64U) {
            return Domain::Result<ManagerToolsSnapshot>::failure(error(
                Domain::ErrorCodes::LimitExceeded,
                "The native tool catalog exceeds the Manager projection bound."));
        }
        ManagerToolsSnapshot snapshot;
        snapshot.shellEnabled = telemetrySources_.shellEnabled;
        snapshot.tools.reserve(catalog.size());
        for (const auto& item : catalog) {
            snapshot.tools.push_back(ManagerToolDescriptor{
                item.tool.name,
                item.tool.description,
                item.tool.pack,
                item.tool.effect,
                item.tool.availability,
                item.tool.requiresProject,
                item.tool.requiresShell,
                item.inputSchema});
        }
        return Domain::Result<ManagerToolsSnapshot>::success(std::move(snapshot));
    }

    [[nodiscard]] Domain::Result<ManagerToolOutcomeSnapshot> invokeTool(
        const ManagerRequest& managerRequest,
        const ManagerToolInvokeRequest& request,
        const Domain::OperationContext& context)
    {
        if (telemetrySources_.projectWorkspaceAuthority == nullptr ||
            telemetrySources_.toolRouter == nullptr) {
            return Domain::Result<ManagerToolOutcomeSnapshot>::failure(error(
                Domain::ErrorCodes::InvalidRequest,
                "Native tool execution is unavailable in this Manager composition."));
        }
        auto authority = telemetrySources_.projectWorkspaceAuthority->authorityFor(
            request.projectId, context);
        if (!authority) {
            return Domain::Result<ManagerToolOutcomeSnapshot>::failure(
                std::move(authority).error());
        }
        Domain::ToolCallRequest call{
            Domain::McpRequestMetadata{
                managerRequest.requestId,
                context.correlationId,
                authority.value().callerId(),
                request.projectId,
                "2025-11-25"},
            request.toolName,
            request.canonicalArguments};
        auto outcome = telemetrySources_.toolRouter->invoke(
            call, authority.value(), context);
        if (!outcome) {
            return Domain::Result<ManagerToolOutcomeSnapshot>::failure(
                std::move(outcome).error());
        }
        return Domain::Result<ManagerToolOutcomeSnapshot>::success(
            ManagerToolOutcomeSnapshot{
                request.projectId,
                request.toolName,
                outcome.value().receipt.ok,
                std::move(outcome.value().canonicalPayload),
                std::move(outcome.value().receipt.error),
                outcome.value().receipt.elapsed});
    }

    [[nodiscard]] Domain::Result<ManagerOperationalSnapshot> operationalSnapshot(
        const ManagerOperationalRequest& request,
        const Domain::OperationContext& context)
    {
        if (telemetrySources_.operational == nullptr) {
            return Domain::Result<ManagerOperationalSnapshot>::failure(error(
                Domain::ErrorCodes::InvalidRequest,
                "Operational pages are unavailable in this Manager composition."));
        }
        auto& service = *telemetrySources_.operational;
        std::vector<std::string> lines;
        if (request.area == ManagerOperationalArea::Runs) {
            if (request.action != ManagerOperationalAction::Inspect ||
                !request.projectId || !managedRuns_) {
                return Domain::Result<ManagerOperationalSnapshot>::failure(error(
                    Domain::ErrorCodes::InvalidRequest,
                    "A project-bound managed-run history inspection is required."));
            }
            auto sessions = service.sessions(context);
            if (!sessions) return Domain::Result<ManagerOperationalSnapshot>::failure(
                std::move(sessions).error());
            std::set<std::string> seen;
            const auto append = [&](const Domain::AgentSession& session)
                -> Domain::Result<void> {
                if (session.agentId.value() != "forge-managed-run" ||
                    !seen.insert(session.id.value()).second) {
                    return Domain::Result<void>::success();
                }
                auto run = managedRuns_->status(session.id, context);
                if (!run) return Domain::Result<void>::failure(std::move(run).error());
                const auto& record = run.value().record;
                if (record.projectId != *request.projectId) {
                    return Domain::Result<void>::success();
                }
                const char* state = "unknown";
                switch (record.state) {
                case Domain::ManagedRunState::Running: state = "running"; break;
                case Domain::ManagedRunState::Paused: state = "paused"; break;
                case Domain::ManagedRunState::Cancelling: state = "stopping"; break;
                case Domain::ManagedRunState::Completed: state = "completed"; break;
                case Domain::ManagedRunState::Failed: state = "failed"; break;
                case Domain::ManagedRunState::Cancelled: state = "stopped"; break;
                }
                lines.push_back(record.runId.value() + " · " + state + "\n" +
                    record.task);
                return Domain::Result<void>::success();
            };
            for (const auto& session : sessions.value().open) {
                auto appended = append(session);
                if (!appended) return Domain::Result<ManagerOperationalSnapshot>::failure(
                    std::move(appended).error());
            }
            for (const auto& session : sessions.value().recent) {
                auto appended = append(session);
                if (!appended) return Domain::Result<ManagerOperationalSnapshot>::failure(
                    std::move(appended).error());
            }
            return Domain::Result<ManagerOperationalSnapshot>::success(
                {request.area, "Recent Manager-owned runs · selected project", std::move(lines)});
        }
        if (request.action == ManagerOperationalAction::PruneSessions) {
            auto pruned = service.pruneSessions(context);
            if (!pruned) return Domain::Result<ManagerOperationalSnapshot>::failure(
                std::move(pruned).error());
            lines.push_back("Closed stale sessions: " + std::to_string(pruned.value()));
        } else if (request.action == ManagerOperationalAction::CloseSession) {
            if (!request.sessionId) {
                return Domain::Result<ManagerOperationalSnapshot>::failure(error(
                    Domain::ErrorCodes::InvalidRequest,
                    "A session ID is required to close a session."));
            }
            auto closed = service.closeSession(
                Dashboard::DashboardSessionCloseRequest{
                    *request.sessionId,
                    request.summary.empty() ? "Closed from native app" : request.summary},
                context);
            if (!closed) return Domain::Result<ManagerOperationalSnapshot>::failure(
                std::move(closed).error());
            lines.push_back("Closed session " + closed.value().id.value() + ".");
        }

        if (request.area == ManagerOperationalArea::Agents) {
            auto agents = service.agents(context);
            auto sessions = service.sessions(context);
            if (!agents) return Domain::Result<ManagerOperationalSnapshot>::failure(
                std::move(agents).error());
            if (!sessions) return Domain::Result<ManagerOperationalSnapshot>::failure(
                std::move(sessions).error());
            lines.push_back("Agent definitions: " + std::to_string(agents.value().size()));
            for (const auto& agent : agents.value()) {
                lines.push_back(agent.id.value() + " — " + agent.displayName +
                    "\n" + agent.description + "\nTools: " +
                    std::to_string(agent.tools.size()));
            }
            lines.push_back("Open sessions: " + std::to_string(sessions.value().open.size()));
            for (const auto& session : sessions.value().open) {
                lines.push_back(session.id.value() + " · " + session.agentId.value() +
                    " · " + std::string{Domain::wireName(session.status)} +
                    (session.clientId ? " · client " + session.clientId->value() : "") +
                    (session.summary ? "\n" + *session.summary : ""));
            }
            lines.push_back("Recent sessions: " + std::to_string(sessions.value().recent.size()));
            for (const auto& session : sessions.value().recent) {
                lines.push_back(session.id.value() + " · " + session.agentId.value() +
                    " · " + std::string{Domain::wireName(session.status)} +
                    " · recent" +
                    (session.clientId ? " · client " + session.clientId->value() : "") +
                    (session.summary ? "\n" + *session.summary : ""));
            }
            return Domain::Result<ManagerOperationalSnapshot>::success(
                {request.area, "Agents and sessions", std::move(lines)});
        }
        if (request.area == ManagerOperationalArea::Feed) {
            auto audit = service.audit(context);
            if (!audit) return Domain::Result<ManagerOperationalSnapshot>::failure(
                std::move(audit).error());
            for (const auto& event : audit.value()) {
                const auto seconds = std::chrono::system_clock::to_time_t(event.timestamp);
                std::tm utc{};
                gmtime_s(&utc, &seconds);
                char timestamp[32]{};
                std::strftime(timestamp, sizeof(timestamp), "%Y-%m-%d %H:%M:%S UTC", &utc);
                lines.push_back(std::string{timestamp} + " · " + event.tool + " · " + event.status +
                    (event.clientId ? " · " + event.clientId->value() : "") +
                    (event.duration ? " · " + std::to_string(event.duration->count()) + " ms" : "") +
                    (event.error ? "\n" + *event.error : ""));
            }
            return Domain::Result<ManagerOperationalSnapshot>::success(
                {request.area, "Recent activity", std::move(lines)});
        }
        if (request.area == ManagerOperationalArea::Diagnostics) {
            auto doctor = service.doctor(context);
            auto diagnosticLines = service.diagnosticLines(context);
            if (!doctor) return Domain::Result<ManagerOperationalSnapshot>::failure(
                std::move(doctor).error());
            lines.push_back(std::string{"Overall health: "} + (doctor.value().ok ? "healthy" : "attention required"));
            for (const auto& check : doctor.value().checks) {
                lines.push_back(std::string{check.ok ? "PASS " : "FAIL "} + check.name + " — " + check.detail);
            }
            if (diagnosticLines) {
                for (const auto& line : diagnosticLines.value()) lines.push_back(line);
            }
            return Domain::Result<ManagerOperationalSnapshot>::success(
                {request.area, "Diagnostics", std::move(lines)});
        }
        auto status = service.status(context);
        if (!status) return Domain::Result<ManagerOperationalSnapshot>::failure(
            std::move(status).error());
        const auto& runtime = status.value().runtimeDiagnostics;
        lines.push_back("Owned operations: " + std::to_string(runtime.ownedOperations));
        lines.push_back("Background threads: " + std::to_string(runtime.backgroundThreads));
        lines.push_back("Child processes: " + std::to_string(runtime.childProcesses));
        lines.push_back("Open repositories/databases: " +
            std::to_string(runtime.openRepositories) + "/" +
            std::to_string(runtime.openDatabases));
        if (request.area == ManagerOperationalArea::Runtimes) {
            auto settings = controller_->settings(context);
            if (!settings) return Domain::Result<ManagerOperationalSnapshot>::failure(
                std::move(settings).error());
            lines.push_back(std::string{"Effective shell policy: "} +
                (settings.value().shellEnabled ? "enabled" : "disabled"));
            lines.push_back("Job inventory: not exposed by this Manager operational projection");
        }
        if (request.area == ManagerOperationalArea::Manager) {
            auto manager = controller_->status(context);
            if (!manager) return Domain::Result<ManagerOperationalSnapshot>::failure(
                std::move(manager).error());
            lines.insert(lines.begin(), "Manager PID " + std::to_string(manager.value().processId) +
                " · service " + (manager.value().serviceActive ? "active" : "inactive"));
        }
        return Domain::Result<ManagerOperationalSnapshot>::success(
            {request.area,
             request.area == ManagerOperationalArea::Runtimes ? "Runtimes" : "Manager",
             std::move(lines)});
    }

    [[nodiscard]] Domain::Result<ManagerMaintenanceSnapshot> resetData(
        const ManagerMaintenanceRequest& request,
        const Domain::OperationContext& context)
    {
        if (telemetrySources_.projectMemory == nullptr ||
            telemetrySources_.continuity == nullptr ||
            telemetrySources_.projects == nullptr) {
            return Domain::Result<ManagerMaintenanceSnapshot>::failure(
                Domain::makeError(
                    Domain::ErrorCodes::InvalidRequest,
                    "Data maintenance is unavailable in this Manager composition."));
        }

        auto& memory = *telemetrySources_.projectMemory;
        auto& continuity = *telemetrySources_.continuity;
        auto resetProject = [&](const Domain::ProjectId& projectId,
                                const ManagerMaintenanceScope scope,
                                const std::string& suppliedToken)
            -> Domain::Result<ManagerMaintenanceSnapshot> {
            const auto id = projectId.value();
            const auto confirmation = [&](const std::string& action,
                                          const std::string& token) {
                return Domain::DestructiveConfirmation{action, id, token};
            };
            ManagerMaintenanceSnapshot snapshot{scope, id};
            if (scope == ManagerMaintenanceScope::ProjectMemory) {
                auto report = memory.resetProjectMemory(
                    projectId,
                    confirmation("reset_project_memory", suppliedToken), context);
                if (!report) return Domain::Result<ManagerMaintenanceSnapshot>::failure(
                    std::move(report).error());
                snapshot.projectsAffected = report.value().projectsAffected;
                snapshot.recordsRemoved = report.value().recordsRemoved;
                snapshot.linksRemoved = report.value().linksRemoved;
                snapshot.eventsRemoved = report.value().eventsRemoved;
                snapshot.verified = report.value().verified;
                snapshot.detail = "Project memory reset completed and its repository generation was closed.";
                return Domain::Result<ManagerMaintenanceSnapshot>::success(
                    std::move(snapshot));
            }
            if (scope == ManagerMaintenanceScope::ProjectContinuity) {
                auto report = continuity.resetProjectContinuity(
                    Domain::ContinuityResetRequest{
                        projectId,
                        confirmation("reset_project_continuity", suppliedToken)},
                    context);
                if (!report) return Domain::Result<ManagerMaintenanceSnapshot>::failure(
                    std::move(report).error());
                auto closed = memory.closeProject(projectId, context);
                if (!closed) return Domain::Result<ManagerMaintenanceSnapshot>::failure(
                    std::move(closed).error());
                snapshot.projectsAffected = report.value().report.projectsAffected;
                snapshot.recordsRemoved = report.value().report.recordsRemoved;
                snapshot.linksRemoved = report.value().report.linksRemoved;
                snapshot.eventsRemoved = report.value().report.eventsRemoved;
                snapshot.verified = report.value().report.verified;
                snapshot.detail = "Project continuity reset completed and the old repository generation was closed.";
                return Domain::Result<ManagerMaintenanceSnapshot>::success(
                    std::move(snapshot));
            }

            const auto expected = "RESET PROJECT DATA " + id;
            auto valid = Domain::validateDestructiveConfirmation(
                confirmation("reset_project_data", suppliedToken),
                "reset_project_data", id, expected);
            if (!valid) return Domain::Result<ManagerMaintenanceSnapshot>::failure(
                std::move(valid).error());
            auto continuityReport = continuity.resetProjectContinuity(
                Domain::ContinuityResetRequest{
                    projectId,
                    confirmation(
                        "reset_project_continuity",
                        "RESET PROJECT CONTINUITY " + id)},
                context);
            if (!continuityReport) {
                return Domain::Result<ManagerMaintenanceSnapshot>::failure(
                    std::move(continuityReport).error());
            }
            auto memoryReport = memory.resetProjectMemory(
                projectId,
                confirmation("reset_project_memory", "RESET PROJECT MEMORY " + id),
                context);
            if (!memoryReport) {
                auto failure = std::move(memoryReport).error();
                failure.message += " Continuity reset had already committed for this project.";
                return Domain::Result<ManagerMaintenanceSnapshot>::failure(
                    std::move(failure));
            }
            snapshot.projectsAffected = 1U;
            snapshot.recordsRemoved = continuityReport.value().report.recordsRemoved +
                memoryReport.value().recordsRemoved;
            snapshot.linksRemoved = continuityReport.value().report.linksRemoved +
                memoryReport.value().linksRemoved;
            snapshot.eventsRemoved = continuityReport.value().report.eventsRemoved +
                memoryReport.value().eventsRemoved;
            snapshot.verified = continuityReport.value().report.verified &&
                memoryReport.value().verified;
            snapshot.detail = "Project memory and continuity reset completed; the old repository generation was closed.";
            return Domain::Result<ManagerMaintenanceSnapshot>::success(
                std::move(snapshot));
        };

        if (request.scope != ManagerMaintenanceScope::AllProjectsAllData) {
            if (!request.projectId) {
                return Domain::Result<ManagerMaintenanceSnapshot>::failure(
                    Domain::makeError(
                        Domain::ErrorCodes::InvalidRequest,
                        "A project maintenance reset requires an exact project ID."));
            }
            return resetProject(*request.projectId, request.scope,
                request.confirmationToken);
        }
        if (request.projectId) {
            return Domain::Result<ManagerMaintenanceSnapshot>::failure(
                Domain::makeError(
                    Domain::ErrorCodes::InvalidRequest,
                    "An all-project reset cannot include a project ID."));
        }
        auto valid = Domain::validateDestructiveConfirmation(
            Domain::DestructiveConfirmation{
                "reset_all_project_data", "all-projects", request.confirmationToken},
            "reset_all_project_data", "all-projects", "RESET ALL PROJECT DATA");
        if (!valid) return Domain::Result<ManagerMaintenanceSnapshot>::failure(
            std::move(valid).error());
        auto listed = telemetrySources_.projects->list(1'024U, context);
        if (!listed) return Domain::Result<ManagerMaintenanceSnapshot>::failure(
            std::move(listed).error());
        ManagerMaintenanceSnapshot aggregate{
            request.scope, "all-projects", 0U, 0U, 0U, 0U, true,
            "All registered project memory and continuity stores were reset."};
        for (const auto& project : listed.value()) {
            auto report = resetProject(
                project.id, ManagerMaintenanceScope::ProjectAllData,
                "RESET PROJECT DATA " + project.id.value());
            if (!report) {
                auto failure = std::move(report).error();
                failure.message += " " + std::to_string(aggregate.projectsAffected) +
                    " earlier project reset(s) remain committed.";
                return Domain::Result<ManagerMaintenanceSnapshot>::failure(
                    std::move(failure));
            }
            ++aggregate.projectsAffected;
            aggregate.recordsRemoved += report.value().recordsRemoved;
            aggregate.linksRemoved += report.value().linksRemoved;
            aggregate.eventsRemoved += report.value().eventsRemoved;
            aggregate.verified = aggregate.verified && report.value().verified;
        }
        return Domain::Result<ManagerMaintenanceSnapshot>::success(
            std::move(aggregate));
    }

    [[nodiscard]] ManagerResponse dispatchRegular(
        const ManagerRequest& request,
        const Domain::OperationContext& context)
    {
        return std::visit(
            [&](const auto& payload) -> ManagerResponse {
                using Payload = std::remove_cvref_t<decltype(payload)>;
                if constexpr (std::is_same_v<Payload, ManagerStatusRequest>) {
                    return controllerResponse(
                        request, controller_->status(context));
                } else if constexpr (
                    std::is_same_v<Payload, ManagerSettingsRequest>) {
                    return controllerResponse(
                        request, controller_->settings(context));
                } else if constexpr (
                    std::is_same_v<Payload, ManagerTelemetryRequest>) {
                    return controllerResponse(
                        request, telemetrySnapshot(payload, context));
                } else if constexpr (
                    std::is_same_v<Payload, ManagerProjectsListRequest>) {
                    if (telemetrySources_.projects == nullptr) {
                        return responseWithError(
                            request,
                            error(
                                Domain::ErrorCodes::InvalidRequest,
                                "Project registration is unavailable in this Manager composition."));
                    }
                    if (payload.maximumCount == 0U || payload.maximumCount > 1'024U) {
                        return responseWithError(
                            request,
                            error(
                                Domain::ErrorCodes::InvalidRequest,
                                "Project result count must be within 1 through 1024."));
                    }
                    auto projects = telemetrySources_.projects->list(
                        payload.maximumCount, context);
                    if (!projects) {
                        return responseWithError(request, std::move(projects).error());
                    }
                    return responseWithResult(
                        request,
                        ManagerProjectsSnapshot{std::move(projects).value()});
                } else if constexpr (
                    std::is_same_v<Payload, ManagerProjectInitializeRequest>) {
                    if (telemetrySources_.projectMemory == nullptr) {
                        return responseWithError(
                            request,
                            error(
                                Domain::ErrorCodes::InvalidRequest,
                                "Project registration is unavailable in this Manager composition."));
                    }
                    auto initialized = telemetrySources_.projectMemory->initialize(
                        Domain::InitializeProjectRequest{
                            payload.projectPath,
                            std::nullopt,
                            payload.displayName,
                            payload.repositoryIdentity,
                            std::nullopt},
                        context);
                    if (!initialized) {
                        return responseWithError(
                            request, std::move(initialized).error());
                    }
                    return controllerResponse(
                        request,
                        projectWorkspace(
                            initialized.value().project.id, {}, 20U,
                            std::nullopt, context));
                } else if constexpr (
                    std::is_same_v<Payload, ManagerProjectMemoryRequest>) {
                    return controllerResponse(
                        request,
                        projectWorkspace(
                            payload.projectId, payload.query,
                            payload.maximumCount, std::nullopt, context));
                } else if constexpr (
                    std::is_same_v<Payload, ManagerProjectRememberRequest>) {
                    if (telemetrySources_.projectMemory == nullptr) {
                        return responseWithError(
                            request,
                            error(
                                Domain::ErrorCodes::InvalidRequest,
                                "Project memory is unavailable in this Manager composition."));
                    }
                    Domain::ProjectMemoryWrite write;
                    write.kind = "note";
                    write.title = payload.title;
                    write.summary = payload.summary;
                    write.body = payload.body;
                    write.tags = payload.tags;
                    write.sourceKind = "native_gui";
                    auto remembered = telemetrySources_.projectMemory->remember(
                        Domain::RememberProjectMemoryRequest{
                            payload.projectId, std::move(write)},
                        context);
                    if (!remembered) {
                        return responseWithError(
                            request, std::move(remembered).error());
                    }
                    return controllerResponse(
                        request,
                        projectWorkspace(
                            payload.projectId, {}, 20U,
                            remembered.value().recordId, context));
                } else if constexpr (
                    std::is_same_v<Payload, ManagerLmStudioStatusRequest>) {
                    return controllerResponse(
                        request, lmStudioWorkflow(request, false, false, context));
                } else if constexpr (
                    std::is_same_v<Payload, ManagerLmStudioRepairRequest>) {
                    return controllerResponse(
                        request, lmStudioWorkflow(request, true, false, context));
                } else if constexpr (
                    std::is_same_v<Payload, ManagerLmStudioActivateRequest>) {
                    return controllerResponse(
                        request, lmStudioWorkflow(request, false, true, context));
                } else if constexpr (
                    std::is_same_v<Payload, ManagerToolsRequest>) {
                    return controllerResponse(request, toolsSnapshot());
                } else if constexpr (
                    std::is_same_v<Payload, ManagerToolInvokeRequest>) {
                    return controllerResponse(
                        request, invokeTool(request, payload, context));
                } else if constexpr (
                    std::is_same_v<Payload, ManagerOperationalRequest>) {
                    return controllerResponse(
                        request, operationalSnapshot(payload, context));
                } else if constexpr (
                    std::is_same_v<Payload, ManagerMaintenanceRequest>) {
                    return controllerResponse(
                        request, resetData(payload, context));
                } else if constexpr (
                    std::is_same_v<Payload, Domain::ManagerControlRequest>) {
                    return controllerResponse(
                        request, controller_->control(payload, context));
                } else if constexpr (
                    std::is_same_v<Payload, ManagerSettingsUpdateRequest>) {
                    auto outcome = controller_->updateSettings(
                        payload.patch,
                        payload.applyImmediately,
                        context);
                    return controllerResponse(
                        request,
                        std::move(outcome));
                } else if constexpr (
                    std::is_same_v<Payload, ManagedRunStartRequest>) {
                    if (!managedRuns_) {
                        return responseWithError(
                            request,
                            error(
                                Domain::ErrorCodes::InvalidRequest,
                                "Managed runs are unavailable in this Manager composition."));
                    }
                    return controllerResponse(
                        request,
                        managedRuns_->start(
                            Domain::ManagedRunStartRequest{
                                payload.runId,
                                payload.projectId,
                                payload.clientId,
                                context.operationId,
                                context.correlationId,
                                payload.authorityGeneration,
                                payload.task},
                            context));
                } else if constexpr (
                    std::is_same_v<Payload, ManagedRunStatusRequest>) {
                    if (!managedRuns_) {
                        return responseWithError(
                            request,
                            error(
                                Domain::ErrorCodes::InvalidRequest,
                                "Managed runs are unavailable in this Manager composition."));
                    }
                    return controllerResponse(
                        request,
                        managedRuns_->status(payload.runId, context));
                } else if constexpr (
                    std::is_same_v<Payload, ManagedRunCancelRequest>) {
                    if (!managedRuns_) {
                        return responseWithError(
                            request,
                            error(
                                Domain::ErrorCodes::InvalidRequest,
                                "Managed runs are unavailable in this Manager composition."));
                    }
                    return controllerResponse(
                        request,
                        managedRuns_->cancel(payload.runId, context));
                } else if constexpr (
                    std::is_same_v<Payload, ManagedRunPauseRequest>) {
                    if (!managedRuns_) {
                        return responseWithError(
                            request,
                            error(
                                Domain::ErrorCodes::InvalidRequest,
                                "Managed runs are unavailable in this Manager composition."));
                    }
                    return controllerResponse(
                        request,
                        managedRuns_->pause(payload.runId, context));
                } else if constexpr (
                    std::is_same_v<Payload, ManagedRunResumeRequest>) {
                    if (!managedRuns_) {
                        return responseWithError(
                            request,
                            error(
                                Domain::ErrorCodes::InvalidRequest,
                                "Managed runs are unavailable in this Manager composition."));
                    }
                    return controllerResponse(
                        request,
                        managedRuns_->resume(payload.runId, context));
                } else {
                    return responseWithError(
                        request,
                        error(
                            Domain::ErrorCodes::InvalidRequest,
                            "The manager control request was dispatched incorrectly."));
                }
            },
            request.payload);
    }

    [[nodiscard]] ManagerResponse dispatchShutdown(
        const ManagerRequest& request,
        Domain::OperationId operationId,
        const Domain::MonotonicTimePoint deadline)
    {
        std::stop_source cancellation;
        const Domain::OperationContext context{
            std::move(operationId),
            deadline,
            cancellation.get_token(),
            request.correlationId};
        beginShutdown();

        const auto remaining = nonnegativeRemaining(
            deadline, clock_->monotonicNow());
        const auto drain = (std::min)(
            limits_.shutdownDrainTimeout, remaining);
        static_cast<void>(waitUntilIdle(drain));
        if (auto current = validateContext(context); !current) {
            return responseWithError(request, std::move(current).error());
        }

        auto shutdownResult = controller_->requestShutdown(context);
        if (!shutdownResult) {
            return responseWithError(
                request, std::move(shutdownResult).error());
        }
        return acknowledgement(request);
    }

    std::shared_ptr<Contracts::IManagerController> controller_;
    std::shared_ptr<Contracts::IClock> clock_;
    ManagerTransportLimits limits_;
    std::shared_ptr<Contracts::IManagedRunService> managedRuns_;
    ManagerTelemetrySources telemetrySources_;

    mutable std::mutex stateMutex_;
    std::condition_variable stateChanged_;
    std::map<Domain::OperationId, std::shared_ptr<ActiveOperation>>
        activeOperations_;
    bool accepting_{true};
    bool closeControllerWhenIdle_{};
    bool controllerClosed_{};
};

ManagerRequestDispatcher::ManagerRequestDispatcher(
    std::shared_ptr<Contracts::IManagerController> controller,
    std::shared_ptr<Contracts::IClock> clock,
    ManagerTransportLimits limits,
    std::shared_ptr<Contracts::IManagedRunService> managedRuns,
    ManagerTelemetrySources telemetrySources)
{
    if (!controller) {
        throw std::invalid_argument{
            "The manager request dispatcher requires a controller."};
    }
    if (!clock) {
        throw std::invalid_argument{
            "The manager request dispatcher requires a clock."};
    }
    implementation_ = std::make_shared<Implementation>(
        std::move(controller),
        std::move(clock),
        std::move(limits),
        std::move(managedRuns),
        telemetrySources);
}

ManagerRequestDispatcher::~ManagerRequestDispatcher() noexcept
{
    shutdown();
}

ManagerResponse ManagerRequestDispatcher::dispatch(
    const ManagerRequest& request) noexcept
{
    const auto implementation = implementation_;
    if (!implementation) {
        return responseWithError(
            request,
            error(
                Domain::ErrorCodes::TransportClosed,
                "The manager dispatcher has no implementation."));
    }
    return implementation->dispatch(request);
}

void ManagerRequestDispatcher::beginShutdown() noexcept
{
    if (const auto implementation = implementation_) {
        implementation->beginShutdown();
    }
}

void ManagerRequestDispatcher::cancel(
    const Domain::OperationId& operationId) noexcept
{
    if (const auto implementation = implementation_) {
        implementation->cancel(operationId);
    }
}

bool ManagerRequestDispatcher::waitUntilIdle(
    const std::chrono::milliseconds timeout) noexcept
{
    const auto implementation = implementation_;
    return !implementation || implementation->waitUntilIdle(timeout);
}

std::size_t ManagerRequestDispatcher::activeOperationCount() const noexcept
{
    const auto implementation = implementation_;
    return implementation ? implementation->activeOperationCount() : 0U;
}

bool ManagerRequestDispatcher::isAccepting() const noexcept
{
    const auto implementation = implementation_;
    return implementation && implementation->isAccepting();
}

void ManagerRequestDispatcher::shutdown() noexcept
{
    if (const auto implementation = implementation_) {
        implementation->shutdown();
    }
}

} // namespace ForgeConductor::Manager
