#include "ManagerCompositionRoot.h"

#include "ManagerControllerClient.h"
#include "ManagerMaintenanceWorker.h"
#include "ManagerProcessHost.h"
#include "ManagerProcessRestartSignal.h"
#include "ManagerProcessStopSignal.h"
#include "ManagerProcessStopWatcher.h"
#include "ManagerProcessWorkerGroup.h"
#include "ManagerTransitionWorker.h"
#include "WindowsManagerRuntime.h"

#include "ManagerDashboardOperationalDataSource.h"
#include "ManagerDoctorService.h"
#include "ManagerLmStudioAuthorityRouter.h"
#include "ManagerLmStudioReadScopeResolver.h"
#include "ManagerMaintenanceService.h"
#include "UnavailableLmStudioDeploymentService.h"
#include "UnavailableTelemetryService.h"

#include "ForgeConductor/Application/AgentCatalog.h"
#include "ForgeConductor/Application/AgentSessionService.h"
#include "ForgeConductor/Application/ContinuityCoordinator.h"
#include "ForgeConductor/Application/DashboardConnectionApplicationFactory.h"
#include "ForgeConductor/Application/DashboardOperationalService.h"
#include "ForgeConductor/Application/DashboardTelemetrySource.h"
#include "ForgeConductor/Application/ManagerController.h"
#include "ForgeConductor/Application/ProjectMemoryRepositoryCache.h"
#include "ForgeConductor/Dashboard/DashboardStaticAssetStore.h"
#include "ForgeConductor/Infrastructure/Windows/BCryptSha256Hasher.h"
#include "ForgeConductor/Infrastructure/Windows/DpapiSecureStorage.h"
#include "ForgeConductor/Infrastructure/Windows/InfrastructureWindows.h"
#include "ForgeConductor/Infrastructure/Windows/SecretRedactor.h"
#include "ForgeConductor/Infrastructure/Windows/SystemClock.h"
#include "ForgeConductor/Infrastructure/Windows/WindowsApplicationPaths.h"
#include "ForgeConductor/Infrastructure/Windows/WindowsAtomicFileStore.h"
#include "ForgeConductor/Infrastructure/Windows/WindowsConfigurationStore.h"
#include "ForgeConductor/Infrastructure/Windows/WindowsContinuityDocumentCodec.h"
#include "ForgeConductor/Infrastructure/Windows/WindowsCurrentUserIdentity.h"
#include "ForgeConductor/Infrastructure/Windows/WindowsDashboardBearerToken.h"
#include "ForgeConductor/Infrastructure/Windows/WindowsDashboardBrowserLauncher.h"
#include "ForgeConductor/Infrastructure/Windows/WindowsDashboardStaticAssetBundle.h"
#include "ForgeConductor/Infrastructure/Windows/WindowsDiagnosticLogTailReader.h"
#include "ForgeConductor/Infrastructure/Windows/WindowsDiagnosticSink.h"
#include "ForgeConductor/Infrastructure/Windows/WindowsLMStudioCandidateSelector.h"
#include "ForgeConductor/Infrastructure/Windows/WindowsLMStudioDeploymentService.h"
#include "ForgeConductor/Infrastructure/Windows/WindowsLMStudioEnvironment.h"
#include "ForgeConductor/Infrastructure/Windows/WindowsLMStudioHostActivator.h"
#include "ForgeConductor/Infrastructure/Windows/WindowsLMStudioServeVerifier.h"
#include "ForgeConductor/Infrastructure/Windows/WindowsManagerAuthentication.h"
#include "ForgeConductor/Infrastructure/Windows/WindowsManagerNamedPipeServer.h"
#include "ForgeConductor/Infrastructure/Windows/WindowsNativeSessionLedger.h"
#include "ForgeConductor/Infrastructure/Windows/WindowsProcessSupervisor.h"
#include "ForgeConductor/Infrastructure/Windows/WindowsProjectWorkspaceAuthority.h"
#include "ForgeConductor/Infrastructure/Windows/WindowsRuntimeDiagnostics.h"
#include "ForgeConductor/Infrastructure/Windows/WindowsUuidGenerator.h"
#include "ForgeConductor/Infrastructure/Windows/WindowsWorkspaceAuthority.h"
#include "ForgeConductor/Manager/ManagerRequestDispatcher.h"
#include "ForgeConductor/Mcp/McpExecutionServices.h"
#include "ForgeConductor/Mcp/McpToolCatalog.h"
#include "ForgeConductor/NativeTools/Windows/WindowsFileSystem.h"
#include "ForgeConductor/Persistence/Windows/WindowsAgentSessionRepository.h"
#include "ForgeConductor/Persistence/Windows/WindowsAuditRepository.h"
#include "ForgeConductor/Persistence/Windows/WindowsCentralDatabase.h"
#include "ForgeConductor/Persistence/Windows/WindowsDashboardOperationalRepository.h"
#include "ForgeConductor/Persistence/Windows/WindowsProjectMemoryArtifactStore.h"
#include "ForgeConductor/Persistence/Windows/WindowsProjectMemoryRepository.h"
#include "ForgeConductor/Persistence/Windows/WindowsProjectMemoryRepositoryOpener.h"
#include "ForgeConductor/Persistence/Windows/WindowsProjectRegistryRepository.h"
#include "ForgeConductor/SessionHost/BoundedLogicalContinuationQueue.h"
#include "ForgeConductor/SessionHost/ForgeNativeSessionHostAdapter.h"
#include "ForgeConductor/SessionHost/LocalLogicalSessionTransport.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <condition_variable>
#include <exception>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace ForgeConductor::Hosts::Manager {
namespace {

namespace Application = ForgeConductor::Application;
namespace CompositionWindows = ForgeConductor::Composition::Windows;
namespace Contracts = ForgeConductor::Contracts;
namespace Dashboard = ForgeConductor::Dashboard;
namespace Domain = ForgeConductor::Domain;
namespace InfrastructureWindows = ForgeConductor::Infrastructure::Windows;
namespace ManagerProtocol = ForgeConductor::Manager;
namespace Mcp = ForgeConductor::Mcp;
namespace NativeSessionHost = ForgeConductor::SessionHost;
namespace NativeToolsWindows = ForgeConductor::NativeTools::Windows;
namespace PersistenceWindows = ForgeConductor::Persistence::Windows;

using namespace std::chrono_literals;

constexpr std::chrono::seconds StartupTimeout{30};
constexpr std::chrono::seconds ShutdownTimeout{10};
constexpr std::string_view ProductName{"Forge Conductor"};
constexpr std::string_view ProductVersion{"0.9.0"};
constexpr std::string_view RuntimeName{"windows-manager"};
constexpr std::size_t MaximumLmStudioSelectionRoots =
    InfrastructureWindows::WindowsWorkspaceAuthority::MaximumTrustedRootsPerPolicy;

class CompositionFailure final : public std::exception {
public:
    explicit CompositionFailure(Domain::Error error) noexcept
        : error_{std::move(error)}
    {
    }

    [[nodiscard]] const Domain::Error& error() const noexcept
    {
        return error_;
    }

private:
    Domain::Error error_;
};

template <typename T>
[[nodiscard]] T take(Domain::Result<T> result)
{
    if (!result) {
        throw CompositionFailure{std::move(result).error()};
    }
    return std::move(result).value();
}

void requireSuccess(Domain::Result<void> result)
{
    if (!result) {
        throw CompositionFailure{std::move(result).error()};
    }
}

[[nodiscard]] Domain::PathText pathText(const std::string_view value)
{
    return take(Domain::PathText::create(value));
}

[[nodiscard]] Domain::Uuid nextUuid(Contracts::IUuidGenerator& generator)
{
    return take(generator.next());
}

[[nodiscard]] Domain::OperationContext makeContext(
    Contracts::IUuidGenerator& generator,
    const Contracts::IClock& clock,
    const std::chrono::seconds timeout,
    const std::string_view correlation)
{
    return Domain::OperationContext{
        Domain::OperationId{nextUuid(generator)},
        clock.monotonicNow() + timeout,
        {},
        take(Domain::CorrelationId::parse(correlation))};
}

[[nodiscard]] Domain::PathText childPath(
    const Domain::PathText& root,
    const std::string_view relative)
{
    if (relative.empty() || relative.starts_with('/') ||
        relative.starts_with('\\')) {
        throw CompositionFailure{Domain::makeError(
            Domain::ErrorCodes::InvalidRequest,
            "A Manager app-owned relative path is invalid.")};
    }
    std::string value = root.value();
    if (!value.ends_with('/') && !value.ends_with('\\')) {
        value.push_back('\\');
    }
    value.append(relative);
    return pathText(value);
}

[[nodiscard]] Domain::PathText parentPath(const Domain::PathText& file)
{
    const auto separator = file.value().find_last_of("\\/");
    if (separator == std::string::npos || separator < 2U) {
        throw CompositionFailure{Domain::makeError(
            Domain::ErrorCodes::InvalidRequest,
            "A Manager executable has no bounded parent path.")};
    }
    return pathText(file.value().substr(0U, separator));
}

[[nodiscard]] Domain::Result<std::wstring> strictUtf8ToWide(
    const std::string_view value) noexcept
{
    try {
        if (value.empty() ||
            value.size() >
                static_cast<std::size_t>((std::numeric_limits<int>::max)())) {
            return Domain::Result<std::wstring>::failure(Domain::makeError(
                Domain::ErrorCodes::InvalidRequest,
                "A Manager path could not be converted from UTF-8."));
        }
        const auto inputLength = static_cast<int>(value.size());
        const int required = ::MultiByteToWideChar(
            CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), inputLength,
            nullptr, 0);
        if (required <= 0) {
            return Domain::Result<std::wstring>::failure(Domain::makeError(
                Domain::ErrorCodes::InvalidRequest,
                "A Manager path is not valid UTF-8."));
        }
        std::wstring converted(static_cast<std::size_t>(required), L'\0');
        if (::MultiByteToWideChar(
                CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), inputLength,
                converted.data(), required) != required) {
            return Domain::Result<std::wstring>::failure(Domain::makeError(
                Domain::ErrorCodes::InternalFailure,
                "A Manager path conversion was incomplete."));
        }
        return Domain::Result<std::wstring>::success(std::move(converted));
    } catch (...) {
        return Domain::Result<std::wstring>::failure(Domain::makeError(
            Domain::ErrorCodes::InternalFailure,
            "A Manager path conversion failed safely."));
    }
}

[[nodiscard]] bool equalWindowsPath(
    const Domain::PathText& left,
    const Domain::PathText& right)
{
    const auto leftWide = take(strictUtf8ToWide(left.value()));
    const auto rightWide = take(strictUtf8ToWide(right.value()));
    return leftWide.size() == rightWide.size() &&
        ::CompareStringOrdinal(
            leftWide.data(), static_cast<int>(leftWide.size()),
            rightWide.data(), static_cast<int>(rightWide.size()), TRUE) ==
            CSTR_EQUAL;
}

[[nodiscard]] bool isWithinWindowsPath(
    const Domain::PathText& candidate,
    const Domain::PathText& root)
{
    const auto candidateWide = take(strictUtf8ToWide(candidate.value()));
    const auto rootWide = take(strictUtf8ToWide(root.value()));
    if (candidateWide.size() < rootWide.size() ||
        ::CompareStringOrdinal(
            candidateWide.data(), static_cast<int>(rootWide.size()),
            rootWide.data(), static_cast<int>(rootWide.size()), TRUE) !=
            CSTR_EQUAL) {
        return false;
    }
    return candidateWide.size() == rootWide.size() ||
        rootWide.back() == L'\\' ||
        candidateWide[rootWide.size()] == L'\\';
}

void appendSelectionRoot(
    std::vector<Domain::PathText>& roots,
    Domain::PathText root)
{
    for (auto iterator = roots.begin(); iterator != roots.end();) {
        if (equalWindowsPath(*iterator, root) ||
            isWithinWindowsPath(root, *iterator)) {
            return;
        }
        if (isWithinWindowsPath(*iterator, root)) {
            iterator = roots.erase(iterator);
            continue;
        }
        ++iterator;
    }
    if (roots.size() >= MaximumLmStudioSelectionRoots) {
        throw CompositionFailure{Domain::makeError(
            Domain::ErrorCodes::LimitExceeded,
            "LM Studio discovery exceeded the Manager selection-root bound.")};
    }
    roots.push_back(std::move(root));
}

[[nodiscard]] InfrastructureWindows::WindowsWorkspaceAuthorityPolicy
authorityPolicy(
    Domain::AuthorityId authorityId,
    Domain::ProjectId projectId,
    Domain::ClientId clientId,
    std::vector<Domain::PathText> roots,
    const Domain::FileAccess intent,
    std::vector<Domain::FileAccess> grants,
    std::vector<Domain::FileAccess> denials,
    const bool shellEnabled,
    const std::uint64_t generation = 1U)
{
    return InfrastructureWindows::WindowsWorkspaceAuthorityPolicy{
        std::move(authorityId), std::move(projectId), std::move(clientId),
        std::move(roots), intent, std::move(grants), std::move(denials),
        shellEnabled, generation};
}

[[nodiscard]] Contracts::AuthorizedPath authorizePath(
    Contracts::IWorkspaceAuthority& issuer,
    const Contracts::WorkspaceAuthority& authority,
    const Domain::PathText& target,
    const Domain::PathText& root,
    const Domain::FileAccess access,
    const Domain::OperationContext& context)
{
    return take(issuer.authorize(
        authority,
        Domain::PathAuthorizationRequest{target, root, access, false},
        context));
}

[[nodiscard]] Domain::Error internalCompositionError() noexcept
{
    return Domain::makeError(
        Domain::ErrorCodes::InternalFailure,
        "The production Manager composition failed safely.");
}

[[nodiscard]] bool isOptionalLmStudioAbsence(
    const Domain::Error& error) noexcept
{
    return error.code == Domain::ErrorCodes::RecordNotFound ||
        error.code == Domain::ErrorCodes::HostCapabilityUnavailable;
}

[[nodiscard]] std::string lmStudioUnavailableReason(
    const Domain::Error& error)
{
    if (error.code == Domain::ErrorCodes::HostCapabilityUnavailable) {
        return "LM Studio deployment is unavailable because the native host capability is not available.";
    }
    return "LM Studio deployment is unavailable because no complete native application and configuration selection was resolved.";
}

} // namespace

class ManagerCompositionRoot::Impl final {
public:
    explicit Impl(ManagerCompositionRootOptions options)
        : options_{std::move(options)},
          processEnvironment_{options_.environment, processEnvironmentProbe_},
          clock_{std::make_shared<InfrastructureWindows::SystemClock>()},
          uuidGenerator_{std::make_shared<
              InfrastructureWindows::WindowsUuidGenerator>()}
    {
        initialize();
    }

    ~Impl() noexcept { shutdown(); }

    [[nodiscard]] Domain::Result<void> run() noexcept;
    void shutdown() noexcept;

private:
    void initialize();
    void initializeFoundation(const Domain::OperationContext& context);
    void initializePersistence(const Domain::OperationContext& context);
    void initializeLmStudio(const Domain::OperationContext& context);
    void initializeDashboard(const Domain::OperationContext& context);
    void initializeManagerHost(const Domain::OperationContext& context);
    void initializeUnavailableLmStudio(
        const Domain::OperationContext& context,
        std::string reason);
    void shutdownServices(bool activeRunOwner = false) noexcept;
    void completeRunFrame() noexcept;

    [[nodiscard]] const CompositionWindows::ManagerProcessEnvironmentSnapshot&
    snapshot() const noexcept
    {
        return preparedEnvironment_->snapshot();
    }

    // Declared first so reverse member destruction releases the per-user lease
    // only after every later service, callback, worker, and repository.
    std::optional<CompositionWindows::PreparedManagerProcessEnvironment>
        preparedEnvironment_;

    const ManagerCompositionRootOptions options_;
    CompositionWindows::WindowsManagerProcessEnvironmentPlatformProbe
        processEnvironmentProbe_;
    CompositionWindows::ManagerProcessEnvironment processEnvironment_;

    std::shared_ptr<InfrastructureWindows::SystemClock> clock_;
    std::shared_ptr<InfrastructureWindows::WindowsUuidGenerator> uuidGenerator_;
    std::shared_ptr<InfrastructureWindows::BCryptSha256Hasher> hasher_;
    std::shared_ptr<InfrastructureWindows::SecretRedactor> redactor_;
    std::shared_ptr<InfrastructureWindows::WindowsAtomicFileStore>
        atomicFileStore_;
    std::shared_ptr<InfrastructureWindows::WindowsApplicationPaths>
        applicationPaths_;
    std::shared_ptr<InfrastructureWindows::WindowsRuntimeDiagnostics>
        runtimeDiagnostics_;
    std::shared_ptr<InfrastructureWindows::WindowsProcessSupervisor>
        processSupervisor_;

    std::optional<Domain::ClientId> managerClientId_;
    std::shared_ptr<InfrastructureWindows::WindowsWorkspaceAuthority>
        dataAuthority_;
    std::optional<Contracts::WorkspaceAuthority> dataScope_;
    std::shared_ptr<InfrastructureWindows::WindowsDiagnosticSink>
        diagnosticSink_;
    std::unique_ptr<InfrastructureWindows::WindowsDiagnosticLogTailReader>
        diagnosticLogTailReader_;

    std::unique_ptr<InfrastructureWindows::DpapiSecureStorage> secureStorage_;
    InfrastructureWindows::WindowsManagerAuthenticationTokenGenerator
        managerAuthenticationGenerator_;
    std::unique_ptr<
        InfrastructureWindows::WindowsManagerAuthenticationTokenStore>
        managerAuthenticationStore_;
    InfrastructureWindows::WindowsDashboardBearerTokenGenerator
        dashboardBearerGenerator_;
    std::unique_ptr<InfrastructureWindows::WindowsDashboardBearerTokenStore>
        dashboardBearerStore_;
    std::optional<Domain::Sha256Digest> managerNonce_;
    std::optional<Domain::Sha256Digest> dashboardBearer_;
    std::shared_ptr<InfrastructureWindows::WindowsConfigurationStore>
        configurationStore_;
    std::optional<Domain::AppConfig> initialConfiguration_;

    std::shared_ptr<PersistenceWindows::WindowsCentralDatabase> centralDatabase_;
    std::shared_ptr<PersistenceWindows::WindowsAgentSessionRepository>
        agentSessionRepository_;
    std::shared_ptr<PersistenceWindows::WindowsAuditRepository> auditRepository_;
    std::shared_ptr<
        PersistenceWindows::WindowsDashboardOperationalRepository>
        dashboardOperationalRepository_;
    std::unique_ptr<PersistenceWindows::WindowsProjectRegistryRepository>
        projectRegistry_;
    std::unique_ptr<InfrastructureWindows::WindowsProjectWorkspaceAuthority>
        projectWorkspaceAuthority_;
    std::shared_ptr<NativeToolsWindows::WindowsFileSystem> fileSystem_;
    std::shared_ptr<PersistenceWindows::WindowsProjectMemoryArtifactStore>
        projectArtifactStore_;
    std::shared_ptr<
        PersistenceWindows::WindowsProjectMemoryRepositoryOpener>
        projectRepositoryOpener_;
    std::unique_ptr<Application::ProjectMemoryRepositoryCache>
        projectRepositoryCache_;
    std::unique_ptr<Application::AgentCatalog> agentCatalog_;
    std::unique_ptr<Contracts::IAgentCompletionReportInspector> reportInspector_;
    std::unique_ptr<Application::AgentSessionService> agentSessions_;
    std::unique_ptr<InfrastructureWindows::WindowsContinuityDocumentCodec>
        continuityCodec_;
    std::unique_ptr<InfrastructureWindows::WindowsNativeSessionLedger>
        nativeSessionLedger_;
    std::unique_ptr<NativeSessionHost::BoundedLogicalContinuationQueue>
        logicalContinuationQueue_;
    std::unique_ptr<NativeSessionHost::LocalLogicalSessionTransport>
        nativeSessionTransport_;
    std::unique_ptr<NativeSessionHost::ForgeNativeSessionHostAdapter>
        nativeSessionAdapter_;
    std::unique_ptr<Application::ContinuityCoordinator> continuity_;
    std::unique_ptr<Mcp::McpToolCatalog> toolCatalog_;
    std::unique_ptr<Mcp::McpToolAuthorizer> toolAuthorizer_;

    std::unique_ptr<InfrastructureWindows::WindowsLMStudioDiscoverySource>
        lmStudioDiscovery_;
    std::unique_ptr<InfrastructureWindows::WindowsWorkspaceAuthority>
        lmStudioSelectionIssuer_;
    std::optional<Contracts::WorkspaceAuthority> lmStudioSelectionAuthority_;
    std::optional<CompositionWindows::ManagerLmStudioReadScope>
        lmStudioReadScope_;
    std::unique_ptr<InfrastructureWindows::WindowsWorkspaceAuthority>
        lmStudioUnavailableReadIssuer_;
    std::unique_ptr<InfrastructureWindows::WindowsWorkspaceAuthority>
        lmStudioWriteIssuer_;
    std::optional<Contracts::WorkspaceAuthority> lmStudioReadAuthority_;
    std::optional<Contracts::WorkspaceAuthority> lmStudioWriteAuthority_;
    std::unique_ptr<CompositionWindows::ManagerLmStudioAuthorityRouter>
        lmStudioAuthorityRouter_;
    std::unique_ptr<InfrastructureWindows::WindowsLMStudioEnvironment>
        lmStudioEnvironment_;
    std::unique_ptr<InfrastructureWindows::WindowsLMStudioServeVerifier>
        lmStudioServeVerifier_;
    std::unique_ptr<InfrastructureWindows::WindowsLMStudioHostActivator>
        lmStudioHostActivator_;
    std::unique_ptr<Contracts::ILMStudioDeploymentService> lmStudioDeployment_;
    std::shared_ptr<CompositionWindows::ManagerMaintenanceService>
        maintenanceService_;

    std::unique_ptr<CompositionWindows::UnavailableTelemetryService>
        telemetryService_;
    std::unique_ptr<Application::DashboardTelemetrySource> telemetrySource_;
    std::unique_ptr<Dashboard::DashboardStaticAssetStore> dashboardAssets_;
    std::unique_ptr<CompositionWindows::WindowsManagerDoctorPlatformProbe>
        doctorPlatformProbe_;
    std::unique_ptr<CompositionWindows::ManagerDoctorService> doctorService_;
    std::unique_ptr<CompositionWindows::ManagerDashboardOperationalDataSource>
        dashboardOperationalDataSource_;
    std::unique_ptr<Application::DashboardOperationalService>
        dashboardOperationalService_;

    ManagerProcessRestartSignal restartSignal_;
    ManagerProcessStopSignal stopSignal_;
    std::unique_ptr<ManagerControllerClient> managerControllerClient_;
    std::shared_ptr<Application::DashboardConnectionApplicationFactory>
        dashboardApplicationFactory_;
    std::shared_ptr<WindowsManagerRuntime> managerRuntime_;
    std::shared_ptr<Application::ManagerController> managerController_;
    std::shared_ptr<ManagerProtocol::ManagerRequestDispatcher> dispatcher_;

    std::shared_ptr<InfrastructureWindows::WindowsWorkspaceAuthority>
        browserAuthority_;
    std::optional<Contracts::WorkspaceAuthority> browserScope_;
    std::shared_ptr<ManagerProcessHost> processHost_;
    std::shared_ptr<ManagerProcessHostShutdownTarget> processShutdownTarget_;
    std::unique_ptr<ManagerProcessStopWatcher> processStopWatcher_;

    std::mutex lifecycleMutex_;
    std::condition_variable lifecycleChanged_;
    std::thread::id runThreadId_;
    bool runInvoked_{};
    bool runActive_{};
    bool shutdownRequested_{};
    bool servicesShutdownClaimed_{};
    bool servicesShutdown_{};
};

void ManagerCompositionRoot::Impl::initialize()
{
    const auto inspectionContext = makeContext(
        *uuidGenerator_, *clock_, StartupTimeout,
        "manager-environment-inspection");
    const auto inspected = take(processEnvironment_.inspect(inspectionContext));

    // A Scheduler-projected --home value is never used as authority. Resolve
    // it independently and reject a mismatch before lease acquisition or the
    // first app-owned directory mutation.
    if (options_.expectedHome) {
        InfrastructureWindows::WindowsApplicationPaths expectedPaths{
            InfrastructureWindows::WindowsApplicationPathsOptions{
                options_.expectedHome, false}};
        const auto assertedHome = take(expectedPaths.dataRoot(inspectionContext));
        if (!equalWindowsPath(assertedHome, inspected.dataRoot())) {
            throw CompositionFailure{Domain::makeError(
                Domain::ErrorCodes::Conflict,
                "The Manager --home assertion does not match the canonical current-user application root.")};
        }
    }

    const auto ownerIdentity = take(
        InfrastructureWindows::WindowsCurrentUserIdentity::load());
    auto lease = take(InfrastructureWindows::WindowsManagerInstanceLease::acquire(
        ownerIdentity, options_.instanceLease));
    const auto preparationContext = makeContext(
        *uuidGenerator_, *clock_, StartupTimeout,
        "manager-environment-preparation");
    preparedEnvironment_.emplace(take(processEnvironment_.prepareAfterLease(
        inspected, std::move(lease), preparationContext)));

    const auto compositionContext = makeContext(
        *uuidGenerator_, *clock_, StartupTimeout,
        "manager-production-composition");
    initializeFoundation(compositionContext);
    initializePersistence(compositionContext);
    initializeLmStudio(compositionContext);
    initializeDashboard(compositionContext);
    initializeManagerHost(compositionContext);
}

void ManagerCompositionRoot::Impl::initializeFoundation(
    const Domain::OperationContext& context)
{
    const auto& process = snapshot();
    hasher_ = std::make_shared<InfrastructureWindows::BCryptSha256Hasher>();
    redactor_ = std::make_shared<InfrastructureWindows::SecretRedactor>();
    atomicFileStore_ =
        std::make_shared<InfrastructureWindows::WindowsAtomicFileStore>();
    applicationPaths_ =
        std::make_shared<InfrastructureWindows::WindowsApplicationPaths>(
            InfrastructureWindows::WindowsApplicationPathsOptions{
                process.dataRoot(), false});

    if (!equalWindowsPath(
            take(applicationPaths_->dataRoot(context)), process.dataRoot()) ||
        !equalWindowsPath(
            take(applicationPaths_->configurationRoot(context)),
            process.configurationRoot()) ||
        !equalWindowsPath(
            take(applicationPaths_->diagnosticsRoot(context)),
            process.diagnosticsRoot())) {
        throw CompositionFailure{Domain::makeError(
            Domain::ErrorCodes::IntegrityFailure,
            "The Manager application paths diverged from the prepared process environment.")};
    }

    runtimeDiagnostics_ = std::make_shared<
        InfrastructureWindows::WindowsRuntimeDiagnostics>(
        *clock_, process.resourceBudgets());
    processSupervisor_ = std::make_shared<
        InfrastructureWindows::WindowsProcessSupervisor>(
        process.resourceBudgets(), runtimeDiagnostics_);
    managerClientId_.emplace(take(Domain::ClientId::parse(
        nextUuid(*uuidGenerator_).value())));

    const Domain::AuthorityId dataAuthorityId{nextUuid(*uuidGenerator_)};
    const Domain::ProjectId dataProjectId{nextUuid(*uuidGenerator_)};
    dataAuthority_ = std::make_shared<
        InfrastructureWindows::WindowsWorkspaceAuthority>(
        std::vector<InfrastructureWindows::WindowsWorkspaceAuthorityPolicy>{
            authorityPolicy(
                dataAuthorityId, dataProjectId, *managerClientId_,
                {process.dataRoot()}, Domain::FileAccess::Write,
                {Domain::FileAccess::Read, Domain::FileAccess::Write,
                 Domain::FileAccess::Create, Domain::FileAccess::Delete},
                {Domain::FileAccess::Execute}, false)});
    dataScope_.emplace(take(dataAuthority_->authorityFor(
        dataProjectId, context)));

    diagnosticSink_ = std::make_shared<
        InfrastructureWindows::WindowsDiagnosticSink>(
        InfrastructureWindows::WindowsDiagnosticSinkOptions{
            process.diagnosticsRoot(), process.exportRoot(),
            process.resourceBudgets(), options_.enableEtw},
        clock_, redactor_, hasher_, dataAuthority_, atomicFileStore_);
    diagnosticLogTailReader_ = std::make_unique<
        InfrastructureWindows::WindowsDiagnosticLogTailReader>(
        process.diagnosticsRoot());

    std::wstring secureStorageSubkey = options_.secureStorageRegistrySubkey
        ? *options_.secureStorageRegistrySubkey
        : std::wstring{
              InfrastructureWindows::DpapiSecureStorage::DefaultRegistrySubkey};
    secureStorage_ = std::make_unique<
        InfrastructureWindows::DpapiSecureStorage>(
        std::move(secureStorageSubkey));
    managerAuthenticationStore_ = std::make_unique<
        InfrastructureWindows::WindowsManagerAuthenticationTokenStore>(
        *secureStorage_, managerAuthenticationGenerator_);
    dashboardBearerStore_ = std::make_unique<
        InfrastructureWindows::WindowsDashboardBearerTokenStore>(
        *secureStorage_, dashboardBearerGenerator_);
    managerNonce_.emplace(take(
        managerAuthenticationStore_->loadOrCreate(context)));
    dashboardBearer_.emplace(take(
        dashboardBearerStore_->loadOrCreate(context)));
    if (*managerNonce_ == *dashboardBearer_) {
        throw CompositionFailure{Domain::makeError(
            Domain::ErrorCodes::IntegrityFailure,
            "The Manager pipe nonce and dashboard bearer are not distinct.")};
    }

    const auto configurationPath =
        childPath(process.configurationRoot(), "config.json");
    configurationStore_ = std::make_shared<
        InfrastructureWindows::WindowsConfigurationStore>(
        *atomicFileStore_,
        authorizePath(
            *dataAuthority_, *dataScope_, configurationPath,
            process.dataRoot(), Domain::FileAccess::Read, context),
        authorizePath(
            *dataAuthority_, *dataScope_, configurationPath,
            process.dataRoot(), Domain::FileAccess::Write, context),
        authorizePath(
            *dataAuthority_, *dataScope_, configurationPath,
            process.dataRoot(), Domain::FileAccess::Create, context),
        authorizePath(
            *dataAuthority_, *dataScope_,
            childPath(process.configurationRoot(), "config.json.bak"),
            process.dataRoot(), Domain::FileAccess::Read, context));
    initialConfiguration_.emplace(take(configurationStore_->load(context)));
}

void ManagerCompositionRoot::Impl::initializePersistence(
    const Domain::OperationContext& context)
{
    const auto& process = snapshot();
    const auto projectMemoryLimits =
        Domain::projectMemoryLimitsForProfile(process.resourceProfile());

    auto centralDatabase = take(PersistenceWindows::WindowsCentralDatabase::open(
        applicationPaths_, runtimeDiagnostics_, clock_, context));
    centralDatabase_ = std::shared_ptr<
        PersistenceWindows::WindowsCentralDatabase>{
        std::move(centralDatabase)};
    agentSessionRepository_ = take(
        PersistenceWindows::WindowsAgentSessionRepository::attach(
            centralDatabase_, clock_));
    auditRepository_ = take(
        PersistenceWindows::WindowsAuditRepository::attach(centralDatabase_));
    dashboardOperationalRepository_ = take(
        PersistenceWindows::WindowsDashboardOperationalRepository::attach(
            centralDatabase_));

    const auto registryPath =
        childPath(process.projectsRoot(), "registry.json");
    projectRegistry_ = std::make_unique<
        PersistenceWindows::WindowsProjectRegistryRepository>(
        applicationPaths_, atomicFileStore_,
        PersistenceWindows::WindowsProjectRegistryStoragePaths{
            authorizePath(
                *dataAuthority_, *dataScope_, registryPath,
                process.dataRoot(), Domain::FileAccess::Read, context),
            authorizePath(
                *dataAuthority_, *dataScope_, registryPath,
                process.dataRoot(), Domain::FileAccess::Write, context),
            authorizePath(
                *dataAuthority_, *dataScope_, registryPath,
                process.dataRoot(), Domain::FileAccess::Create, context),
            authorizePath(
                *dataAuthority_, *dataScope_,
                childPath(process.projectsRoot(), "registry.json.bak"),
                process.dataRoot(), Domain::FileAccess::Read, context)},
        uuidGenerator_, hasher_, clock_, projectMemoryLimits);
    projectWorkspaceAuthority_ = std::make_unique<
        InfrastructureWindows::WindowsProjectWorkspaceAuthority>(
        *projectRegistry_, *uuidGenerator_, *managerClientId_, false);

    fileSystem_ =
        std::make_shared<NativeToolsWindows::WindowsFileSystem>(
            atomicFileStore_);
    projectArtifactStore_ = std::make_shared<
        PersistenceWindows::WindowsProjectMemoryArtifactStore>(
        applicationPaths_, uuidGenerator_);
    projectRepositoryOpener_ = std::make_shared<
        PersistenceWindows::WindowsProjectMemoryRepositoryOpener>(
        applicationPaths_, projectArtifactStore_, runtimeDiagnostics_,
        redactor_, hasher_, uuidGenerator_, clock_,
        PersistenceWindows::WindowsProjectMemoryRepositoryOptions{
            {}, projectMemoryLimits});
    projectRepositoryCache_ = std::make_unique<
        Application::ProjectMemoryRepositoryCache>(
        projectRepositoryOpener_,
        process.resourceBudgets().openProjectRepositoriesMaximum);

    agentCatalog_ = take(Application::AgentCatalog::create(
        clock_, std::span<const Application::AgentDefinitionDocument>{},
        context));
    reportInspector_ =
        InfrastructureWindows::createWindowsAgentCompletionReportInspector(
            *clock_);
    if (!reportInspector_) {
        throw CompositionFailure{Domain::makeError(
            Domain::ErrorCodes::InternalFailure,
            "The Manager agent completion report inspector was not created.")};
    }
    agentSessions_ = std::make_unique<Application::AgentSessionService>(
        *agentCatalog_, *agentSessionRepository_, *reportInspector_,
        *projectWorkspaceAuthority_, *clock_, *uuidGenerator_,
        initialConfiguration_->sessions.idleTimeToLive);

    continuityCodec_ = std::make_unique<
        InfrastructureWindows::WindowsContinuityDocumentCodec>(
        hasher_, clock_);
    const auto ledgerPath =
        childPath(process.dataRoot(), "native-session-ledger.json");
    nativeSessionLedger_ = std::make_unique<
        InfrastructureWindows::WindowsNativeSessionLedger>(
        *atomicFileStore_, *hasher_,
        authorizePath(
            *dataAuthority_, *dataScope_, ledgerPath,
            process.dataRoot(), Domain::FileAccess::Read, context),
        authorizePath(
            *dataAuthority_, *dataScope_, ledgerPath,
            process.dataRoot(), Domain::FileAccess::Write, context),
        authorizePath(
            *dataAuthority_, *dataScope_, ledgerPath,
            process.dataRoot(), Domain::FileAccess::Create, context),
        authorizePath(
            *dataAuthority_, *dataScope_,
            childPath(process.dataRoot(), "native-session-ledger.json.bak"),
            process.dataRoot(), Domain::FileAccess::Read, context));
    logicalContinuationQueue_ = std::make_unique<
        NativeSessionHost::BoundedLogicalContinuationQueue>();
    nativeSessionTransport_ = std::make_unique<
        NativeSessionHost::LocalLogicalSessionTransport>(
        hasher_, *continuityCodec_, *logicalContinuationQueue_);
    nativeSessionAdapter_ = std::make_unique<
        NativeSessionHost::ForgeNativeSessionHostAdapter>(
        take(Domain::AdapterId::parse(
            NativeSessionHost::ForgeNativeSessionHostAdapter::
                AdapterIdentifier)),
        *nativeSessionLedger_, *nativeSessionTransport_, *continuityCodec_,
        *uuidGenerator_, *clock_);
    continuity_ = std::make_unique<Application::ContinuityCoordinator>(
        *projectRegistry_, *projectRepositoryCache_,
        *nativeSessionAdapter_, *clock_);

    toolCatalog_ = take(Mcp::McpToolCatalog::create());
    toolAuthorizer_ = std::make_unique<Mcp::McpToolAuthorizer>(*clock_);
}

void ManagerCompositionRoot::Impl::initializeUnavailableLmStudio(
    const Domain::OperationContext& context,
    std::string reason)
{
    const auto& process = snapshot();
    std::vector<Domain::PathText> roots;
    roots.reserve(2U);
    appendSelectionRoot(roots, parentPath(process.cliExecutable()));
    appendSelectionRoot(roots, process.dataRoot());

    const Domain::ProjectId projectId{nextUuid(*uuidGenerator_)};
    const Domain::AuthorityId readAuthorityId{nextUuid(*uuidGenerator_)};
    const Domain::AuthorityId writeAuthorityId{nextUuid(*uuidGenerator_)};
    lmStudioUnavailableReadIssuer_ = std::make_unique<
        InfrastructureWindows::WindowsWorkspaceAuthority>(
        std::vector<InfrastructureWindows::WindowsWorkspaceAuthorityPolicy>{
            authorityPolicy(
                readAuthorityId, projectId, *managerClientId_, roots,
                Domain::FileAccess::Read,
                {Domain::FileAccess::Read},
                {Domain::FileAccess::Write, Domain::FileAccess::Create,
                 Domain::FileAccess::Delete, Domain::FileAccess::Execute},
                false)});
    lmStudioWriteIssuer_ = std::make_unique<
        InfrastructureWindows::WindowsWorkspaceAuthority>(
        std::vector<InfrastructureWindows::WindowsWorkspaceAuthorityPolicy>{
            authorityPolicy(
                writeAuthorityId, projectId, *managerClientId_, roots,
                Domain::FileAccess::Write,
                {Domain::FileAccess::Read, Domain::FileAccess::Write,
                 Domain::FileAccess::Create, Domain::FileAccess::Delete,
                 Domain::FileAccess::Execute},
                {}, true)});
    lmStudioReadAuthority_.emplace(take(
        lmStudioUnavailableReadIssuer_->authorityFor(projectId, context)));
    lmStudioWriteAuthority_.emplace(take(
        lmStudioWriteIssuer_->authorityFor(projectId, context)));
    lmStudioDeployment_ = std::make_unique<
        CompositionWindows::UnavailableLmStudioDeploymentService>(
        *clock_, std::move(reason));
}

void ManagerCompositionRoot::Impl::initializeLmStudio(
    const Domain::OperationContext& context)
{
    const auto& process = snapshot();
    if (!options_.enableExternalHostMaintenance) {
        initializeUnavailableLmStudio(
            context,
            "LM Studio deployment is disabled for this controlled Manager composition.");
        maintenanceService_ = std::make_shared<
            CompositionWindows::ManagerMaintenanceService>(
            *agentSessions_, *continuity_, *lmStudioDeployment_,
            *toolAuthorizer_, *uuidGenerator_, *clock_,
            CompositionWindows::ManagerMaintenanceServiceConfiguration{
                process.cliExecutable(), *lmStudioReadAuthority_,
                *lmStudioWriteAuthority_});
        return;
    }

    lmStudioDiscovery_ = std::make_unique<
        InfrastructureWindows::WindowsLMStudioDiscoverySource>();

    bool availableGraphComposed = false;
    try {
        auto candidatesResult = lmStudioDiscovery_->discover(context);
        if (!candidatesResult) {
            throw CompositionFailure{std::move(candidatesResult).error()};
        }
        auto candidates = std::move(candidatesResult).value();
        if (candidates.empty()) {
            throw CompositionFailure{Domain::makeError(
                Domain::ErrorCodes::RecordNotFound,
                "LM Studio discovery returned no candidate resources.")};
        }

        std::vector<Domain::PathText> selectionRoots;
        selectionRoots.reserve((std::min)(
            candidates.size(), MaximumLmStudioSelectionRoots));
        for (const auto& candidate : candidates) {
            if (candidate.applicationExecutable) {
                appendSelectionRoot(
                    selectionRoots,
                    parentPath(*candidate.applicationExecutable));
            }
            if (candidate.configurationPath) {
                appendSelectionRoot(
                    selectionRoots,
                    parentPath(*candidate.configurationPath));
            }
        }
        if (selectionRoots.empty()) {
            throw CompositionFailure{Domain::makeError(
                Domain::ErrorCodes::RecordNotFound,
                "LM Studio discovery returned no authority-bound resources.")};
        }

        const Domain::ProjectId maintenanceProjectId{
            nextUuid(*uuidGenerator_)};
        const Domain::AuthorityId selectionAuthorityId{
            nextUuid(*uuidGenerator_)};
        const Domain::AuthorityId readAuthorityId{
            nextUuid(*uuidGenerator_)};
        const Domain::AuthorityId writeAuthorityId{
            nextUuid(*uuidGenerator_)};
        lmStudioSelectionIssuer_ = std::make_unique<
            InfrastructureWindows::WindowsWorkspaceAuthority>(
            std::vector<
                InfrastructureWindows::WindowsWorkspaceAuthorityPolicy>{
                authorityPolicy(
                    selectionAuthorityId, maintenanceProjectId,
                    *managerClientId_, selectionRoots,
                    Domain::FileAccess::Read,
                    {Domain::FileAccess::Read},
                    {Domain::FileAccess::Write, Domain::FileAccess::Create,
                     Domain::FileAccess::Delete,
                     Domain::FileAccess::Execute},
                    false)});
        lmStudioSelectionAuthority_.emplace(take(
            lmStudioSelectionIssuer_->authorityFor(
                maintenanceProjectId, context)));

        InfrastructureWindows::WindowsLMStudioCandidateSelector selector{
            *lmStudioSelectionIssuer_, *fileSystem_};
        auto selection = take(selector.select(
            std::move(candidates), *lmStudioSelectionAuthority_, context));
        CompositionWindows::ManagerLmStudioReadScopeResolver resolver{
            CompositionWindows::ManagerLmStudioReadScopeConfiguration{
                CompositionWindows::ManagerLmStudioReadScopeIdentity{
                    selectionAuthorityId, maintenanceProjectId,
                    *managerClientId_, 1U},
                CompositionWindows::ManagerLmStudioReadScopeIdentity{
                    readAuthorityId, maintenanceProjectId,
                    *managerClientId_, 1U},
                process.dataRoot(), process.cliExecutable()}};
        lmStudioReadScope_.emplace(take(resolver.resolve(selection, context)));
        lmStudioReadAuthority_.emplace(
            lmStudioReadScope_->authority());

        lmStudioWriteIssuer_ = std::make_unique<
            InfrastructureWindows::WindowsWorkspaceAuthority>(
            std::vector<
                InfrastructureWindows::WindowsWorkspaceAuthorityPolicy>{
                authorityPolicy(
                    writeAuthorityId, maintenanceProjectId,
                    *managerClientId_,
                    lmStudioReadAuthority_->trustedRoots(),
                    Domain::FileAccess::Write,
                    {Domain::FileAccess::Read, Domain::FileAccess::Write,
                     Domain::FileAccess::Create, Domain::FileAccess::Delete,
                     Domain::FileAccess::Execute},
                    {}, true)});
        lmStudioWriteAuthority_.emplace(take(
            lmStudioWriteIssuer_->authorityFor(
                maintenanceProjectId, context)));
        lmStudioAuthorityRouter_ = std::make_unique<
            CompositionWindows::ManagerLmStudioAuthorityRouter>(
            lmStudioReadScope_->issuer(), *lmStudioReadAuthority_,
            *lmStudioWriteIssuer_, *lmStudioWriteAuthority_);

        lmStudioEnvironment_ = std::make_unique<
            InfrastructureWindows::WindowsLMStudioEnvironment>(
            *lmStudioAuthorityRouter_, *fileSystem_, *lmStudioDiscovery_);
        lmStudioServeVerifier_ = std::make_unique<
            InfrastructureWindows::WindowsLMStudioServeVerifier>(
            *processSupervisor_);
        lmStudioHostActivator_ = std::make_unique<
            InfrastructureWindows::WindowsLMStudioHostActivator>(
            *lmStudioAuthorityRouter_, *fileSystem_, *clock_);
        lmStudioDeployment_ = std::make_unique<
            InfrastructureWindows::WindowsLMStudioDeploymentService>(
            *lmStudioEnvironment_, *lmStudioServeVerifier_,
            *lmStudioHostActivator_, *lmStudioAuthorityRouter_,
            *fileSystem_, *atomicFileStore_, *applicationPaths_,
            *clock_, *uuidGenerator_, *diagnosticSink_);
        availableGraphComposed = true;
    } catch (const CompositionFailure& failure) {
        if (failure.error().code == Domain::ErrorCodes::Cancelled ||
            failure.error().code == Domain::ErrorCodes::DeadlineExceeded) {
            throw;
        }
        if (!isOptionalLmStudioAbsence(failure.error())) {
            throw;
        }
        if (lmStudioDeployment_) {
            lmStudioDeployment_->shutdown();
            lmStudioDeployment_.reset();
        }
        if (lmStudioHostActivator_) {
            lmStudioHostActivator_->shutdown();
            lmStudioHostActivator_.reset();
        }
        if (lmStudioServeVerifier_) {
            lmStudioServeVerifier_->shutdown();
            lmStudioServeVerifier_.reset();
        }
        if (lmStudioEnvironment_) {
            lmStudioEnvironment_->shutdown();
            lmStudioEnvironment_.reset();
        }
        lmStudioAuthorityRouter_.reset();
        lmStudioWriteAuthority_.reset();
        lmStudioReadAuthority_.reset();
        lmStudioWriteIssuer_.reset();
        lmStudioUnavailableReadIssuer_.reset();
        lmStudioReadScope_.reset();
        lmStudioSelectionAuthority_.reset();
        lmStudioSelectionIssuer_.reset();
        lmStudioDiscovery_->shutdown();
        lmStudioDiscovery_.reset();

        initializeUnavailableLmStudio(
            context, lmStudioUnavailableReason(failure.error()));
        availableGraphComposed = true;
    }

    if (!availableGraphComposed) {
        throw CompositionFailure{Domain::makeError(
            Domain::ErrorCodes::InternalFailure,
            "The Manager LM Studio composition did not resolve an implementation.")};
    }

    maintenanceService_ = std::make_shared<
        CompositionWindows::ManagerMaintenanceService>(
        *agentSessions_, *continuity_, *lmStudioDeployment_,
        *toolAuthorizer_, *uuidGenerator_, *clock_,
        CompositionWindows::ManagerMaintenanceServiceConfiguration{
            process.cliExecutable(), *lmStudioReadAuthority_,
            *lmStudioWriteAuthority_});
}

void ManagerCompositionRoot::Impl::initializeDashboard(
    const Domain::OperationContext& context)
{
    const auto& process = snapshot();
    telemetryService_ = std::make_unique<
        CompositionWindows::UnavailableTelemetryService>(*clock_);
    requireSuccess(telemetryService_->start(context));
    telemetrySource_ = take(Application::DashboardTelemetrySource::create(
        *telemetryService_, *runtimeDiagnostics_, *clock_,
        Application::DashboardTelemetrySourceConfiguration{
            process.resourceBudgets(),
            Dashboard::DashboardSseBroadcaster::HardMaximumSubscriptions,
            Dashboard::DashboardApplicationJsonCodec::MaximumResponseBytes,
            true,
            true,
            false}));
    dashboardAssets_ = take(
        InfrastructureWindows::WindowsDashboardStaticAssetBundle::create());

    doctorPlatformProbe_ = std::make_unique<
        CompositionWindows::WindowsManagerDoctorPlatformProbe>(*clock_);
    doctorService_ = take(CompositionWindows::ManagerDoctorService::create(
        CompositionWindows::ManagerDoctorServiceConfiguration{
            std::string{ProductVersion},
            process.dataRoot(),
            childPath(process.dataRoot(), "store.sqlite"),
            process.cliExecutable(),
            process.resourceBudgets(),
            *lmStudioReadAuthority_},
        *doctorPlatformProbe_, *agentCatalog_, *agentSessionRepository_,
        *telemetryService_, *lmStudioDeployment_, *clock_));
    dashboardOperationalDataSource_ = std::make_unique<
        CompositionWindows::ManagerDashboardOperationalDataSource>(
        *dashboardOperationalRepository_, *agentSessionRepository_,
        *diagnosticLogTailReader_, *clock_);
    dashboardOperationalService_ = std::make_unique<
        Application::DashboardOperationalService>(
        Application::DashboardOperationalServiceDependencies{
            *agentCatalog_, *agentSessions_, *auditRepository_,
            *doctorService_, *runtimeDiagnostics_, *toolCatalog_, *clock_,
            *dashboardOperationalDataSource_});

    managerControllerClient_ = std::make_unique<ManagerControllerClient>(
        restartSignal_, stopSignal_);
    dashboardApplicationFactory_ = std::make_shared<
        Application::DashboardConnectionApplicationFactory>(
        process.resourceBudgets(),
        Dashboard::DashboardApplicationIdentity{
            std::string{ProductName}, std::string{ProductVersion},
            std::string{RuntimeName}, process.dataRoot(),
            childPath(process.dataRoot(), "store.sqlite"),
            static_cast<std::uint32_t>(::GetCurrentProcessId())},
        *dashboardBearer_, *dashboardAssets_, *telemetrySource_,
        *dashboardOperationalService_, *managerControllerClient_);
    auto managerRuntime = take(WindowsManagerRuntime::create(
        clock_, uuidGenerator_, dashboardApplicationFactory_));
    managerRuntime_ = std::shared_ptr<WindowsManagerRuntime>{
        std::move(managerRuntime)};
    managerController_ = std::make_shared<Application::ManagerController>(
        configurationStore_, managerRuntime_, clock_,
        Domain::ManagerControllerOptions{
            process.dataRoot(), std::string{ProductVersion},
            static_cast<std::uint32_t>(::GetCurrentProcessId())});
    requireSuccess(managerControllerClient_->bind(managerController_));
    dispatcher_ = std::make_shared<
        ManagerProtocol::ManagerRequestDispatcher>(
        managerController_, clock_);
}

void ManagerCompositionRoot::Impl::initializeManagerHost(
    const Domain::OperationContext& context)
{
    const auto& process = snapshot();
    const auto ownerIdentity = take(
        InfrastructureWindows::WindowsCurrentUserIdentity::load());
    auto server = take(
        InfrastructureWindows::WindowsManagerNamedPipeServer::create(
            clock_, dispatcher_, ownerIdentity, *managerNonce_,
            InfrastructureWindows::WindowsManagerNamedPipeServerOptions{
                std::wstring{preparedEnvironment_->lease().pipeName()},
                ManagerProtocol::ManagerTransportLimits{},
                4U,
                2s}));

    std::vector<std::unique_ptr<IManagerTransitionWorker>> workers;
    workers.reserve(2U);
    workers.push_back(std::make_unique<ManagerTransitionWorker>(
        managerController_, clock_, uuidGenerator_, restartSignal_));
    workers.push_back(std::make_unique<ManagerMaintenanceWorker>(
        maintenanceService_, clock_, uuidGenerator_));
    auto workerGroup = std::make_unique<ManagerProcessWorkerGroup>(
        std::move(workers));

    const Domain::AuthorityId browserAuthorityId{nextUuid(*uuidGenerator_)};
    const Domain::ProjectId browserProjectId{nextUuid(*uuidGenerator_)};
    browserAuthority_ = std::make_shared<
        InfrastructureWindows::WindowsWorkspaceAuthority>(
        std::vector<InfrastructureWindows::WindowsWorkspaceAuthorityPolicy>{
            authorityPolicy(
                browserAuthorityId, browserProjectId, *managerClientId_,
                {parentPath(process.managerExecutable())},
                Domain::FileAccess::Execute,
                {Domain::FileAccess::Execute}, {}, true)});
    browserScope_.emplace(take(browserAuthority_->authorityFor(
        browserProjectId, context)));
    auto browser = std::make_unique<
        InfrastructureWindows::WindowsDashboardBrowserLauncher>(
        *clock_, *diagnosticSink_, processSupervisor_,
        InfrastructureWindows::WindowsDashboardBrowserLaunchConfiguration{
            process.managerExecutable(), process.cliExecutable(),
            *browserScope_, 5s},
        *dashboardBearer_);

    processHost_ = std::make_shared<ManagerProcessHost>(
        managerController_, dispatcher_, std::move(server),
        std::move(workerGroup), std::move(browser),
        ManagerProcessHostOptions{options_.openBrowserOverride});
    processShutdownTarget_ =
        std::make_shared<ManagerProcessHostShutdownTarget>(processHost_);
    processStopWatcher_ = std::make_unique<ManagerProcessStopWatcher>(
        stopSignal_, processShutdownTarget_);
}

Domain::Result<void> ManagerCompositionRoot::Impl::run() noexcept
{
    try {
        {
            const std::lock_guard lock{lifecycleMutex_};
            if (runInvoked_) {
                return Domain::Result<void>::failure(Domain::makeError(
                    Domain::ErrorCodes::Conflict,
                    "The production Manager composition may run only once."));
            }
            if (shutdownRequested_) {
                return Domain::Result<void>::failure(Domain::makeError(
                    Domain::ErrorCodes::TransportClosed,
                    "The production Manager composition is shut down."));
            }
            runInvoked_ = true;
            runActive_ = true;
            runThreadId_ = std::this_thread::get_id();
        }

        const auto startupContext = makeContext(
            *uuidGenerator_, *clock_, StartupTimeout,
            "manager-process-startup");
        const Domain::OperationContext ingressContext{
            Domain::OperationId{nextUuid(*uuidGenerator_)},
            Domain::MonotonicTimePoint::max(),
            {},
            take(Domain::CorrelationId::parse("manager-pipe-ingress"))};
        auto outcome = processHost_->run(startupContext, ingressContext);
        processStopWatcher_->cancel();
        bool requestedShutdown = false;
        {
            const std::lock_guard lock{lifecycleMutex_};
            requestedShutdown = shutdownRequested_;
            shutdownRequested_ = true;
        }
        shutdownServices(true);
        completeRunFrame();
        if (!outcome && requestedShutdown &&
            outcome.error().code == Domain::ErrorCodes::TransportClosed) {
            return Domain::Result<void>::success();
        }
        return outcome;
    } catch (const CompositionFailure& failure) {
        if (processStopWatcher_) {
            processStopWatcher_->cancel();
        }
        {
            const std::lock_guard lock{lifecycleMutex_};
            shutdownRequested_ = true;
        }
        shutdownServices(true);
        completeRunFrame();
        return Domain::Result<void>::failure(failure.error());
    } catch (...) {
        if (processStopWatcher_) {
            processStopWatcher_->cancel();
        }
        {
            const std::lock_guard lock{lifecycleMutex_};
            shutdownRequested_ = true;
        }
        shutdownServices(true);
        completeRunFrame();
        return Domain::Result<void>::failure(internalCompositionError());
    }
}

void ManagerCompositionRoot::Impl::completeRunFrame() noexcept
{
    try {
        {
            const std::lock_guard lock{lifecycleMutex_};
            runActive_ = false;
            runThreadId_ = {};
            shutdownRequested_ = true;
        }
        lifecycleChanged_.notify_all();
    } catch (...) {
        std::terminate();
    }
}

void ManagerCompositionRoot::Impl::shutdownServices(
    const bool activeRunOwner) noexcept
{
    try {
        std::unique_lock lock{lifecycleMutex_};
        if (servicesShutdown_) {
            return;
        }
        if (servicesShutdownClaimed_) {
            lifecycleChanged_.wait(
                lock, [this]() noexcept { return servicesShutdown_; });
            return;
        }
        if (runActive_ && !activeRunOwner) {
            return;
        }
        servicesShutdownClaimed_ = true;
    } catch (...) {
        return;
    }

    try {
        if (dashboardOperationalService_) {
            dashboardOperationalService_->shutdown();
        }
        if (doctorService_) {
            doctorService_->shutdown();
        }
        if (telemetrySource_) {
            telemetrySource_->shutdown();
        }
        if (telemetryService_) {
            telemetryService_->stop();
        }

        if (lmStudioDeployment_) {
            lmStudioDeployment_->shutdown();
        }
        if (lmStudioHostActivator_) {
            lmStudioHostActivator_->shutdown();
        }
        if (lmStudioServeVerifier_) {
            lmStudioServeVerifier_->shutdown();
        }
        if (lmStudioEnvironment_) {
            lmStudioEnvironment_->shutdown();
        }
        if (lmStudioDiscovery_) {
            lmStudioDiscovery_->shutdown();
        }

        if (continuity_) {
            continuity_->shutdown();
        }
        if (projectRepositoryCache_) {
            projectRepositoryCache_->shutdown();
        }
        if (projectRepositoryOpener_) {
            projectRepositoryOpener_->shutdown();
        }
        if (nativeSessionAdapter_) {
            nativeSessionAdapter_->shutdown();
        }
        if (nativeSessionTransport_) {
            nativeSessionTransport_->shutdown();
        }
        if (logicalContinuationQueue_) {
            logicalContinuationQueue_->shutdown();
        }
        if (nativeSessionLedger_) {
            nativeSessionLedger_->shutdown();
        }
        if (agentSessions_) {
            agentSessions_->shutdown();
        }

        if (dashboardOperationalRepository_) {
            dashboardOperationalRepository_->close();
        }
        if (auditRepository_) {
            auditRepository_->close();
        }
        if (agentSessionRepository_) {
            agentSessionRepository_->close();
        }
        if (centralDatabase_) {
            try {
                const auto context = makeContext(
                    *uuidGenerator_, *clock_, ShutdownTimeout,
                    "manager-database-shutdown");
                static_cast<void>(centralDatabase_->close(context));
            } catch (...) {
            }
        }

        if (diagnosticSink_) {
            diagnosticSink_->shutdown();
        }
        if (processSupervisor_) {
            processSupervisor_->shutdown();
        }
        if (runtimeDiagnostics_) {
            runtimeDiagnostics_->shutdown();
        }
        if (managerAuthenticationStore_) {
            managerAuthenticationStore_->shutdown();
        }
        if (dashboardBearerStore_) {
            dashboardBearerStore_->shutdown();
        }
        if (secureStorage_) {
            secureStorage_->shutdown();
        }
    } catch (...) {
    }

    try {
        {
            const std::lock_guard lock{lifecycleMutex_};
            servicesShutdown_ = true;
            servicesShutdownClaimed_ = false;
        }
        lifecycleChanged_.notify_all();
    } catch (...) {
        std::terminate();
    }
}

void ManagerCompositionRoot::Impl::shutdown() noexcept
{
    bool runActive = false;
    bool waitForRun = false;
    try {
        {
            const std::lock_guard lock{lifecycleMutex_};
            shutdownRequested_ = true;
            runActive = runActive_;
            waitForRun = runActive_ &&
                runThreadId_ != std::this_thread::get_id();
        }
        if (processHost_) {
            processHost_->shutdown();
        }
        if (processStopWatcher_) {
            processStopWatcher_->cancel();
        }
        if (waitForRun) {
            std::unique_lock lock{lifecycleMutex_};
            lifecycleChanged_.wait(
                lock, [this]() noexcept { return !runActive_; });
            runActive = false;
        }
        if (!runActive) {
            shutdownServices();
        }
    } catch (...) {
    }
}

Domain::Result<std::unique_ptr<ManagerCompositionRoot>>
ManagerCompositionRoot::create(ManagerCompositionRootOptions options) noexcept
{
    try {
        return Domain::Result<std::unique_ptr<ManagerCompositionRoot>>::success(
            std::unique_ptr<ManagerCompositionRoot>{new ManagerCompositionRoot{
                std::make_unique<Impl>(std::move(options))}});
    } catch (const CompositionFailure& failure) {
        return Domain::Result<std::unique_ptr<ManagerCompositionRoot>>::failure(
            failure.error());
    } catch (...) {
        return Domain::Result<std::unique_ptr<ManagerCompositionRoot>>::failure(
            internalCompositionError());
    }
}

ManagerCompositionRoot::ManagerCompositionRoot(
    std::unique_ptr<Impl> implementation) noexcept
    : implementation_{std::move(implementation)}
{
}

ManagerCompositionRoot::~ManagerCompositionRoot() noexcept = default;

Domain::Result<void> ManagerCompositionRoot::run() noexcept
{
    return implementation_
        ? implementation_->run()
        : Domain::Result<void>::failure(internalCompositionError());
}

void ManagerCompositionRoot::shutdown() noexcept
{
    if (implementation_) {
        implementation_->shutdown();
    }
}

} // namespace ForgeConductor::Hosts::Manager
