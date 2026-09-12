#include "ManagerConnection.h"
#include "ForgeConductor/Infrastructure/Windows/DpapiSecureStorage.h"
#include "ForgeConductor/Infrastructure/Windows/SystemClock.h"
#include "ForgeConductor/Infrastructure/Windows/LMStudioResponsesTransport.h"
#include "ForgeConductor/Infrastructure/Windows/WindowsCurrentUserIdentity.h"
#include "ForgeConductor/Infrastructure/Windows/WindowsManagerAuthentication.h"
#include "ForgeConductor/Infrastructure/Windows/WindowsManagerInstanceLease.h"
#include "ForgeConductor/Infrastructure/Windows/WindowsManagerNamedPipeClient.h"
#include "ForgeConductor/Infrastructure/Windows/WindowsUuidGenerator.h"
#include "ForgeConductor/Manager/ManagerProcessExitCodes.h"
#include <windows.h>
#include <array>
#include <chrono>
#include <filesystem>
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

[[nodiscard]] Domain::OperationContext operationContext(
    const std::shared_ptr<W::SystemClock>& clock,
    const std::stop_token cancellation,
    const std::chrono::seconds timeout = std::chrono::seconds{5})
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
                "Manager is not initialized. Select Start manager."));
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
        "\nManager processes: " +
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
        profileError_ = "Invalid isolated Alpha root: " +
            created.error().message;
        return;
    }
    alphaProfile_.emplace(std::move(created).value());
}
std::string ManagerConnection::refresh(std::stop_token cancellation) noexcept {
    try {
        if (!profileError_.empty()) return profileError_;
        auto clock = std::make_shared<W::SystemClock>();
        auto context = operationContext(clock, cancellation);
        auto created = connectManager(alphaProfile_, context, clock);
        if (!created) return created.error().message;
        auto client = std::move(created).value();
        const auto result = client->status(context);
        client->shutdown();
        if (!result) return "Disconnected: " + result.error().message;
        const auto& status = result.value();
        const std::string profile = alphaProfile_
            ? "Isolated Alpha\nData: " + alphaProfile_->dataRoot().value()
            : "Production\nData: %LOCALAPPDATA%\\Forge Conductor";
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
        auto result = client->projectMemory(
            Manager::ManagerProjectMemoryRequest{
                std::move(parsed).value(), std::move(query), 20U},
            context);
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

LmStudioView ManagerConnection::lmStudio(
    const LmStudioAction action,
    const std::stop_token cancellation) noexcept
{
    try {
        if (!profileError_.empty()) return {false, profileError_, std::nullopt};
        auto clock = std::make_shared<W::SystemClock>();
        auto context = operationContext(
            clock, cancellation, std::chrono::seconds{30});
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
        auto clock = std::make_shared<W::SystemClock>();
        auto context = operationContext(clock, cancellation, std::chrono::seconds{15});
        auto created = connectManager(alphaProfile_, context, clock);
        if (!created) return {false, created.error().message, std::nullopt};
        auto client = std::move(created).value();
        auto result = client->operational(
            Manager::ManagerOperationalRequest{
                area, action, std::move(parsed), std::move(summary)}, context);
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
            ". Restart the Manager before starting new model sessions.";
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

ManagedRunView ManagerConnection::startManagedRun(
    std::string projectId,
    std::string clientId,
    const std::uint64_t authorityGeneration,
    std::string task,
    const std::stop_token cancellation) noexcept
{
    try {
        if (!profileError_.empty()) return {false, profileError_, std::nullopt};
        auto project = Domain::ProjectId::parse(projectId);
        auto clientIdValue = Domain::ClientId::parse(clientId);
        if (!project) return {false, project.error().message, std::nullopt};
        if (!clientIdValue) return {false, clientIdValue.error().message, std::nullopt};
        if (authorityGeneration == 0U || task.empty()) {
            return {false,
                "Project authority generation and task are required.",
                std::nullopt};
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
                std::move(task)},
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
        std::array<wchar_t, 32768> path{};
        const auto length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
        if (!length || length >= path.size()) return "Cannot resolve the installed application directory.";
        const auto executable = std::filesystem::path{path.data()}.parent_path() / L"ForgeConductor.Manager.exe";
        std::wstring arguments = L"\"" + executable.wstring() + L"\"";
        if (alphaProfile_) {
            arguments.append(L" --alpha-root \"");
            arguments.append(alphaProfile_->nativeDataRoot());
            arguments.push_back(L'\"');
        }
        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        ProcessHandles process;
        if (!CreateProcessW(executable.c_str(), arguments.data(), nullptr, nullptr, FALSE,
                CREATE_NO_WINDOW, nullptr, executable.parent_path().c_str(), &startup, &process.value)) {
            return "Manager could not start. Windows error " + std::to_string(GetLastError()) +
                ". Verify the manager executable is installed beside the app.";
        }
        const DWORD startupState = WaitForSingleObject(process.value.hProcess, 2'000U);
        if (startupState == WAIT_OBJECT_0) {
            DWORD exitCode{};
            if (!GetExitCodeProcess(process.value.hProcess, &exitCode)) {
                return "Manager exited during startup and Windows could not read its exit code.";
            }
            if (exitCode == static_cast<DWORD>(
                    Manager::ManagerUnsupportedDataStoreExitCode)) {
                return "The default Forge Conductor data store is newer than this build "
                    "supports and was left unchanged. Install a build that supports that "
                    "store, or launch ForgeConductorApp.exe with --alpha-root followed by "
                    "an absolute empty folder to use an explicitly isolated profile.";
            }
            return "Manager exited during startup with code " +
                std::to_string(exitCode) +
                ". Open Diagnostics for recovery details.";
        }
        if (startupState == WAIT_FAILED) {
            return "Manager started, but Windows could not observe its startup state (error " +
                std::to_string(GetLastError()) + "). Select Refresh to attach.";
        }
        // The manager is independently owned. Closing this connection never terminates it.
        return alphaProfile_
            ? "Isolated Alpha manager start requested for " +
                alphaProfile_->dataRoot().value() +
                ". Select Refresh to attach and read its status."
            : "Manager start requested. Select Refresh to attach and read its status.";
    } catch (const std::exception& error) { return error.what(); }
      catch (...) { return "Could not start manager."; }
}
}
