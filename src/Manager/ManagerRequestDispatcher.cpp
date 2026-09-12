#include "ForgeConductor/Manager/ManagerRequestDispatcher.h"

#include "ForgeConductor/Manager/ManagerDeadlineMapper.h"

#include <algorithm>
#include <condition_variable>
#include <map>
#include <memory>
#include <mutex>
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
            telemetry->history};

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
