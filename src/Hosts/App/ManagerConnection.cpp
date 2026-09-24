#include "ManagerConnection.h"
#include "ForgeConductor/Infrastructure/Windows/DpapiSecureStorage.h"
#include "ForgeConductor/Infrastructure/Windows/SystemClock.h"
#include "ForgeConductor/Infrastructure/Windows/LMStudioResponsesTransport.h"
#include "ForgeConductor/Infrastructure/Windows/WindowsModelPreparation.h"
#include "ForgeConductor/Infrastructure/Windows/WindowsCurrentUserIdentity.h"
#include "ForgeConductor/Infrastructure/Windows/WindowsManagerAuthentication.h"
#include "ForgeConductor/Infrastructure/Windows/WindowsManagerInstanceLease.h"
#include "ForgeConductor/Infrastructure/Windows/WindowsManagerNamedPipeClient.h"
#include "ForgeConductor/Infrastructure/Windows/WindowsUuidGenerator.h"
#include "ForgeConductor/Manager/ManagerProcessExitCodes.h"
#include "ForgeConductor/Domain/Utf8.h"
#include <windows.h>
#include <winhttp.h>
#include <nlohmann/json.hpp>
#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <memory>
#include <sstream>
#include <thread>

namespace ForgeConductor::Hosts::App {
namespace W = Infrastructure::Windows;
namespace {
struct ProcessHandles final {
    PROCESS_INFORMATION value{};
    ~ProcessHandles() { if (value.hThread) CloseHandle(value.hThread); if (value.hProcess) CloseHandle(value.hProcess); }
};

struct NativeHandle final {
    HANDLE value{INVALID_HANDLE_VALUE};
    NativeHandle() = default;
    NativeHandle(const NativeHandle&) = delete;
    NativeHandle& operator=(const NativeHandle&) = delete;
    ~NativeHandle()
    {
        close();
    }
    [[nodiscard]] bool valid() const noexcept
    {
        return value != nullptr && value != INVALID_HANDLE_VALUE;
    }
    void close() noexcept
    {
        if (valid()) CloseHandle(value);
        value = INVALID_HANDLE_VALUE;
    }
};

struct InternetHandle final {
    HINTERNET value{};
    ~InternetHandle() { if (value) ::WinHttpCloseHandle(value); }
};

[[nodiscard]] std::string managerStartupDetail(
    const std::filesystem::path& path) noexcept
{
    try {
        std::ifstream input{path, std::ios::binary};
        if (!input) return {};
        std::string detail(2'048U, '\0');
        input.read(detail.data(), static_cast<std::streamsize>(detail.size()));
        detail.resize(static_cast<std::size_t>(input.gcount()));
        for (auto& character : detail) {
            if (character == '\r' || character == '\n' || character == '\t') {
                character = ' ';
            } else if (static_cast<unsigned char>(character) < 0x20U) {
                character = '?';
            }
        }
        while (!detail.empty() && detail.back() == ' ') detail.pop_back();
        const auto first = detail.find_first_not_of(' ');
        return first == std::string::npos ? std::string{} : detail.substr(first);
    } catch (...) {
        return {};
    }
}

[[nodiscard]] Domain::OperationContext operationContext(
    const std::shared_ptr<W::SystemClock>& clock,
    const std::stop_token cancellation,
    const std::chrono::milliseconds timeout = std::chrono::seconds{5})
{
    W::WindowsUuidGenerator ids;
    auto id = ids.next();
    if (!id) throw std::runtime_error{id.error().message};
    return Domain::OperationContext{
        Domain::OperationId{id.value()},
        clock->monotonicNow() + timeout,
        cancellation,
        Domain::CorrelationId::parse(id.value().value()).value()};
}

[[nodiscard]] Domain::Result<std::unique_ptr<W::WindowsManagerNamedPipeClient>>
connectManager(
    const std::optional<W::WindowsAlphaManagerProfile>& alphaProfile,
    const Domain::OperationContext& context,
    const std::shared_ptr<W::SystemClock>& clock)
{
    auto identity = W::WindowsCurrentUserIdentity::load();
    if (!identity) {
        return Domain::Result<std::unique_ptr<W::WindowsManagerNamedPipeClient>>::failure(
            identity.error());
    }
    W::WindowsManagerInstanceLeaseOptions leaseOptions;
    if (alphaProfile) leaseOptions.purposeSuffix = alphaProfile->purposeSuffix();
    auto names = W::WindowsManagerInstanceLease::namesFor(
        identity.value(), leaseOptions);
    if (!names) {
        return Domain::Result<std::unique_ptr<W::WindowsManagerNamedPipeClient>>::failure(
            names.error());
    }
    const std::wstring registrySubkey = alphaProfile
        ? std::wstring{alphaProfile->secureStorageRegistrySubkey()}
        : std::wstring{W::DpapiSecureStorage::DefaultRegistrySubkey};
    W::DpapiSecureStorage secure{registrySubkey};
    W::WindowsManagerAuthenticationTokenGenerator generator;
    W::WindowsManagerAuthenticationTokenStore tokens{secure, generator};
    auto nonce = tokens.load(context);
    if (!nonce) {
        return Domain::Result<std::unique_ptr<W::WindowsManagerNamedPipeClient>>::failure(
            nonce.error());
    }
    if (!nonce.value()) {
        return Domain::Result<std::unique_ptr<W::WindowsManagerNamedPipeClient>>::failure(
            Domain::makeError(
                Domain::ErrorCodes::SessionNotFound,
                "Manager authentication is not initialized."));
    }
    return W::WindowsManagerNamedPipeClient::create(
        clock, std::wstring{names.value().pipeName()}, *nonce.value());
}

[[nodiscard]] std::string_view managedStateName(
    const Domain::ManagedRunState state) noexcept
{
    switch (state) {
    case Domain::ManagedRunState::Running: return "running";
    case Domain::ManagedRunState::Cancelling: return "stopping";
    case Domain::ManagedRunState::Completed: return "completed";
    case Domain::ManagedRunState::Failed: return "failed";
    case Domain::ManagedRunState::Cancelled: return "stopped";
    case Domain::ManagedRunState::Paused: return "paused";
    }
    return "unknown";
}

[[nodiscard]] ManagedRunView managedView(
    Domain::Result<Domain::ManagedRunSnapshot> result)
{
    if (!result) {
        return {false, result.error().message, std::nullopt};
    }
    auto snapshot = std::move(result).value();
    const auto& record = snapshot.record;
    std::string message = "Run " + record.runId.value() +
        "\nState: " + std::string{managedStateName(record.state)} +
        (snapshot.pauseRequested ? " (pause requested)" : "") +
        "\nProject: " + record.projectId.value() +
        "\nAuthority generation: " +
            std::to_string(record.authorityGeneration) +
        "\nLifetime tokens: " + std::to_string(record.inputTokens) +
            " input / " + std::to_string(record.outputTokens) + " output";
    if (record.retainedContextTokens) {
        message += "\nRetained context: " +
            std::to_string(*record.retainedContextTokens) + " tokens";
    }
    if (!record.pendingFunctionCalls.empty()) {
        message += "\nPending provider calls: " +
            std::to_string(record.pendingFunctionCalls.size());
    }
    if (record.lastError) {
        message += "\nError: " + record.lastError->message;
    }
    if (record.outputText && !record.outputText->empty()) {
        message += "\n\nOutput\n" + *record.outputText;
    }
    return {true, std::move(message), std::move(snapshot)};
}

[[nodiscard]] std::string percentText(
    const Domain::TelemetryMetric<double>& metric)
{
    if (metric.value) {
        std::ostringstream text;
        text << std::fixed << std::setprecision(1) << *metric.value << '%';
        if (metric.stale) text << " (stale)";
        return text.str();
    }
    std::string text{Domain::telemetryMetricAvailabilityName(metric.availability)};
    if (metric.unavailableReason) text += ": " + *metric.unavailableReason;
    return text;
}

[[nodiscard]] std::string telemetryMessage(
    const Domain::ManagerTelemetrySnapshot& snapshot)
{
    std::string message =
        "Connected to Manager PID " + std::to_string(snapshot.manager.processId) +
        "\nCPU: " + percentText(snapshot.resources.cpuPercent) +
        "\nRAM: " + percentText(snapshot.resources.ramPercent) +
        "\nRelevant processes: " +
            std::to_string(snapshot.resources.processes.size()) +
        "\nProvider: " + (snapshot.provider.secure ? "https://" : "http://") +
            snapshot.provider.host + ':' + std::to_string(snapshot.provider.port) +
        "\nProjects: " + std::to_string(snapshot.projects.size()) +
        " · Tools: " + std::to_string(snapshot.tools.size()) +
        " · Recent events: " + std::to_string(snapshot.recentEvents.size());
    if (snapshot.provider.model) {
        message += "\nModel: " + *snapshot.provider.model;
    }
    if (snapshot.context.authoritative && snapshot.context.retainedTokens) {
        message += "\nRetained context: " +
            std::to_string(*snapshot.context.retainedTokens) + " / " +
            std::to_string(snapshot.context.capacityTokens) + " tokens";
        if (snapshot.context.headroomTokens) {
            message += " · Headroom: " +
                std::to_string(*snapshot.context.headroomTokens);
        }
    } else {
        message += "\nRetained context: no authoritative run sample selected";
    }
    if (snapshot.storeHealthy.value) {
        message += *snapshot.storeHealthy.value
            ? "\nStore: healthy"
            : "\nStore: unhealthy";
    } else {
        message += "\nStore: " + std::string{
            Domain::telemetryMetricAvailabilityName(
                snapshot.storeHealthy.availability)};
        if (snapshot.storeHealthy.unavailableReason) {
            message += " · " + *snapshot.storeHealthy.unavailableReason;
        }
    }
    return message;
}
}
ManagerConnection::ManagerConnection(
    std::optional<std::wstring> alphaRoot) noexcept
{
    if (!alphaRoot) return;
    auto created = W::WindowsAlphaManagerProfile::create(*alphaRoot);
    if (!created) {
        profileError_ = "Invalid isolated data root: " +
            created.error().message;
        return;
    }
    alphaProfile_.emplace(std::move(created).value());
    auto persistentRoot = W::WindowsAlphaManagerProfile::persistentDataRoot();
    persistentProfile_ = persistentRoot &&
        ::CompareStringOrdinal(alphaProfile_->nativeDataRoot().data(),
            static_cast<int>(alphaProfile_->nativeDataRoot().size()),
            persistentRoot.value().data(),
            static_cast<int>(persistentRoot.value().size()), TRUE) == CSTR_EQUAL;
}

std::string ManagerConnection::profileSummary() const
{
    if (!alphaProfile_) {
        return "Production\nData: %LOCALAPPDATA%\\Forge Conductor";
    }
    return std::string{persistentProfile_
            ? "Production\nData: "
            : "Isolated profile\nData: "} +
        alphaProfile_->dataRoot().value();
}

std::optional<std::string> ManagerConnection::viewStateScope() const noexcept
{
    return alphaProfile_
        ? std::optional<std::string>{alphaProfile_->dataRoot().value()}
        : std::nullopt;
}

std::string ManagerConnection::refresh(std::stop_token cancellation) noexcept {
    try {
        if (!profileError_.empty()) return profileError_;
        const std::string profile = profileSummary();
        auto clock = std::make_shared<W::SystemClock>();
        auto context = operationContext(clock, cancellation);
        auto created = connectManager(alphaProfile_, context, clock);
        if (!created) {
            return "Profile: " + profile +
                "\nDisconnected: " + created.error().message;
        }
        auto client = std::move(created).value();
        const auto result = client->status(context);
        client->shutdown();
        if (!result) return "Disconnected: " + result.error().message;
        const auto& status = result.value();
        return "Connected to manager PID " + std::to_string(status.processId) +
            "\nProfile: " + profile +
            "\nVersion: " + status.version +
            "\nService: " + (status.serviceActive ? "active" : "inactive") +
            "\nDashboard: " + (status.httpListening ? "listening" : "not listening") +
            (status.lastError ? "\nLast error: " + *status.lastError : "");
    } catch (const std::exception& error) { return error.what(); }
      catch (...) { return "Could not read manager status."; }
}

TelemetryView ManagerConnection::telemetry(
    std::string selectedRunId,
    const std::stop_token cancellation) noexcept
{
    try {
        if (!profileError_.empty()) {
            return {false, profileError_, std::nullopt};
        }
        std::optional<Domain::SessionId> runId;
        if (!selectedRunId.empty()) {
            auto parsed = Domain::SessionId::parse(selectedRunId);
            if (!parsed) return {false, parsed.error().message, std::nullopt};
            runId = std::move(parsed).value();
        }
        auto clock = std::make_shared<W::SystemClock>();
        auto context = operationContext(clock, cancellation);
        auto created = connectManager(alphaProfile_, context, clock);
        if (!created) {
            return {false, "Disconnected: " + created.error().message, std::nullopt};
        }
        auto client = std::move(created).value();
        auto result = client->telemetry(runId, context);
        client->shutdown();
        if (!result) {
            return {false, "Telemetry unavailable: " + result.error().message,
                std::nullopt};
        }
        auto snapshot = std::move(result).value();
        auto message = telemetryMessage(snapshot);
        return {true, std::move(message), std::move(snapshot)};
    } catch (const std::exception& error) {
        return {false, error.what(), std::nullopt};
    } catch (...) {
        return {false, "Could not read Manager telemetry.", std::nullopt};
    }
}

ProjectsView ManagerConnection::projects(
    const std::stop_token cancellation) noexcept
{
    try {
        if (!profileError_.empty()) return {false, profileError_, std::nullopt};
        auto clock = std::make_shared<W::SystemClock>();
        auto context = operationContext(clock, cancellation);
        auto created = connectManager(alphaProfile_, context, clock);
        if (!created) return {false, created.error().message, std::nullopt};
        auto client = std::move(created).value();
        auto result = client->projects(1'024U, context);
        client->shutdown();
        if (!result) return {false, result.error().message, std::nullopt};
        auto snapshot = std::move(result).value();
        auto message = "Loaded " + std::to_string(snapshot.projects.size()) +
            " registered project" +
            (snapshot.projects.size() == 1U ? "." : "s.");
        return {true, std::move(message), std::move(snapshot)};
    } catch (const std::exception& error) {
        return {false, error.what(), std::nullopt};
    } catch (...) {
        return {false, "Could not load registered projects.", std::nullopt};
    }
}

ProjectWorkspaceView ManagerConnection::initializeProject(
    std::string projectPath,
    std::string displayName,
    const std::stop_token cancellation) noexcept
{
    try {
        if (!profileError_.empty()) return {false, profileError_, std::nullopt};
        auto path = Domain::PathText::create(projectPath);
        if (!path) return {false, path.error().message, std::nullopt};
        auto clock = std::make_shared<W::SystemClock>();
        auto context = operationContext(
            clock, cancellation, std::chrono::seconds{15});
        auto created = connectManager(alphaProfile_, context, clock);
        if (!created) return {false, created.error().message, std::nullopt};
        auto client = std::move(created).value();
        auto result = client->initializeProject(
            Manager::ManagerProjectInitializeRequest{
                std::move(path).value(),
                displayName.empty()
                    ? std::nullopt
                    : std::optional<std::string>{std::move(displayName)},
                std::nullopt},
            context);
        client->shutdown();
        if (!result) return {false, result.error().message, std::nullopt};
        auto snapshot = std::move(result).value();
        const auto message = "Registered " + snapshot.project.displayName +
            " with exact project ID " + snapshot.project.id.value() + ".";
        return {true, message, std::move(snapshot)};
    } catch (const std::exception& error) {
        return {false, error.what(), std::nullopt};
    } catch (...) {
        return {false, "Could not register the project.", std::nullopt};
    }
}

ProjectWorkspaceView ManagerConnection::projectMemory(
    std::string projectId,
    std::string query,
    const std::stop_token cancellation) noexcept
{
    try {
        if (!profileError_.empty()) return {false, profileError_, std::nullopt};
        auto parsed = Domain::ProjectId::parse(projectId);
        if (!parsed) return {false, parsed.error().message, std::nullopt};
        auto clock = std::make_shared<W::SystemClock>();
        auto context = operationContext(clock, cancellation);
        auto created = connectManager(alphaProfile_, context, clock);
        if (!created) return {false, created.error().message, std::nullopt};
        auto client = std::move(created).value();
        const auto request = Manager::ManagerProjectMemoryRequest{
            parsed.value(), query, 20U};
        auto result = client->projectMemory(request, context);
        for (std::size_t retry{}; retry < 4U && !result &&
             !cancellation.stop_requested() &&
             result.error().code == Domain::ErrorCodes::DatabaseBusy &&
             result.error().message ==
                 "The selected project repository is already opening.";
             ++retry) {
            std::this_thread::sleep_for(std::chrono::milliseconds{80});
            result = client->projectMemory(request, context);
        }
        client->shutdown();
        if (!result) return {false, result.error().message, std::nullopt};
        auto snapshot = std::move(result).value();
        const auto message = "Loaded " + std::to_string(snapshot.records.size()) +
            " memory record" + (snapshot.records.size() == 1U ? "." : "s.");
        return {true, message, std::move(snapshot)};
    } catch (const std::exception& error) {
        return {false, error.what(), std::nullopt};
    } catch (...) {
        return {false, "Could not read project memory.", std::nullopt};
    }
}

ProjectWorkspaceView ManagerConnection::rememberProjectMemory(
    std::string projectId,
    std::string title,
    std::string summary,
    std::string body,
    std::vector<std::string> tags,
    const std::stop_token cancellation) noexcept
{
    try {
        if (!profileError_.empty()) return {false, profileError_, std::nullopt};
        auto parsed = Domain::ProjectId::parse(projectId);
        if (!parsed) return {false, parsed.error().message, std::nullopt};
        auto clock = std::make_shared<W::SystemClock>();
        auto context = operationContext(
            clock, cancellation, std::chrono::seconds{15});
        auto created = connectManager(alphaProfile_, context, clock);
        if (!created) return {false, created.error().message, std::nullopt};
        auto client = std::move(created).value();
        auto result = client->rememberProjectMemory(
            Manager::ManagerProjectRememberRequest{
                std::move(parsed).value(),
                std::move(title),
                std::move(summary),
                body.empty()
                    ? std::nullopt
                    : std::optional<std::string>{std::move(body)},
                std::move(tags)},
            context);
        client->shutdown();
        if (!result) return {false, result.error().message, std::nullopt};
        auto snapshot = std::move(result).value();
        const auto message = snapshot.writtenRecordId
            ? "Memory saved as " + snapshot.writtenRecordId->value() + "."
            : "Memory saved.";
        return {true, message, std::move(snapshot)};
    } catch (const std::exception& error) {
        return {false, error.what(), std::nullopt};
    } catch (...) {
        return {false, "Could not save project memory.", std::nullopt};
    }
}

InstructionPackageView ManagerConnection::instructionPackage(
    std::string projectId,
    std::string packagePath,
    const bool activate,
    std::string expectedRevision,
    const std::stop_token cancellation) noexcept
{
    try {
        if (!profileError_.empty()) return {false, profileError_, std::nullopt};
        auto project = Domain::ProjectId::parse(projectId);
        if (!project) return {false, project.error().message, std::nullopt};
        auto path = Domain::PathText::create(packagePath);
        if (!path) return {false, path.error().message, std::nullopt};
        std::optional<Domain::Sha256Digest> expected;
        if (!expectedRevision.empty()) {
            auto parsed = Domain::Sha256Digest::parse(expectedRevision);
            if (!parsed) return {false, parsed.error().message, std::nullopt};
            expected = std::move(parsed).value();
        }
        auto clock = std::make_shared<W::SystemClock>();
        auto context = operationContext(
            clock, cancellation, std::chrono::seconds{60});
        auto created = connectManager(alphaProfile_, context, clock);
        if (!created) return {false, created.error().message, std::nullopt};
        auto client = std::move(created).value();
        auto result = client->instructionPackage(
            Manager::ManagerInstructionPackageRequest{
                std::move(project).value(), std::move(path).value(), activate,
                std::move(expected)},
            context);
        client->shutdown();
        if (!result) return {false, result.error().message, std::nullopt};
        auto snapshot = std::move(result).value();
        const auto message = activate
            ? "Activated instruction revision " +
                snapshot.revision.value().substr(0U, 16U) + " for " +
                std::to_string(snapshot.fileCount) + " files."
            : "Validated " + std::to_string(snapshot.fileCount) +
                " instruction files as revision " +
                snapshot.revision.value().substr(0U, 16U) + ".";
        return {true, message, std::move(snapshot)};
    } catch (const std::exception& error) {
        return {false, error.what(), std::nullopt};
    } catch (...) {
        return {false, "Could not process the instruction package.", std::nullopt};
    }
}

LmStudioView ManagerConnection::lmStudio(
    const LmStudioAction action,
    const std::stop_token cancellation) noexcept
{
    try {
        if (!profileError_.empty()) return {false, profileError_, std::nullopt};
        auto clock = std::make_shared<W::SystemClock>();
        auto context = operationContext(
            clock, cancellation, action == LmStudioAction::Repair
                ? std::chrono::seconds{120} : std::chrono::seconds{30});
        auto created = connectManager(alphaProfile_, context, clock);
        if (!created) return {false, created.error().message, std::nullopt};
        auto client = std::move(created).value();
        Domain::Result<Manager::ManagerLmStudioSnapshot> result =
            action == LmStudioAction::Repair
                ? client->repairLmStudio(context)
                : action == LmStudioAction::Activate
                    ? client->activateLmStudio(context)
                    : client->lmStudioStatus(context);
        client->shutdown();
        if (!result) return {false, result.error().message, std::nullopt};
        auto snapshot = std::move(result).value();
        return {true, snapshot.actionDetail, std::move(snapshot)};
    } catch (const std::exception& error) {
        return {false, error.what(), std::nullopt};
    } catch (...) {
        return {false, "The LM Studio workflow failed safely.", std::nullopt};
    }
}

ToolsView ManagerConnection::tools(
    const std::stop_token cancellation) noexcept
{
    try {
        if (!profileError_.empty()) return {false, profileError_, std::nullopt};
        auto clock = std::make_shared<W::SystemClock>();
        auto context = operationContext(clock, cancellation);
        auto created = connectManager(alphaProfile_, context, clock);
        if (!created) return {false, created.error().message, std::nullopt};
        auto client = std::move(created).value();
        auto result = client->tools(context);
        client->shutdown();
        if (!result) return {false, result.error().message, std::nullopt};
        auto snapshot = std::move(result).value();
        auto message = "Loaded " + std::to_string(snapshot.tools.size()) +
            " Manager-owned tools. Shell preference is " +
            (snapshot.shellEnabled ? "enabled." : "disabled.");
        return {true, std::move(message), std::move(snapshot)};
    } catch (const std::exception& error) {
        return {false, error.what(), std::nullopt};
    } catch (...) {
        return {false, "The native tool catalog could not be loaded.", std::nullopt};
    }
}

ToolOutcomeView ManagerConnection::invokeTool(
    std::string projectId,
    std::string toolName,
    std::string canonicalArguments,
    const std::stop_token cancellation) noexcept
{
    try {
        if (!profileError_.empty()) return {false, profileError_, std::nullopt};
        auto parsed = Domain::ProjectId::parse(projectId);
        if (!parsed) return {false, parsed.error().message, std::nullopt};
        if (toolName.empty() || canonicalArguments.empty()) {
            return {false, "Select a tool and provide its JSON arguments.", std::nullopt};
        }
        auto clock = std::make_shared<W::SystemClock>();
        auto context = operationContext(
            clock, cancellation, std::chrono::seconds{30});
        auto created = connectManager(alphaProfile_, context, clock);
        if (!created) return {false, created.error().message, std::nullopt};
        auto client = std::move(created).value();
        auto result = client->invokeTool(
            Manager::ManagerToolInvokeRequest{
                std::move(parsed).value(),
                std::move(toolName),
                std::move(canonicalArguments)},
            context);
        client->shutdown();
        if (!result) return {false, result.error().message, std::nullopt};
        auto snapshot = std::move(result).value();
        std::string message = snapshot.ok ? "Tool completed." : "Tool reported failure.";
        if (snapshot.error) message += " " + snapshot.error->message;
        return {snapshot.ok, std::move(message), std::move(snapshot)};
    } catch (const std::exception& error) {
        return {false, error.what(), std::nullopt};
    } catch (...) {
        return {false, "The native tool invocation failed safely.", std::nullopt};
    }
}

OperationalView ManagerConnection::operational(
    const Manager::ManagerOperationalArea area,
    const Manager::ManagerOperationalAction action,
    std::string sessionId,
    std::string summary,
    std::optional<std::string> projectId,
    const std::stop_token cancellation) noexcept
{
    try {
        if (!profileError_.empty()) return {false, profileError_, std::nullopt};
        std::optional<Domain::SessionId> parsed;
        if (!sessionId.empty()) {
            auto value = Domain::SessionId::parse(sessionId);
            if (!value) return {false, value.error().message, std::nullopt};
            parsed = std::move(value).value();
        }
        std::optional<Domain::ProjectId> parsedProject;
        if (projectId) {
            auto value = Domain::ProjectId::parse(*projectId);
            if (!value) return {false, value.error().message, std::nullopt};
            parsedProject = std::move(value).value();
        }
        auto clock = std::make_shared<W::SystemClock>();
        auto context = operationContext(clock, cancellation, std::chrono::seconds{15});
        auto created = connectManager(alphaProfile_, context, clock);
        if (!created) return {false, created.error().message, std::nullopt};
        auto client = std::move(created).value();
        auto result = client->operational(
            Manager::ManagerOperationalRequest{
                area, action, std::move(parsed), std::move(summary),
                std::move(parsedProject)}, context);
        client->shutdown();
        if (!result) return {false, result.error().message, std::nullopt};
        auto snapshot = std::move(result).value();
        return {true, snapshot.title, std::move(snapshot)};
    } catch (const std::exception& error) {
        return {false, error.what(), std::nullopt};
    } catch (...) {
        return {false, "The operational page could not be loaded.", std::nullopt};
    }
}

MaintenanceView ManagerConnection::resetData(
    const Manager::ManagerMaintenanceScope scope,
    std::optional<std::string> projectId,
    std::string confirmationToken,
    const std::stop_token cancellation) noexcept
{
    try {
        if (!profileError_.empty()) return {false, profileError_, std::nullopt};
        std::optional<Domain::ProjectId> parsed;
        if (projectId) {
            auto value = Domain::ProjectId::parse(*projectId);
            if (!value) return {false, value.error().message, std::nullopt};
            parsed = std::move(value).value();
        }
        auto clock = std::make_shared<W::SystemClock>();
        auto context = operationContext(clock, cancellation, std::chrono::seconds{60});
        auto created = connectManager(alphaProfile_, context, clock);
        if (!created) return {false, created.error().message, std::nullopt};
        auto client = std::move(created).value();
        auto result = client->maintenance(
            Manager::ManagerMaintenanceRequest{
                scope, std::move(parsed), std::move(confirmationToken)}, context);
        client->shutdown();
        if (!result) return {false, result.error().message, std::nullopt};
        auto snapshot = std::move(result).value();
        auto message = snapshot.detail + " Projects affected: " +
            std::to_string(snapshot.projectsAffected) +
            "; records removed: " + std::to_string(snapshot.recordsRemoved) +
            "; links removed: " + std::to_string(snapshot.linksRemoved) +
            "; events removed: " + std::to_string(snapshot.eventsRemoved) + ".";
        return {true, std::move(message), std::move(snapshot)};
    } catch (const std::exception& error) {
        return {false, error.what(), std::nullopt};
    } catch (...) {
        return {false, "The maintenance reset failed safely.", std::nullopt};
    }
}

ProviderSettingsView ManagerConnection::providerSettings(
    const std::stop_token cancellation) noexcept
{
    try {
        if (!profileError_.empty()) return {false, profileError_, {}};
        auto clock = std::make_shared<W::SystemClock>();
        auto context = operationContext(clock, cancellation);
        auto created = connectManager(alphaProfile_, context, clock);
        if (!created) return {false, created.error().message, {}};
        auto client = std::move(created).value();
        auto result = client->settings(context);
        client->shutdown();
        if (!result) return {false, result.error().message, {}};
        return {true, "Provider settings loaded from the manager.",
            std::move(result).value()};
    } catch (const std::exception& error) {
        return {false, error.what(), {}};
    } catch (...) {
        return {false, "Could not load provider settings.", {}};
    }
}

std::string ManagerConnection::control(
    const Domain::ManagerControlAction action,
    const std::stop_token cancellation) noexcept
{
    try {
        if (!profileError_.empty()) return profileError_;
        auto clock = std::make_shared<W::SystemClock>();
        auto context = operationContext(clock, cancellation);
        auto created = connectManager(alphaProfile_, context, clock);
        if (!created) return created.error().message;
        auto client = std::move(created).value();
        auto result = client->control({action}, context);
        client->shutdown();
        if (!result) return "Manager control failed: " + result.error().message;
        const auto& status = result.value();
        const char* state = status.serviceActive ? "active" : "inactive";
        return "Manager service is " + std::string{state} +
            ". Process PID " + std::to_string(status.processId) + ".";
    } catch (const std::exception& error) {
        return error.what();
    } catch (...) {
        return "Could not control the Manager service.";
    }
}

std::string ManagerConnection::saveProviderSettings(
    const Domain::ManagerSettingsPatch& patch,
    const std::stop_token cancellation) noexcept
{
    try {
        if (!profileError_.empty()) return profileError_;
        auto clock = std::make_shared<W::SystemClock>();
        auto context = operationContext(clock, cancellation);
        auto created = connectManager(alphaProfile_, context, clock);
        if (!created) return created.error().message;
        auto client = std::move(created).value();
        auto result = client->updateSettings(patch, true, context);
        client->shutdown();
        if (!result) return "Provider settings were not saved: " + result.error().message;
        return "Provider settings saved for " +
            result.value().settings.localModelHost + ":" +
            std::to_string(result.value().settings.localModelPort) +
            ". New runs use the saved model connection. Restart the Manager after changing context or shell settings.";
    } catch (const std::exception& error) {
        return error.what();
    } catch (...) {
        return "Could not save provider settings.";
    }
}

std::string ManagerConnection::testProvider(
    const Domain::ManagerSettings& settings,
    const std::stop_token cancellation) noexcept
{
    try {
        auto valid = Domain::validateManagerSettings(settings);
        if (!valid) return "Connection test blocked: " + valid.error().message;
        W::LMStudioResponsesTransportConfiguration configuration;
        configuration.loopbackHost = settings.localModelHost;
        configuration.port = settings.localModelPort;
        configuration.secure = settings.localModelSecure;
        if (!settings.localModelName.empty()) {
            configuration.model = settings.localModelName;
        }
        auto transport = std::make_unique<W::LMStudioResponsesTransport>(
            std::move(configuration));
        auto clock = std::make_shared<W::SystemClock>();
        auto context = operationContext(clock, cancellation);
        W::WindowsUuidGenerator ids;
        const auto next = [&ids]() {
            auto value = ids.next();
            if (!value) throw std::runtime_error{value.error().message};
            return value.value().value();
        };
        Domain::SessionCreationRequest request{
            Domain::ContinuityOperationId::parse(next()).value(),
            Domain::ProjectId::parse(next()).value(),
            Domain::SessionId::parse(next()).value(),
            Domain::IdempotencyKey::create(next()).value()};
        auto probed = transport->createSession(request, context);
        transport->shutdown();
        if (!probed) return "LM Studio connection failed: " + probed.error().message;
        return "LM Studio model discovery succeeded. Loaded model: " +
            probed.value().model.value_or("unknown") + ".";
    } catch (const std::exception& error) {
        return "LM Studio connection failed: " + std::string{error.what()};
    } catch (...) {
        return "LM Studio connection failed safely.";
    }
}

ProjectPolicyView ManagerConnection::projectPolicy(const Contracts::ProjectPolicyRequest& request,
    std::stop_token cancellation) noexcept
{
    try {
        if (!profileError_.empty()) return {false, profileError_, {}};
        auto clock = std::make_shared<W::SystemClock>();
        auto context = operationContext(clock, cancellation, std::chrono::minutes{5});
        auto created = connectManager(alphaProfile_, context, clock);
        if (!created) return {false, created.error().message, {}};
        auto client = std::move(created).value();
        auto result = client->projectPolicy(request, context);
        client->shutdown();
        if (!result) return {false, result.error().message, {}};
        return {true, "Policy operation completed.", result.value().canonicalJson};
    } catch (const std::exception& error) { return {false, error.what(), {}}; }
    catch (...) { return {false, "Policy operation failed safely.", {}}; }
}

std::string ManagerConnection::probeProviderContract(
    const Domain::ManagerSettings& settings,
    const std::stop_token cancellation) noexcept
{
    try {
        auto valid = Domain::validateManagerSettings(settings);
        if (!valid) return "Responses probe blocked: " + valid.error().message;
        W::LMStudioResponsesTransportConfiguration configuration;
        configuration.loopbackHost = settings.localModelHost;
        configuration.port = settings.localModelPort;
        configuration.secure = settings.localModelSecure;
        if (!settings.localModelName.empty()) configuration.model = settings.localModelName;
        W::LMStudioResponsesTransport transport{std::move(configuration)};
        auto clock = std::make_shared<W::SystemClock>();
        auto context = operationContext(clock, cancellation, std::chrono::seconds{90});
        W::WindowsUuidGenerator ids;
        const auto nextId = [&ids]() {
            auto next = ids.next();
            if (!next) throw std::runtime_error{next.error().message};
            return next.value().value();
        };
        Domain::ManagedProviderTurnRequest request{
            Domain::ProjectId::parse(nextId()).value(),
            Domain::SessionId::parse(nextId()).value(),
            1U,
            "For a disposable Responses contract check, reply with exactly OK. Do not call tools.",
            std::nullopt,
            {},
            {}};
        auto response = transport.complete(request, context);
        transport.shutdown();
        if (!response) return "Responses contract failed: " + response.error().message;
        if (!response.value().functionCalls.empty()) {
            return "Responses protocol returned a response ID, but the probe unexpectedly requested a function call. No function was executed.";
        }
        return "Responses contract passed: fresh response " +
            response.value().responseId.value() + " · " +
            std::to_string(response.value().inputTokens) + " input / " +
            std::to_string(response.value().outputTokens) +
            " output tokens. This disposable API response is not a desktop chat or a managed project run.";
    } catch (const std::exception& error) {
        return "Responses contract failed: " + std::string{error.what()};
    } catch (...) {
        return "Responses contract failed safely.";
    }
}

ManagedRunView ManagerConnection::startManagedRun(
    std::string projectId,
    std::string clientId,
    const std::uint64_t authorityGeneration,
    std::string task,
    const bool allowTools,
    const std::stop_token cancellation) noexcept
{
    try {
        if (!profileError_.empty()) return {false, profileError_, std::nullopt};
        auto project = Domain::ProjectId::parse(projectId);
        auto clientIdValue = Domain::ClientId::parse(clientId);
        if (!project) return {false, project.error().message, std::nullopt};
        if (!clientIdValue) return {false, clientIdValue.error().message, std::nullopt};
        if (task.empty()) {
            return {false, "A mission is required.", std::nullopt};
        }
        auto clock = std::make_shared<W::SystemClock>();
        auto context = operationContext(clock, cancellation, std::chrono::seconds{15});
        W::WindowsUuidGenerator ids;
        auto generated = ids.next();
        if (!generated) return {false, generated.error().message, std::nullopt};
        auto runId = Domain::SessionId::parse(generated.value().value());
        if (!runId) return {false, runId.error().message, std::nullopt};
        auto created = connectManager(alphaProfile_, context, clock);
        if (!created) return {false, created.error().message, std::nullopt};
        auto manager = std::move(created).value();
        auto result = manager->startManagedRun(
            Domain::ManagedRunStartRequest{
                std::move(runId).value(),
                std::move(project).value(),
                std::move(clientIdValue).value(),
                context.operationId,
                context.correlationId,
                authorityGeneration,
                std::move(task),
                allowTools},
            context);
        manager->shutdown();
        return managedView(std::move(result));
    } catch (const std::exception& error) {
        return {false, error.what(), std::nullopt};
    } catch (...) {
        return {false, "The managed run could not be started.", std::nullopt};
    }
}

ManagedRunView ManagerConnection::controlManagedRun(
    std::string runId,
    const ManagedRunAction action,
    const std::stop_token cancellation) noexcept
{
    try {
        if (!profileError_.empty()) return {false, profileError_, std::nullopt};
        auto parsedRunId = Domain::SessionId::parse(runId);
        if (!parsedRunId) {
            return {false, parsedRunId.error().message, std::nullopt};
        }
        auto clock = std::make_shared<W::SystemClock>();
        auto context = operationContext(clock, cancellation, std::chrono::seconds{15});
        auto created = connectManager(alphaProfile_, context, clock);
        if (!created) return {false, created.error().message, std::nullopt};
        auto manager = std::move(created).value();
        Domain::Result<Domain::ManagedRunSnapshot> result =
            Domain::Result<Domain::ManagedRunSnapshot>::failure(
                Domain::makeError(Domain::ErrorCodes::InvalidRequest,
                    "The managed run action is invalid."));
        switch (action) {
        case ManagedRunAction::Status:
            result = manager->managedRunStatus(parsedRunId.value(), context);
            break;
        case ManagedRunAction::Pause:
            result = manager->pauseManagedRun(parsedRunId.value(), context);
            break;
        case ManagedRunAction::Resume:
            result = manager->resumeManagedRun(parsedRunId.value(), context);
            break;
        case ManagedRunAction::Cancel:
            result = manager->cancelManagedRun(parsedRunId.value(), context);
            break;
        }
        manager->shutdown();
        return managedView(std::move(result));
    } catch (const std::exception& error) {
        return {false, error.what(), std::nullopt};
    } catch (...) {
        return {false, "The managed run action failed safely.", std::nullopt};
    }
}
std::string ManagerConnection::start(std::stop_token cancellation) noexcept {
    try {
        if (!profileError_.empty()) return profileError_;
        if (cancellation.stop_requested()) return "Cancelled.";
        auto clock = std::make_shared<W::SystemClock>();
        std::string lastConnectionError;
        const auto readyManager = [&]() -> std::optional<std::string> {
            try {
                auto context = operationContext(
                    clock, cancellation, std::chrono::milliseconds{750});
                auto created = connectManager(alphaProfile_, context, clock);
                if (!created) {
                    lastConnectionError = created.error().message;
                    return std::nullopt;
                }
                auto client = std::move(created).value();
                auto status = client->status(context);
                client->shutdown();
                if (!status) {
                    lastConnectionError = status.error().message;
                    return std::nullopt;
                }
                const auto& value = status.value();
                return "Manager ready and authenticated. PID " +
                    std::to_string(value.processId) + ", version " +
                    value.version + ", service " +
                    (value.serviceActive ? "active." : "inactive.");
            } catch (const std::exception& error) {
                lastConnectionError = error.what();
                return std::nullopt;
            }
        };
        if (auto existing = readyManager()) {
            return "Attached to the existing " + *existing;
        }
        std::array<wchar_t, 32768> path{};
        const auto length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
        if (!length || length >= path.size()) return "Cannot resolve the installed application directory.";
        const auto executable = std::filesystem::path{path.data()}.parent_path() / L"ForgeConductor.Manager.exe";
        std::error_code executableError;
        if (!std::filesystem::is_regular_file(executable, executableError)) {
            return "Manager could not start because the verified installed sibling is missing: " +
                executable.string();
        }
        std::wstring arguments = L"\"" + executable.wstring() + L"\"";
        if (alphaProfile_) {
            arguments.append(L" --alpha-root \"");
            arguments.append(alphaProfile_->nativeDataRoot());
            arguments.push_back(L'\"');
        }
        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        std::filesystem::path startupLog;
        NativeHandle startupInput;
        NativeHandle startupOutput;
        try {
            startupLog = std::filesystem::temp_directory_path() /
                (L"ForgeConductor.Manager.startup." +
                    std::to_wstring(GetCurrentProcessId()) + L".log");
            SECURITY_ATTRIBUTES attributes{};
            attributes.nLength = sizeof(attributes);
            attributes.bInheritHandle = TRUE;
            startupOutput.value = CreateFileW(
                startupLog.c_str(), GENERIC_WRITE,
                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                &attributes, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            startupInput.value = CreateFileW(
                L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                &attributes, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (startupOutput.valid() && startupInput.valid()) {
                startup.dwFlags = STARTF_USESTDHANDLES;
                startup.hStdInput = startupInput.value;
                startup.hStdOutput = startupOutput.value;
                startup.hStdError = startupOutput.value;
            }
        } catch (...) {
            startupLog.clear();
        }
        const auto discardStartupLog = [&]() noexcept {
            startupInput.close();
            startupOutput.close();
            std::error_code ignored;
            std::filesystem::remove(startupLog, ignored);
        };
        ProcessHandles process;
        const auto inheritHandles =
            (startup.dwFlags & STARTF_USESTDHANDLES) != 0 ? TRUE : FALSE;
        if (!CreateProcessW(executable.c_str(), arguments.data(), nullptr, nullptr, inheritHandles,
                CREATE_NO_WINDOW, nullptr, executable.parent_path().c_str(), &startup, &process.value)) {
            const auto error = GetLastError();
            discardStartupLog();
            return "Manager could not start. Windows error " + std::to_string(error) +
                ". Verify the manager executable is installed beside the app.";
        }
        const auto readyDeadline = GetTickCount64() + 10'000ULL;
        std::optional<DWORD> observedExitCode;
        while (GetTickCount64() < readyDeadline) {
            if (cancellation.stop_requested()) {
                discardStartupLog();
                return "Manager startup was cancelled; the independently owned process was left unchanged.";
            }
            if (auto attached = readyManager()) {
                discardStartupLog();
                return "Started and attached to the " + *attached;
            }
            const auto processState = WaitForSingleObject(process.value.hProcess, 0U);
            if (processState == WAIT_OBJECT_0) {
                DWORD exitCode{};
                if (GetExitCodeProcess(process.value.hProcess, &exitCode)) {
                    observedExitCode = exitCode;
                }
                // A concurrent launch may have won the per-user lease. Give its
                // authenticated endpoint a brief opportunity to become ready.
                if (GetTickCount64() + 1'000ULL < readyDeadline) {
                    const auto concurrentDeadline = GetTickCount64() + 1'000ULL;
                    while (GetTickCount64() < concurrentDeadline) {
                        if (auto attached = readyManager()) {
                            discardStartupLog();
                            return "Attached to the concurrently established " + *attached;
                        }
                        std::this_thread::sleep_for(std::chrono::milliseconds{100});
                    }
                }
                break;
            }
            if (processState == WAIT_FAILED) {
                const auto error = GetLastError();
                discardStartupLog();
                return "Manager started, but Windows could not observe its startup state (error " +
                    std::to_string(error) + ").";
            }
            std::this_thread::sleep_for(std::chrono::milliseconds{100});
        }
        if (auto attached = readyManager()) {
            discardStartupLog();
            return "Started and attached to the " + *attached;
        }
        startupOutput.close();
        const auto detail = managerStartupDetail(startupLog);
        if (observedExitCode == static_cast<DWORD>(
                Manager::ManagerUnsupportedDataStoreExitCode)) {
            discardStartupLog();
            return "The default Forge Conductor data store is newer than this build "
                "supports and was left unchanged. Install a build that supports that "
                "store, or use an explicitly isolated profile for testing.";
        }
        if (observedExitCode) {
            const auto exitCode = *observedExitCode;
            discardStartupLog();
            return "Manager exited during automatic startup with code " +
                std::to_string(exitCode) +
                (detail.empty()
                    ? ". Open Diagnostics for recovery details."
                    : ". " + detail);
        }
        discardStartupLog();
        return "Manager did not complete its authenticated ready handshake within 10 seconds" +
            (lastConnectionError.empty()
                ? std::string{"."}
                : std::string{". Last connection error: "} + lastConnectionError);
    } catch (const std::exception& error) { return error.what(); }
      catch (...) { return "Could not start manager."; }
}

Application::ProjectSetupSnapshot ManagerConnection::prepareProject(
    std::string folder, std::stop_token cancellation,
    const Application::ProjectSetupCoordinator::Observer& observer)
{
    Application::ProjectSetupCoordinator coordinator{*this};
    return coordinator.prepare(std::move(folder), cancellation, observer);
}

Application::SetupOperationResult ManagerConnection::ensureManager(std::stop_token cancellation)
{
    const auto startup = start(cancellation);
    if (cancellation.stop_requested()) return {false, "Preparation cancelled."};
    auto clock = std::make_shared<W::SystemClock>();
    auto context = operationContext(clock, cancellation);
    auto created = connectManager(alphaProfile_, context, clock);
    if (!created) return {false, startup + " " + created.error().message};
    auto client = std::move(created).value();
    auto ready = client->control({Domain::ManagerControlAction::Start}, context);
    client->shutdown();
    if (!ready) return {false, ready.error().message};
    return {ready.value().serviceActive, ready.value().serviceActive
        ? "Manager started and authenticated."
        : "Manager is connected but its service is not active. Retry preparation."};
}

Application::SetupOperationResult ManagerConnection::ensureProject(
    Application::ProjectSetupSnapshot& setup, std::stop_token cancellation)
{
    const auto project = initializeProject(setup.folder, {}, cancellation);
    if (!project.loaded || !project.snapshot) return {false, project.message};
    if (!project.snapshot->integrityOk)
        return {false, "Project storage needs repair. Open Diagnostics before starting work."};
    setup.projectId = project.snapshot->project.id.value();
    setup.projectName = project.snapshot->project.displayName;
    return {true, "Project registered and storage verified: " + setup.projectName};
}

Application::SetupOperationResult ManagerConnection::ensureProvider(
    Application::ProjectSetupSnapshot& setup, std::stop_token cancellation)
{
    auto current = providerSettings(cancellation);
    if (!current.loaded) return {false, current.message};
    auto clock = std::make_shared<W::SystemClock>();
    auto context = operationContext(clock, cancellation, std::chrono::seconds{180});
    W::WindowsModelPreparation preparation;
    auto prepared = preparation.prepare(current.settings, context);
    if (!prepared) return {false, prepared.error().message};
    setup.model = prepared.value().identifier;
    Domain::ManagerSettingsPatch patch;
    patch.localModelName = setup.model;
    patch.effectiveContextCapacity = current.settings.effectiveContextCapacity;
    auto created = connectManager(alphaProfile_, context, clock);
    if (!created) return {false, created.error().message};
    auto client = std::move(created).value();
    auto saved = client->updateSettings(patch, true, context);
    client->shutdown();
    if (!saved) return {false, saved.error().message};
    if (saved.value().settings.localModelName != setup.model ||
        saved.value().settings.effectiveContextCapacity != current.settings.effectiveContextCapacity)
        return {false, "The Manager did not confirm the prepared model settings. Retry preparation."};
    return {true, std::string{prepared.value().modelLoaded ? "Loaded and verified " : "Verified loaded model "} + setup.model};
}

Application::SetupOperationResult ManagerConnection::verifyProvider(
    Application::ProjectSetupSnapshot& setup, std::stop_token cancellation)
{
    auto current = providerSettings(cancellation);
    if (!current.loaded) return {false, current.message};
    if (!current.settings.localModelName.empty() && current.settings.localModelName != setup.model)
        return {false, "Model settings changed during preparation. Retry with the current settings."};
    W::LMStudioResponsesTransportConfiguration configuration;
    configuration.loopbackHost = current.settings.localModelHost;
    configuration.port = current.settings.localModelPort;
    configuration.secure = current.settings.localModelSecure;
    configuration.model = setup.model;
    W::LMStudioResponsesTransport transport{std::move(configuration)};
    auto clock = std::make_shared<W::SystemClock>();
    auto context = operationContext(clock, cancellation, std::chrono::seconds{90});
    W::WindowsUuidGenerator ids;
    auto id = ids.next();
    if (!id) return {false, id.error().message};
    Domain::ManagedProviderTurnRequest request{
        Domain::ProjectId::parse(setup.projectId).value(),
        Domain::SessionId::parse(id.value().value()).value(), 1U,
        "Connection check: reply OK. Do not call tools.", std::nullopt, {}, {}};
    auto response = transport.complete(request, context);
    transport.shutdown();
    if (!response) return {false, "The model could not answer: " + response.error().message};
    if (!response.value().functionCalls.empty() || response.value().outputText.empty())
        return {false, "The model did not complete the connection check. Choose another model or retry."};
    return {true, "Model answered successfully. Your project is ready for a task."};
}

ProviderModelsView ManagerConnection::providerModels(
    const Domain::ManagerSettings& settings,
    const std::stop_token cancellation) noexcept
{
    try {
        const auto valid = Domain::validateManagerSettings(settings);
        if (!valid) return {false, valid.error().message, {}};
        if (cancellation.stop_requested()) return {false, "Model discovery cancelled.", {}};
        const std::wstring host{settings.localModelHost.begin(),
            settings.localModelHost.end()};
        InternetHandle session{::WinHttpOpen(L"Forge Conductor/1.1",
            WINHTTP_ACCESS_TYPE_NO_PROXY, WINHTTP_NO_PROXY_NAME,
            WINHTTP_NO_PROXY_BYPASS, 0)};
        if (!session.value) return {false, "Could not initialize local model discovery.", {}};
        if (!::WinHttpSetTimeouts(session.value, 2000, 2000, 2000, 2000))
            return {false, "Could not set a bounded model-discovery timeout.", {}};
        InternetHandle connection{::WinHttpConnect(session.value, host.c_str(),
            static_cast<INTERNET_PORT>(settings.localModelPort), 0)};
        if (!connection.value) return {false, "Could not connect to the configured loopback endpoint.", {}};
        InternetHandle request{::WinHttpOpenRequest(connection.value, L"GET",
            L"/v1/models", nullptr, WINHTTP_NO_REFERER,
            WINHTTP_DEFAULT_ACCEPT_TYPES,
            settings.localModelSecure ? WINHTTP_FLAG_SECURE : 0)};
        if (!request.value || !::WinHttpSendRequest(request.value,
                WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA,
                0, 0, 0) || !::WinHttpReceiveResponse(request.value, nullptr)) {
            return {false, "LM Studio /v1/models is not reachable at the configured endpoint.", {}};
        }
        DWORD status{}, statusBytes{sizeof(status)};
        if (!::WinHttpQueryHeaders(request.value,
                WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                WINHTTP_HEADER_NAME_BY_INDEX, &status, &statusBytes,
                WINHTTP_NO_HEADER_INDEX) || status != 200U) {
            return {false, "LM Studio model discovery returned HTTP " +
                std::to_string(status) + ".", {}};
        }
        std::string body;
        while (!cancellation.stop_requested()) {
            DWORD available{};
            if (!::WinHttpQueryDataAvailable(request.value, &available))
                return {false, "Could not read LM Studio model metadata.", {}};
            if (available == 0U) break;
            if (available > 65536U - body.size())
                return {false, "LM Studio model metadata exceeded the safe display limit.", {}};
            const auto begin = body.size();
            body.resize(begin + available);
            DWORD read{};
            if (!::WinHttpReadData(request.value, body.data() + begin,
                    available, &read)) {
                return {false, "Could not read LM Studio model metadata.", {}};
            }
            body.resize(begin + read);
            if (read == 0U) break;
        }
        if (cancellation.stop_requested()) return {false, "Model discovery cancelled.", {}};
        const auto document = nlohmann::json::parse(body);
        if (!document.is_object() || !document.contains("data") ||
            !document.at("data").is_array()) {
            return {false, "LM Studio returned no model collection.", {}};
        }
        std::vector<std::string> models;
        for (const auto& item : document.at("data")) {
            if (!item.is_object() || !item.contains("id") ||
                !item.at("id").is_string()) continue;
            auto id = item.at("id").get<std::string>();
            if (id.empty() || id.size() > 256U || id.find('\0') != std::string::npos ||
                !Domain::isValidUtf8(id)) continue;
            if (std::find(models.begin(), models.end(), id) == models.end())
                models.push_back(std::move(id));
            if (models.size() == 128U) break;
        }
        return {true, models.empty()
            ? "LM Studio has no valid loaded model for Responses."
            : std::to_string(models.size()) + " loaded model" +
                (models.size() == 1U ? "" : "s") + " discovered.",
            std::move(models)};
    } catch (const std::exception& error) {
        return {false, "LM Studio model discovery failed: " +
            std::string{error.what()}, {}};
    } catch (...) {
        return {false, "LM Studio model discovery failed safely.", {}};
    }
}
}
