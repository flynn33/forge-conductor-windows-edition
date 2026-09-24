#include "ForgeConductor/Manager/ManagerRequestDispatcher.h"

#include "ForgeConductor/Manager/ManagerDeadlineMapper.h"
#include "ForgeConductor/Dashboard/DashboardSessionCloseRequest.h"
#include "ForgeConductor/Domain/Utf8.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <cctype>
#include <condition_variable>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <span>
#include <sstream>
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

// Recent heartbeat rows are claims, not proof of a currently connected host.
// Match both a live PID and the exact packaged CLI image before displaying one.
[[nodiscard]] bool liveExpectedMcpProcess(
    const std::uint32_t processId,
    const Domain::PathText& expectedBinary) noexcept
{
    const auto& utf8 = expectedBinary.value();
    const int expectedLength = ::MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(),
        static_cast<int>(utf8.size()), nullptr, 0);
    if (expectedLength <= 0) {
        return false;
    }
    wchar_t expected[Domain::PathText::MaximumBytes + 1U]{};
    if (::MultiByteToWideChar(
            CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(),
            static_cast<int>(utf8.size()), expected,
            static_cast<int>(sizeof(expected) / sizeof(expected[0]))) !=
        expectedLength) {
        return false;
    }
    const HANDLE process = ::OpenProcess(
        PROCESS_QUERY_LIMITED_INFORMATION, FALSE, processId);
    if (process == nullptr) {
        return false;
    }
    DWORD exitCode{};
    wchar_t actual[Domain::PathText::MaximumBytes + 1U]{};
    DWORD actualLength = static_cast<DWORD>(
        sizeof(actual) / sizeof(actual[0]));
    const bool live = ::GetExitCodeProcess(process, &exitCode) != FALSE &&
        exitCode == STILL_ACTIVE;
    const bool queried = live &&
        ::QueryFullProcessImageNameW(
            process, 0, actual, &actualLength) != FALSE;
    ::CloseHandle(process);
    return queried && ::CompareStringOrdinal(
        expected, expectedLength, actual,
        static_cast<int>(actualLength), TRUE) == CSTR_EQUAL;
}

struct InstructionPackageFile final {
    std::string relativePath;
    std::string content;
};

struct ScannedInstructionPackage final {
    std::string name;
    Domain::PathText path;
    Domain::Sha256Digest revision;
    std::vector<InstructionPackageFile> files;
    std::size_t ignoredFileCount{};
    std::uint64_t contentBytes{};
};

[[nodiscard]] std::wstring utf8Path(const std::string_view value)
{
    if (value.empty()) return {};
    const auto count = ::MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
        static_cast<int>(value.size()), nullptr, 0);
    if (count <= 0) {
        throw std::runtime_error{"The instruction package path is not valid UTF-8."};
    }
    std::wstring result(static_cast<std::size_t>(count), L'\0');
    if (::MultiByteToWideChar(
            CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
            static_cast<int>(value.size()), result.data(), count) != count) {
        throw std::runtime_error{"The instruction package path could not be decoded."};
    }
    return result;
}

[[nodiscard]] std::string pathUtf8(const std::filesystem::path& value)
{
    const auto native = value.wstring();
    if (native.empty()) return {};
    const auto count = ::WideCharToMultiByte(
        CP_UTF8, WC_ERR_INVALID_CHARS, native.data(),
        static_cast<int>(native.size()), nullptr, 0, nullptr, nullptr);
    if (count <= 0) {
        throw std::runtime_error{"An instruction package file name is not valid Unicode."};
    }
    std::string result(static_cast<std::size_t>(count), '\0');
    if (::WideCharToMultiByte(
            CP_UTF8, WC_ERR_INVALID_CHARS, native.data(),
            static_cast<int>(native.size()), result.data(), count,
            nullptr, nullptr) != count) {
        throw std::runtime_error{"An instruction package file name could not be encoded."};
    }
    std::replace(result.begin(), result.end(), '\\', '/');
    return result;
}

[[nodiscard]] bool supportedInstructionExtension(std::string extension)
{
    std::transform(
        extension.begin(), extension.end(), extension.begin(),
        [](const unsigned char character) {
            return static_cast<char>(std::tolower(character));
        });
    return extension == ".md" || extension == ".txt" ||
        extension == ".json" || extension == ".yaml" ||
        extension == ".yml" || extension == ".toml" ||
        extension == ".csv";
}

[[nodiscard]] std::span<const std::byte> byteView(
    const std::string& value) noexcept
{
    return std::as_bytes(std::span{value.data(), value.size()});
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

        auto activeManifestPage = telemetrySources_.projectMemory->listRecent(
            Domain::ListRecentProjectMemoryRequest{
                projectId, {"instruction_package"}, std::nullopt, 1U,
                std::nullopt, true, 64U * 1024U},
            context);
        if (!activeManifestPage) {
            return Domain::Result<ManagerProjectWorkspaceSnapshot>::failure(
                std::move(activeManifestPage).error());
        }
        if (activeManifestPage.value().projectId != projectId ||
            std::any_of(
                activeManifestPage.value().records.begin(),
                activeManifestPage.value().records.end(),
                [&](const Domain::MemorySearchHit& hit) {
                    return hit.record.projectId != projectId;
                })) {
            return Domain::Result<ManagerProjectWorkspaceSnapshot>::failure(
                error(
                    Domain::ErrorCodes::ProjectScopeMismatch,
                    "The active instruction manifest returned a different project identity."));
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

        std::optional<ManagerProjectMemoryRecord> activeInstructionManifest;
        if (!activeManifestPage.value().records.empty()) {
            auto& record = activeManifestPage.value().records.front().record;
            activeInstructionManifest = ManagerProjectMemoryRecord{
                record.id,
                record.version,
                std::move(record.kind),
                std::move(record.title),
                std::move(record.summary),
                std::move(record.body),
                std::move(record.tags),
                record.updatedAt};
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
                std::move(activeInstructionManifest),
                std::move(page.value().nextCursor),
                page.value().truncated,
                std::move(writtenRecordId)});
    }

    [[nodiscard]] Domain::Result<ScannedInstructionPackage>
    scanInstructionPackage(
        const ManagerInstructionPackageRequest& request,
        const Domain::OperationContext& context)
    {
        constexpr std::size_t maximumFiles = 32U;
        constexpr std::uint64_t maximumContentBytes = 212U * 1024U;
        constexpr std::uint64_t maximumFileBytes = 256U * 1024U;
        if (telemetrySources_.projects == nullptr ||
            telemetrySources_.projectMemory == nullptr ||
            telemetrySources_.evidenceHasher == nullptr) {
            return Domain::Result<ScannedInstructionPackage>::failure(error(
                Domain::ErrorCodes::InvalidRequest,
                "Instruction packages are unavailable in this Manager composition."));
        }
        if (auto current = validateContext(context); !current) {
            return Domain::Result<ScannedInstructionPackage>::failure(
                std::move(current).error());
        }
        auto descriptor = telemetrySources_.projects->descriptor(
            request.projectId, context);
        if (!descriptor) {
            return Domain::Result<ScannedInstructionPackage>::failure(
                std::move(descriptor).error());
        }
        try {
            const std::filesystem::path root{utf8Path(request.packagePath.value())};
            std::error_code pathError;
            const auto canonicalRoot = std::filesystem::weakly_canonical(
                root, pathError);
            if (pathError || !std::filesystem::is_directory(canonicalRoot, pathError) ||
                pathError) {
                return Domain::Result<ScannedInstructionPackage>::failure(error(
                    Domain::ErrorCodes::InvalidRequest,
                    "Choose an existing instruction package folder."));
            }
            const auto rootAttributes = ::GetFileAttributesW(canonicalRoot.c_str());
            if (rootAttributes == INVALID_FILE_ATTRIBUTES ||
                (rootAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0U) {
                return Domain::Result<ScannedInstructionPackage>::failure(error(
                    Domain::ErrorCodes::Unauthorized,
                    "Instruction package roots cannot be links or reparse points."));
            }

            ScannedInstructionPackage scanned{
                pathUtf8(canonicalRoot.filename()),
                request.packagePath,
                Domain::Sha256Digest::parse(std::string(64U, '0')).value(),
                {}, 0U, 0U};
            std::vector<std::filesystem::path> candidates;
            for (std::filesystem::recursive_directory_iterator iterator{
                     canonicalRoot,
                     std::filesystem::directory_options::skip_permission_denied,
                     pathError}, end;
                 iterator != end; iterator.increment(pathError)) {
                if (pathError) {
                    return Domain::Result<ScannedInstructionPackage>::failure(error(
                        Domain::ErrorCodes::Unauthorized,
                        "The Manager could not enumerate the complete instruction package."));
                }
                if (context.cancellation.stop_requested()) {
                    return Domain::Result<ScannedInstructionPackage>::failure(error(
                        Domain::ErrorCodes::Cancelled,
                        "Instruction package validation was cancelled."));
                }
                const auto attributes = ::GetFileAttributesW(iterator->path().c_str());
                if (attributes == INVALID_FILE_ATTRIBUTES) {
                    return Domain::Result<ScannedInstructionPackage>::failure(error(
                        Domain::ErrorCodes::Unauthorized,
                        "The Manager could not inspect an instruction package entry."));
                }
                if ((attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0U) {
                    return Domain::Result<ScannedInstructionPackage>::failure(error(
                        Domain::ErrorCodes::Unauthorized,
                        "Instruction packages cannot contain links or reparse points."));
                }
                if (!iterator->is_regular_file(pathError)) {
                    if (pathError) {
                        return Domain::Result<ScannedInstructionPackage>::failure(error(
                            Domain::ErrorCodes::Unauthorized,
                            "The Manager could not inspect an instruction package file."));
                    }
                    continue;
                }
                if (!supportedInstructionExtension(
                        pathUtf8(iterator->path().extension()))) {
                    ++scanned.ignoredFileCount;
                    continue;
                }
                candidates.push_back(iterator->path());
            }
            std::sort(candidates.begin(), candidates.end(),
                [&](const auto& left, const auto& right) {
                    return pathUtf8(std::filesystem::relative(left, canonicalRoot)) <
                        pathUtf8(std::filesystem::relative(right, canonicalRoot));
                });
            if (candidates.empty()) {
                return Domain::Result<ScannedInstructionPackage>::failure(error(
                    Domain::ErrorCodes::InvalidRequest,
                    "The folder contains no supported instruction files."));
            }
            if (candidates.size() > maximumFiles) {
                return Domain::Result<ScannedInstructionPackage>::failure(error(
                    Domain::ErrorCodes::PayloadTooLarge,
                    "Instruction packages can contain at most 32 supported text files."));
            }

            std::string digestInput{"forge-instruction-package-v1\n"};
            scanned.files.reserve(candidates.size());
            for (const auto& candidate : candidates) {
                const auto relative = pathUtf8(
                    std::filesystem::relative(candidate, canonicalRoot, pathError));
                if (pathError || relative.empty() || relative.size() > 512U) {
                    return Domain::Result<ScannedInstructionPackage>::failure(error(
                        Domain::ErrorCodes::InvalidRequest,
                        "An instruction package relative path is invalid or too long."));
                }
                const auto size = std::filesystem::file_size(candidate, pathError);
                if (pathError || size > maximumFileBytes ||
                    scanned.contentBytes > maximumContentBytes - size) {
                    return Domain::Result<ScannedInstructionPackage>::failure(error(
                        Domain::ErrorCodes::PayloadTooLarge,
                        "Instruction package text exceeds the bounded 212 KiB package limit or 256 KiB file limit."));
                }
                std::ifstream input{candidate, std::ios::binary};
                if (!input) {
                    return Domain::Result<ScannedInstructionPackage>::failure(error(
                        Domain::ErrorCodes::Unauthorized,
                        "The Manager could not read an instruction package file."));
                }
                std::string content(
                    static_cast<std::size_t>(size), '\0');
                input.read(content.data(), static_cast<std::streamsize>(content.size()));
                if (!input && static_cast<std::size_t>(input.gcount()) != content.size()) {
                    return Domain::Result<ScannedInstructionPackage>::failure(error(
                        Domain::ErrorCodes::IntegrityFailure,
                        "An instruction package file changed while it was being read."));
                }
                if (content.starts_with("\xEF\xBB\xBF")) content.erase(0U, 3U);
                if (content.find('\0') != std::string::npos ||
                    !Domain::isValidUtf8(content)) {
                    return Domain::Result<ScannedInstructionPackage>::failure(error(
                        Domain::ErrorCodes::InvalidRequest,
                        "Instruction package files must be valid UTF-8 text without NUL bytes."));
                }
                scanned.contentBytes += content.size();
                digestInput += relative + "\n" +
                    std::to_string(content.size()) + "\n" + content + "\n";
                scanned.files.push_back(
                    InstructionPackageFile{relative, std::move(content)});
            }
            auto revision = telemetrySources_.evidenceHasher->sha256(
                byteView(digestInput));
            if (!revision) {
                return Domain::Result<ScannedInstructionPackage>::failure(
                    std::move(revision).error());
            }
            scanned.revision = std::move(revision).value();
            return Domain::Result<ScannedInstructionPackage>::success(
                std::move(scanned));
        } catch (const std::exception&) {
            return Domain::Result<ScannedInstructionPackage>::failure(error(
                Domain::ErrorCodes::InvalidRequest,
                "The Manager could not validate the selected instruction package folder."));
        }
    }

    [[nodiscard]] Domain::Result<ManagerInstructionPackageSnapshot>
    instructionPackage(
        const ManagerInstructionPackageRequest& request,
        const Domain::OperationContext& context)
    {
        auto scannedResult = scanInstructionPackage(request, context);
        if (!scannedResult) {
            return Domain::Result<ManagerInstructionPackageSnapshot>::failure(
                std::move(scannedResult).error());
        }
        auto scanned = std::move(scannedResult).value();
        if (request.activate && !request.expectedRevision) {
            return Domain::Result<ManagerInstructionPackageSnapshot>::failure(error(
                Domain::ErrorCodes::InvalidRequest,
                "Activation requires the exact validated instruction revision."));
        }
        if (!request.activate && request.expectedRevision) {
            return Domain::Result<ManagerInstructionPackageSnapshot>::failure(error(
                Domain::ErrorCodes::InvalidRequest,
                "Validation cannot supply an activation revision."));
        }
        if (request.expectedRevision &&
            *request.expectedRevision != scanned.revision) {
            return Domain::Result<ManagerInstructionPackageSnapshot>::failure(error(
                Domain::ErrorCodes::Conflict,
                "The instruction package changed after validation. Validate the folder again."));
        }
        std::vector<std::string> fileNames;
        fileNames.reserve(scanned.files.size());
        for (const auto& file : scanned.files) fileNames.push_back(file.relativePath);
        std::optional<Domain::MemoryRecordId> manifestRecordId;
        if (request.activate) {
            std::vector<Domain::ProjectMemoryWrite> writes;
            writes.reserve(scanned.files.size());
            const auto revisionTag = "revision-" +
                scanned.revision.value().substr(0U, 16U);
            for (std::size_t index{}; index < scanned.files.size(); ++index) {
                const auto& file = scanned.files[index];
                auto idempotency = Domain::IdempotencyKey::create(
                    "instruction-file:" + scanned.revision.value() + ":" +
                    std::to_string(index));
                if (!idempotency) {
                    return Domain::Result<ManagerInstructionPackageSnapshot>::failure(
                        std::move(idempotency).error());
                }
                Domain::ProjectMemoryWrite write;
                write.kind = "project_instruction";
                write.title = file.relativePath;
                write.summary = "Instruction package " + scanned.name +
                    " · revision " + scanned.revision.value().substr(0U, 16U);
                write.body = file.content;
                write.tags = {"instruction-package", revisionTag};
                write.importance = 1.0;
                write.confidence = 1.0;
                write.sourceKind = "manager_instruction_package";
                write.sourceReference = file.relativePath;
                write.idempotencyKey = std::move(idempotency).value();
                writes.push_back(std::move(write));
            }
            auto remembered = telemetrySources_.projectMemory->rememberBatch(
                Domain::RememberProjectMemoryBatchRequest{
                    request.projectId, std::move(writes)},
                context);
            if (!remembered) {
                return Domain::Result<ManagerInstructionPackageSnapshot>::failure(
                    std::move(remembered).error());
            }
            nlohmann::json manifest{
                {"schema", "forge-instruction-package-v1"},
                {"package_name", scanned.name},
                {"package_path", scanned.path.value()},
                {"revision", scanned.revision.value()},
                {"content_bytes", scanned.contentBytes},
                {"files", nlohmann::json::array()}};
            Domain::ProjectMemoryWrite manifestWrite;
            manifestWrite.kind = "instruction_package";
            manifestWrite.title = scanned.name;
            manifestWrite.summary = std::to_string(scanned.files.size()) +
                " files · " + std::to_string(scanned.contentBytes) +
                " bytes · SHA-256 " + scanned.revision.value();
            for (std::size_t index{};
                 index < remembered.value().results.size(); ++index) {
                const auto& outcome = remembered.value().results[index];
                manifest["files"].push_back({
                    {"path", scanned.files[index].relativePath},
                    {"record_id", outcome.recordId.value()}});
                manifestWrite.relatedIds.push_back(outcome.recordId);
            }
            manifestWrite.body = manifest.dump();
            manifestWrite.tags = {"active-instructions", revisionTag};
            manifestWrite.importance = 1.0;
            manifestWrite.confidence = 1.0;
            manifestWrite.sourceKind = "manager_instruction_package";
            manifestWrite.sourceReference = scanned.path.value();
            auto manifestKey = Domain::IdempotencyKey::create(
                "instruction-manifest:" + scanned.revision.value());
            if (!manifestKey) {
                return Domain::Result<ManagerInstructionPackageSnapshot>::failure(
                    std::move(manifestKey).error());
            }
            manifestWrite.idempotencyKey = std::move(manifestKey).value();
            auto manifestOutcome = telemetrySources_.projectMemory->remember(
                Domain::RememberProjectMemoryRequest{
                    request.projectId, std::move(manifestWrite)},
                context);
            if (!manifestOutcome) {
                return Domain::Result<ManagerInstructionPackageSnapshot>::failure(
                    std::move(manifestOutcome).error());
            }
            manifestRecordId = manifestOutcome.value().recordId;
        }
        return Domain::Result<ManagerInstructionPackageSnapshot>::success(
            ManagerInstructionPackageSnapshot{
                request.projectId,
                std::move(scanned.name),
                std::move(scanned.path),
                std::move(scanned.revision),
                scanned.files.size(),
                scanned.ignoredFileCount,
                scanned.contentBytes,
                std::move(fileNames),
                request.activate,
                std::move(manifestRecordId)});
    }

    [[nodiscard]] Domain::Result<std::string> managedRunTaskWithInstructions(
        const Domain::ProjectId& projectId,
        std::string task,
        const bool allowTools,
        const Domain::OperationContext& context)
    {
        if (telemetrySources_.projectMemory == nullptr ||
            task.size() >= Domain::MaximumManagedRunTaskBytes) {
            return Domain::Result<std::string>::success(std::move(task));
        }
        auto manifests = telemetrySources_.projectMemory->listRecent(
            Domain::ListRecentProjectMemoryRequest{
                projectId, {"instruction_package"}, std::nullopt, 1U,
                std::nullopt, true, 64U * 1024U},
            context);
        if (!manifests) {
            return Domain::Result<std::string>::failure(
                std::move(manifests).error());
        }
        if (manifests.value().records.empty() ||
            !manifests.value().records.front().record.body) {
            return Domain::Result<std::string>::success(std::move(task));
        }
        try {
            const auto manifest = nlohmann::json::parse(
                *manifests.value().records.front().record.body);
            if (!manifest.is_object() ||
                manifest.value("schema", std::string{}) !=
                    "forge-instruction-package-v1" ||
                !manifest.contains("files") || !manifest.at("files").is_array()) {
                return Domain::Result<std::string>::failure(error(
                    Domain::ErrorCodes::IntegrityFailure,
                    "The active instruction package manifest is malformed."));
            }
            std::vector<Domain::MemoryRecordId> ids;
            std::vector<std::pair<std::string, std::string>> index;
            for (const auto& file : manifest.at("files")) {
                if (!file.is_object() || !file.contains("path") ||
                    !file.at("path").is_string() ||
                    !file.contains("record_id") ||
                    !file.at("record_id").is_string()) {
                    return Domain::Result<std::string>::failure(error(
                        Domain::ErrorCodes::IntegrityFailure,
                        "The active instruction package file index is malformed."));
                }
                auto id = Domain::MemoryRecordId::parse(
                    file.at("record_id").get<std::string>());
                if (!id) {
                    return Domain::Result<std::string>::failure(
                        std::move(id).error());
                }
                index.emplace_back(
                    file.at("path").get<std::string>(), id.value().value());
                ids.push_back(std::move(id).value());
            }
            auto records = telemetrySources_.projectMemory->get(
                Domain::GetProjectMemoryRequest{
                    projectId, std::move(ids), true, 256U * 1024U},
                context);
            if (!records) {
                return Domain::Result<std::string>::failure(
                    std::move(records).error());
            }
            if (records.value().projectId != projectId) {
                return Domain::Result<std::string>::failure(error(
                    Domain::ErrorCodes::ProjectScopeMismatch,
                    "The active instruction package crossed project scope."));
            }
            auto priority = [](std::string value) {
                std::transform(value.begin(), value.end(), value.begin(),
                    [](const unsigned char character) {
                        return static_cast<char>(std::tolower(character));
                    });
                if (value == "agents.md") return 0;
                if (value.find("start-here") != std::string::npos) return 1;
                if (value.find("execution") != std::string::npos ||
                    value.find("instruction") != std::string::npos) return 2;
                if (value.find("readme") != std::string::npos) return 3;
                return 4;
            };
            auto ordered = std::move(records).value().records;
            std::stable_sort(ordered.begin(), ordered.end(),
                [&](const auto& left, const auto& right) {
                    return priority(left.title) < priority(right.title);
                });

            std::string enriched = "[USER MISSION]\n" + task +
                "\n\n[ACTIVE PROJECT INSTRUCTION PACKAGE]\nPackage: " +
                manifest.value("package_name", std::string{"unnamed"}) +
                "\nRevision SHA-256: " +
                manifest.value("revision", std::string{"unknown"}) +
                "\nThe Manager has bound this revision to the exact project. "
                "Follow these instructions for this run.\n";
            if (allowTools) {
                enriched += "Files not embedded below remain available through "
                    "project_memory.get using this index:\n";
                for (const auto& [path, id] : index) {
                    enriched += "- " + path + " | " + id + "\n";
                }
            }
            std::size_t embedded{};
            const std::string footer =
                "\n[END ACTIVE PROJECT INSTRUCTION PACKAGE]\n";
            for (const auto& record : ordered) {
                if (!record.body) continue;
                const auto section = "\n[INSTRUCTION FILE: " + record.title +
                    "]\n" + *record.body + "\n";
                if (enriched.size() + section.size() + footer.size() >
                    Domain::MaximumManagedRunTaskBytes) {
                    continue;
                }
                enriched += section;
                ++embedded;
            }
            enriched += "\nEmbedded " + std::to_string(embedded) + " of " +
                std::to_string(index.size()) + " instruction files." + footer;
            if (enriched.size() > Domain::MaximumManagedRunTaskBytes) {
                return Domain::Result<std::string>::failure(error(
                    Domain::ErrorCodes::PayloadTooLarge,
                    "The mission leaves no safe context budget for the active instruction package index."));
            }
            return Domain::Result<std::string>::success(std::move(enriched));
        } catch (const std::exception&) {
            return Domain::Result<std::string>::failure(error(
                Domain::ErrorCodes::IntegrityFailure,
                "The active instruction package could not be assembled safely."));
        }
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
            if (sources.lmStudioActivationAuthority == nullptr) {
                return Domain::Result<ManagerLmStudioSnapshot>::failure(error(
                    Domain::ErrorCodes::InvalidRequest,
                    "LM Studio connector activation has no Execute-scoped Manager authority."));
            }
            const auto& authority = *sources.lmStudioActivationAuthority;
            const auto deploymentId = *inspected.value().deploymentId;
            Domain::ToolCallRequest call{
                Domain::McpRequestMetadata{
                    managerRequest.requestId,
                    context.correlationId,
                    authority.callerId(),
                    authority.projectId(),
                    "2025-11-25"},
                "install-lmstudio-plugin",
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
        bool liveRoleHostObserved{};
        if (status.deploymentId && sources.clientPresence != nullptr &&
            sources.preferredForgeBinary) {
            auto recent = sources.clientPresence->recentForDeployment(
                *status.deploymentId,
                clock_->utcNow() - std::chrono::seconds{25}, context);
            if (recent) {
                connectionCheckPerformed = true;
                for (const auto& owner : recent.value()) {
                    if (!owner.processId || !liveExpectedMcpProcess(
                            *owner.processId,
                            *sources.preferredForgeBinary)) {
                        continue;
                    }
                    liveRoleHostObserved = true;
                    primaryReady |= owner.role == "primary";
                    fallbackReady |= owner.role == "fallback";
                    continuityReady |= owner.role == "clu";
                }
            } else {
                if (!actionDetail.empty()) {
                    actionDetail += " ";
                }
                actionDetail +=
                    "Live MCP role readback unavailable; no session is claimed.";
            }
        }
        bool toolAuditChecked{};
        bool primaryToolRecorded{};
        bool fallbackToolRecorded{};
        bool continuityToolRecorded{};
        std::string toolAuditDetail{
            "No deployment-scoped MCP tool audit was inspected."};
        if (status.deploymentId && sources.audit != nullptr) {
            auto recentAudit = sources.audit->recent(200U, context);
            if (!recentAudit) {
                toolAuditDetail =
                    "MCP tool audit readback unavailable; no outcome is claimed.";
            } else {
                toolAuditChecked = true;
                toolAuditDetail =
                    "No successful native MCP tool outcome is recorded for this exact deployment in the bounded audit readback.";
                for (const auto& event : recentAudit.value()) {
                    if (!event.deploymentId ||
                        *event.deploymentId != *status.deploymentId ||
                        !event.mcpRole || !event.clientId ||
                        event.status != "ok") {
                        continue;
                    }
                    bool* recorded{};
                    std::string_view lane;
                    switch (*event.mcpRole) {
                    case Domain::McpRole::Primary:
                        recorded = &primaryToolRecorded;
                        lane = "Primary";
                        break;
                    case Domain::McpRole::Fallback:
                        recorded = &fallbackToolRecorded;
                        lane = "Fallback";
                        break;
                    case Domain::McpRole::Clu:
                        recorded = &continuityToolRecorded;
                        lane = "CLU";
                        break;
                    }
                    if (recorded == nullptr || *recorded) continue;
                    *recorded = true;
                    if (toolAuditDetail.starts_with("No successful")) {
                        toolAuditDetail = "Recorded native MCP tool success: ";
                    } else {
                        toolAuditDetail += " · ";
                    }
                    toolAuditDetail += std::string{lane} + " " + event.tool;
                }
                if (primaryToolRecorded || fallbackToolRecorded ||
                    continuityToolRecorded) {
                    toolAuditDetail +=
                        ". Audit is not verified evidence or proof of the external caller.";
                }
            }
        }
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
                liveRoleHostObserved,
                sources.continuityAutomation == nullptr
                    ? 0U
                    : sources.continuityAutomation->trackedProjectCount(),
                status.detail,
                std::move(actionDetail),
                toolAuditChecked,
                primaryToolRecorded,
                fallbackToolRecorded,
                continuityToolRecorded,
                std::move(toolAuditDetail)});
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
        const ManagerRequest& managerRequest,
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
        if (request.area == ManagerOperationalArea::Evidence) {
            if (!request.projectId ||
                !telemetrySources_.durableManagedRunStore ||
                !telemetrySources_.evidenceHasher) {
                return Domain::Result<ManagerOperationalSnapshot>::failure(error(
                    Domain::ErrorCodes::InvalidRequest,
                    "An exact project and native durable evidence services are required."));
            }
            if (request.action == ManagerOperationalAction::VerifyTask) {
                std::lock_guard checkGuard{evidenceCheckMutex_};
                if (!request.sessionId || request.summary.empty() ||
                    request.summary.size() > 1'024U ||
                    !telemetrySources_.shellEnabled ||
                    !telemetrySources_.toolRouter ||
                    !telemetrySources_.projectWorkspaceAuthority) {
                    return Domain::Result<ManagerOperationalSnapshot>::failure(error(
                        Domain::ErrorCodes::InvalidRequest,
                        "An exact completed run, bounded check command and enabled native shell authority are required."));
                }
                auto loaded = telemetrySources_.durableManagedRunStore->load(
                    *request.sessionId, context);
                if (!loaded) return Domain::Result<ManagerOperationalSnapshot>::failure(
                    std::move(loaded).error());
                if (!loaded.value() ||
                    loaded.value()->projectId != *request.projectId ||
                    loaded.value()->state != Domain::ManagedRunState::Completed ||
                    loaded.value()->evidenceIntegrity !=
                        Domain::ManagedRunEvidenceIntegrity::Verified ||
                    loaded.value()->nativeTaskChecks.size() >= 8U) {
                    return Domain::Result<ManagerOperationalSnapshot>::failure(error(
                        Domain::ErrorCodes::Conflict,
                        "The exact project run is not a verified completed record with check capacity."));
                }
                auto authority = telemetrySources_.projectWorkspaceAuthority->authorityFor(
                    *request.projectId, context);
                if (!authority) return Domain::Result<ManagerOperationalSnapshot>::failure(
                    std::move(authority).error());
                const nlohmann::json arguments{
                    {"command", request.summary}, {"timeout_sec", 60}};
                Domain::ToolCallRequest call{
                    Domain::McpRequestMetadata{
                        managerRequest.requestId,
                        context.correlationId,
                        authority.value().callerId(),
                        *request.projectId,
                        "2025-11-25"},
                    "shell_exec", arguments.dump()};
                auto outcome = telemetrySources_.toolRouter->invoke(
                    call, authority.value(), context);
                if (!outcome) return Domain::Result<ManagerOperationalSnapshot>::failure(
                    std::move(outcome).error());
                nlohmann::json process;
                try {
                    process = nlohmann::json::parse(outcome.value().canonicalPayload);
                    if (!process.is_object() ||
                        !process.contains("exit_code") ||
                        !process["exit_code"].is_number_integer() ||
                        !process.contains("ok") || !process["ok"].is_boolean() ||
                        !process.contains("timed_out") ||
                        !process["timed_out"].is_boolean() ||
                        !process.contains("cancelled") ||
                        !process["cancelled"].is_boolean() ||
                        !process.contains("termination_confirmed") ||
                        !process["termination_confirmed"].is_boolean() ||
                        !process.contains("elapsed_ms") ||
                        !process["elapsed_ms"].is_number_integer() ||
                        process["elapsed_ms"].get<std::int64_t>() < 0 ||
                        (process.contains("stdout_truncated") &&
                         !process["stdout_truncated"].is_boolean()) ||
                        (process.contains("stderr_truncated") &&
                         !process["stderr_truncated"].is_boolean()) ||
                        !process.contains("stdout") ||
                        !process["stdout"].is_string() ||
                        !process.contains("stderr") ||
                        !process["stderr"].is_string() ||
                        !process.contains("command") ||
                        process["command"] != request.summary) {
                        throw std::runtime_error{"Native check result is incomplete."};
                    }
                } catch (...) {
                    return Domain::Result<ManagerOperationalSnapshot>::failure(error(
                        Domain::ErrorCodes::IntegrityFailure,
                        "The native check returned an invalid process receipt."));
                }
                const auto digest = [&](const std::string& value)
                    -> Domain::Result<Domain::Sha256Digest> {
                    return telemetrySources_.evidenceHasher->sha256(
                        std::as_bytes(std::span<const char>{
                            value.data(), value.size()}));
                };
                auto commandDigest = digest(request.summary);
                auto stdoutDigest = digest(process["stdout"].get<std::string>());
                auto stderrDigest = digest(process["stderr"].get<std::string>());
                if (!commandDigest || !stdoutDigest || !stderrDigest) {
                    return Domain::Result<ManagerOperationalSnapshot>::failure(error(
                        Domain::ErrorCodes::InternalFailure,
                        "Native check digests could not be computed."));
                }
                auto checked = std::move(*loaded.value());
                const bool timedOut = process.value("timed_out", false);
                const bool cancelled = process.value("cancelled", false);
                const bool terminated = process.value("termination_confirmed", false);
                const bool outputTruncated = process.value("stdout_truncated", false) ||
                    process.value("stderr_truncated", false);
                const int exitCode = process["exit_code"].get<int>();
                const auto elapsed = process.value("elapsed_ms", 0ULL);
                checked.nativeTaskChecks.push_back(Domain::ManagedNativeTaskCheck{
                    std::move(commandDigest).value(),
                    std::move(stdoutDigest).value(),
                    std::move(stderrDigest).value(),
                    exitCode,
                    outcome.value().receipt.ok && process.value("ok", false) &&
                        exitCode == 0 && !timedOut && !cancelled && terminated &&
                        !outputTruncated,
                    timedOut, cancelled, terminated, elapsed,
                    clock_->utcNow()});
                checked.updatedAt = clock_->utcNow();
                auto saved = telemetrySources_.durableManagedRunStore->save(
                    checked, context);
                if (!saved) return Domain::Result<ManagerOperationalSnapshot>::failure(
                    std::move(saved).error());
                return operationalSnapshot(managerRequest,
                    ManagerOperationalRequest{
                        ManagerOperationalArea::Evidence,
                        ManagerOperationalAction::Inspect,
                        std::nullopt, {}, request.projectId},
                    context);
            }
            if (request.action != ManagerOperationalAction::Inspect) {
                return Domain::Result<ManagerOperationalSnapshot>::failure(error(
                    Domain::ErrorCodes::InvalidRequest,
                    "The evidence action is not supported."));
            }
            auto sessions = service.sessions(context);
            if (!sessions) {
                return Domain::Result<ManagerOperationalSnapshot>::failure(
                    std::move(sessions).error());
            }
            std::set<std::string> seen;
            const auto hashText = [&](const std::string& source)
                -> Domain::Result<Domain::Sha256Digest> {
                return telemetrySources_.evidenceHasher->sha256(
                    std::as_bytes(std::span<const char>{
                        source.data(), source.size()}));
            };
            const auto append = [&](const Domain::AgentSession& session)
                -> Domain::Result<void> {
                if (session.agentId.value() != "forge-managed-run" ||
                    !seen.insert(session.id.value()).second) {
                    return Domain::Result<void>::success();
                }
                auto loaded = telemetrySources_.durableManagedRunStore->load(
                    session.id, context);
                if (!loaded) return Domain::Result<void>::failure(
                    std::move(loaded).error());
                if (!loaded.value()) return Domain::Result<void>::failure(error(
                    Domain::ErrorCodes::IntegrityFailure,
                    "A managed-run evidence identity has no durable record."));
                const auto& record = *loaded.value();
                if (record.projectId != *request.projectId) {
                    return Domain::Result<void>::success();
                }
                auto taskDigest = hashText(record.task);
                if (!taskDigest) return Domain::Result<void>::failure(
                    std::move(taskDigest).error());
                std::optional<Domain::Sha256Digest> outputDigest;
                if (record.outputText) {
                    auto computed = hashText(*record.outputText);
                    if (!computed) return Domain::Result<void>::failure(
                        std::move(computed).error());
                    outputDigest = std::move(computed).value();
                }
                const char* state = "unknown";
                switch (record.state) {
                case Domain::ManagedRunState::Running: state = "running"; break;
                case Domain::ManagedRunState::Cancelling: state = "stopping"; break;
                case Domain::ManagedRunState::Completed: state = "completed"; break;
                case Domain::ManagedRunState::Failed: state = "failed"; break;
                case Domain::ManagedRunState::Cancelled: state = "stopped"; break;
                case Domain::ManagedRunState::Paused: state = "paused"; break;
                }
                const char* integrity = "not_terminal";
                switch (record.evidenceIntegrity) {
                case Domain::ManagedRunEvidenceIntegrity::NotTerminal:
                    integrity = "not_terminal"; break;
                case Domain::ManagedRunEvidenceIntegrity::LegacyUnsealed:
                    integrity = "legacy_unsealed"; break;
                case Domain::ManagedRunEvidenceIntegrity::Verified:
                    integrity = "verified"; break;
                case Domain::ManagedRunEvidenceIntegrity::Mismatch:
                    integrity = "mismatch"; break;
                }
                nlohmann::json nativeCheck = nullptr;
                std::string taskVerification{"not_configured"};
                std::string taskDetail{
                    "No approved native task check was attached to this run; model text is not a verified task outcome."};
                if (!record.nativeTaskChecks.empty()) {
                    const auto& check = record.nativeTaskChecks.back();
                    nativeCheck = nlohmann::json{
                        {"kind", "operator_authorized_post_run_shell_check"},
                        {"check_count", record.nativeTaskChecks.size()},
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
                            std::chrono::milliseconds>(
                                check.checkedAt.time_since_epoch()).count()}};
                    taskVerification = integrity == std::string_view{"verified"}
                        ? check.passed ? "native_check_passed" : "native_check_failed"
                        : "record_integrity_unverified";
                    taskDetail = integrity != std::string_view{"verified"}
                        ? "The stored native check cannot be trusted while durable record integrity is unverified."
                        : check.passed
                            ? "The requested post-run native check passed. This verifies the specified check only, not every assignment requirement."
                            : "The requested post-run native check did not pass. Model text cannot override its result.";
                }
                const nlohmann::json evidence{
                    {"format", "forge-conductor-managed-run-evidence-v1"},
                    {"run_id", record.runId.value()},
                    {"project_id", record.projectId.value()},
                    {"state", state},
                    {"manager_owned", true},
                    {"provider_response_id", record.providerResponseId
                        ? nlohmann::json(record.providerResponseId->value())
                        : nlohmann::json{nullptr}},
                    {"authority_generation", record.authorityGeneration},
                    {"native_tools_allowed", record.allowTools},
                    {"input_tokens", record.inputTokens},
                    {"output_tokens", record.outputTokens},
                    {"task_sha256", taskDigest.value().value()},
                    {"stored_output_sha256", outputDigest
                        ? nlohmann::json(outputDigest->value())
                        : nlohmann::json{nullptr}},
                    {"evidence_seal_sha256", record.evidenceSeal
                        ? nlohmann::json(record.evidenceSeal->value())
                        : nlohmann::json{nullptr}},
                    {"native_record_integrity", integrity},
                    {"native_check", std::move(nativeCheck)},
                    {"task_outcome_verification", std::move(taskVerification)},
                    {"task_outcome_detail", std::move(taskDetail)}};
                lines.push_back(evidence.dump());
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
                {request.area, "Durable Manager-owned runs · exact selected project",
                    std::move(lines)});
        }
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
                    (event.projectId ? " · project " + event.projectId->value() : "") +
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
            if (!request.projectId || !managedRuns_) {
                lines.push_back("Job inventory: select an authorized project to inspect persisted managed runs");
            } else {
                auto sessions = service.sessions(context);
                if (!sessions) return Domain::Result<ManagerOperationalSnapshot>::failure(
                    std::move(sessions).error());
                std::set<std::string> seen;
                std::vector<std::string> jobs;
                std::size_t active{};
                std::size_t completed{};
                std::size_t failed{};
                std::size_t stopped{};
                const auto append = [&](const Domain::AgentSession& session)
                    -> Domain::Result<void> {
                    if (session.agentId.value() != "forge-managed-run" ||
                        !seen.insert(session.id.value()).second) {
                        return Domain::Result<void>::success();
                    }
                    auto run = managedRuns_->status(session.id, context);
                    if (!run) return Domain::Result<void>::failure(std::move(run).error());
                    const auto& record = run.value().record;
                    if (record.projectId != *request.projectId)
                        return Domain::Result<void>::success();
                    const char* state = "unknown";
                    switch (record.state) {
                    case Domain::ManagedRunState::Running: state = "running"; ++active; break;
                    case Domain::ManagedRunState::Paused: state = "paused"; ++active; break;
                    case Domain::ManagedRunState::Cancelling: state = "stopping"; ++active; break;
                    case Domain::ManagedRunState::Completed: state = "completed"; ++completed; break;
                    case Domain::ManagedRunState::Failed: state = "failed"; ++failed; break;
                    case Domain::ManagedRunState::Cancelled: state = "stopped"; ++stopped; break;
                    }
                    const auto outcome = record.lastError
                        ? "Error · " + record.lastError->message
                        : record.outputText && !record.outputText->empty()
                            ? "Result · " + *record.outputText
                            : "Result · not recorded yet";
                    jobs.push_back("Job " + record.runId.value() + " · " + state +
                        "\nMission · " + Domain::truncateAgentSummaryUtf8(record.task, 240U) +
                        "\n" + Domain::truncateAgentSummaryUtf8(outcome, 800U));
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
                lines.push_back("Job inventory: " + std::to_string(jobs.size()) +
                    " recent selected-project runs · " + std::to_string(active) +
                    " active · " + std::to_string(completed) + " completed · " +
                    std::to_string(failed) + " failed · " +
                    std::to_string(stopped) + " stopped");
                lines.insert(lines.end(), std::make_move_iterator(jobs.begin()),
                    std::make_move_iterator(jobs.end()));
            }
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
                    std::is_same_v<Payload, ManagerInstructionPackageRequest>) {
                    return controllerResponse(
                        request, instructionPackage(payload, context));
                } else if constexpr (std::is_same_v<Payload, Contracts::ProjectPolicyRequest>) {
                    if (!telemetrySources_.projectPolicy) return responseWithError(request,
                        error(Domain::ErrorCodes::InvalidRequest, "Project policy service is unavailable."));
                    auto policy = telemetrySources_.projectPolicy->execute(payload, context);
                    if (!policy) return responseWithError(request, policy.error());
                    return controllerResponse(request, Domain::Result<ManagerProjectPolicySnapshot>::success(
                        ManagerProjectPolicySnapshot{std::move(policy).value()}));
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
                        request, operationalSnapshot(request, payload, context));
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
                    auto task = managedRunTaskWithInstructions(
                        payload.projectId, payload.task,
                        payload.allowTools, context);
                    if (!task) {
                        return responseWithError(
                            request, std::move(task).error());
                    }
                    if (telemetrySources_.projectPolicy) {
                        auto policy = telemetrySources_.projectPolicy->execute(
                            {payload.projectId, Contracts::ProjectPolicyAction::Inspect}, context);
                        if (!policy) return responseWithError(request, policy.error());
                        const auto state = nlohmann::json::parse(policy.value());
                        if (state.value("adopted", false)) {
                            const auto guidance = std::string{"\n\nPROJECT DEVELOPMENT POLICY\nPinned source: "} +
                                state.value("source", "") + "\nSnapshot: " + state.value("revision", "") +
                                "\nUse project_policy.read without a path to inspect the policy and reviewed scope, then read applicable documents completely using path and offset. "
                                "Follow their governing requirements. Import is not evidence of reading or compliance. "
                                "Never claim human review or approvals on the user's behalf. "
                                "File edits are limited to reviewed paths; commands must match the reviewed shell_exec arguments exactly. " +
                                (state.value("review_accepted", false) ? "An accepted review is recorded.\n" : "Review is pending; only read-only work is permitted.\n");
                            if (task.value().size() + guidance.size() > Domain::MaximumManagedRunTaskBytes)
                                return responseWithError(request, error(Domain::ErrorCodes::PayloadTooLarge, "Task and policy guidance exceed the run input limit."));
                            task.value() += guidance;
                        }
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
                                std::move(task).value(),
                                payload.allowTools},
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
    std::mutex evidenceCheckMutex_;

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
