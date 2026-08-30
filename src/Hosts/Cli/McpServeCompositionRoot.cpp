#include "McpServeCompositionRoot.h"

#include "ForgeConductor/Application/AgentCatalog.h"
#include "ForgeConductor/Application/AgentSessionService.h"
#include "ForgeConductor/Application/ClientPresenceLifecycle.h"
#include "ForgeConductor/Application/ContinuityCoordinator.h"
#include "ForgeConductor/Application/LegacyContextContinuityService.h"
#include "ForgeConductor/Application/LegacyMemoryService.h"
#include "ForgeConductor/Application/ProjectMemoryRepositoryCache.h"
#include "ForgeConductor/Application/ProjectMemoryService.h"
#include "ForgeConductor/Infrastructure/Windows/BCryptSha256Hasher.h"
#include "ForgeConductor/Infrastructure/Windows/InfrastructureWindows.h"
#include "ForgeConductor/Infrastructure/Windows/SecretRedactor.h"
#include "ForgeConductor/Infrastructure/Windows/SystemClock.h"
#include "ForgeConductor/Infrastructure/Windows/WindowsApplicationPaths.h"
#include "ForgeConductor/Infrastructure/Windows/WindowsAtomicFileStore.h"
#include "ForgeConductor/Infrastructure/Windows/WindowsConfigurationStore.h"
#include "ForgeConductor/Infrastructure/Windows/WindowsContinuityDocumentCodec.h"
#include "ForgeConductor/Infrastructure/Windows/WindowsLegacyContinuityProjectionStore.h"
#include "ForgeConductor/Infrastructure/Windows/WindowsNativeSessionLedger.h"
#include "ForgeConductor/Infrastructure/Windows/WindowsProcessSupervisor.h"
#include "ForgeConductor/Infrastructure/Windows/WindowsProjectWorkspaceAuthority.h"
#include "ForgeConductor/Infrastructure/Windows/WindowsRuntimeDiagnostics.h"
#include "ForgeConductor/Infrastructure/Windows/WindowsUnicodeCanonicalizer.h"
#include "ForgeConductor/Infrastructure/Windows/WindowsUuidGenerator.h"
#include "ForgeConductor/Infrastructure/Windows/WindowsWorkspaceAuthority.h"
#include "ForgeConductor/Mcp/McpClientWorkspaceContext.h"
#include "ForgeConductor/Mcp/McpExecutionServices.h"
#include "ForgeConductor/Mcp/McpInvocationGuard.h"
#include "ForgeConductor/Mcp/McpServer.h"
#include "ForgeConductor/Mcp/McpToolCatalog.h"
#include "ForgeConductor/Mcp/McpToolPackAdapter.h"
#include "ForgeConductor/Mcp/McpToolRouter.h"
#include "ForgeConductor/Mcp/WindowsStdioMcpTransport.h"
#include "ForgeConductor/NativeTools/Windows/WindowsFileSystem.h"
#include "ForgeConductor/NativeTools/Windows/WindowsGitService.h"
#include "ForgeConductor/NativeTools/Windows/WindowsPathGlobService.h"
#include "ForgeConductor/NativeTools/Windows/WindowsPdfService.h"
#include "ForgeConductor/NativeTools/Windows/WindowsShellService.h"
#include "ForgeConductor/NativeTools/Windows/WindowsTextSearchService.h"
#include "ForgeConductor/Persistence/Windows/PersistenceWindows.h"
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
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ForgeConductor::Hosts::Cli {
namespace {

namespace Application = ForgeConductor::Application;
namespace Contracts = ForgeConductor::Contracts;
namespace Domain = ForgeConductor::Domain;
namespace InfrastructureWindows = ForgeConductor::Infrastructure::Windows;
namespace Mcp = ForgeConductor::Mcp;
namespace NativeToolsWindows = ForgeConductor::NativeTools::Windows;
namespace PersistenceWindows = ForgeConductor::Persistence::Windows;
namespace NativeSessionHost = ForgeConductor::SessionHost;

constexpr std::chrono::seconds StartupTimeout{30};
constexpr std::chrono::seconds ShutdownTimeout{10};
constexpr std::string_view ProductVersion{"0.9.0"};
constexpr std::string_view RuntimeName{"forge-conductor-windows-stdio"};
constexpr std::size_t MaximumEnvironmentValueCharacters = 32U * 1024U;

template <typename T>
[[nodiscard]] T take(Domain::Result<T> result)
{
    if (!result) {
        throw std::runtime_error{
            result.error().code + ": " + result.error().message};
    }
    return std::move(result).value();
}

void requireSuccess(Domain::Result<void> result)
{
    if (!result) {
        throw std::runtime_error{
            result.error().code + ": " + result.error().message};
    }
}

[[nodiscard]] Domain::PathText pathText(const std::string_view value)
{
    return take(Domain::PathText::create(value));
}

[[nodiscard]] Domain::Result<std::string> strictWideToUtf8(
    const std::wstring_view value) noexcept
{
    try {
        if (value.empty() ||
            value.size() >
                static_cast<std::size_t>((std::numeric_limits<int>::max)())) {
            return Domain::Result<std::string>::failure(Domain::makeError(
                Domain::ErrorCodes::InvalidRequest,
                "A Windows path could not be converted to UTF-8."));
        }
        const auto inputLength = static_cast<int>(value.size());
        const int required = ::WideCharToMultiByte(
            CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), inputLength,
            nullptr, 0, nullptr, nullptr);
        if (required <= 0) {
            return Domain::Result<std::string>::failure(Domain::makeError(
                Domain::ErrorCodes::InvalidRequest,
                "A Windows path is not valid UTF-16."));
        }
        std::string converted(static_cast<std::size_t>(required), '\0');
        if (::WideCharToMultiByte(
                CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), inputLength,
                converted.data(), required, nullptr, nullptr) != required) {
            return Domain::Result<std::string>::failure(Domain::makeError(
                Domain::ErrorCodes::InternalFailure,
                "A Windows path conversion was incomplete."));
        }
        return Domain::Result<std::string>::success(std::move(converted));
    } catch (...) {
        return Domain::Result<std::string>::failure(Domain::makeError(
            Domain::ErrorCodes::InternalFailure,
            "A Windows path conversion failed safely."));
    }
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
                "A Windows path could not be converted from UTF-8."));
        }
        const auto inputLength = static_cast<int>(value.size());
        const int required = ::MultiByteToWideChar(
            CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), inputLength,
            nullptr, 0);
        if (required <= 0) {
            return Domain::Result<std::wstring>::failure(Domain::makeError(
                Domain::ErrorCodes::InvalidRequest,
                "A Windows path is not valid UTF-8."));
        }
        std::wstring converted(static_cast<std::size_t>(required), L'\0');
        if (::MultiByteToWideChar(
                CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), inputLength,
                converted.data(), required) != required) {
            return Domain::Result<std::wstring>::failure(Domain::makeError(
                Domain::ErrorCodes::InternalFailure,
                "A Windows path conversion was incomplete."));
        }
        return Domain::Result<std::wstring>::success(std::move(converted));
    } catch (...) {
        return Domain::Result<std::wstring>::failure(Domain::makeError(
            Domain::ErrorCodes::InternalFailure,
            "A Windows path conversion failed safely."));
    }
}

[[nodiscard]] std::optional<std::string> environmentValue(
    const wchar_t* const name)
{
    ::SetLastError(ERROR_SUCCESS);
    const DWORD required = ::GetEnvironmentVariableW(name, nullptr, 0U);
    if (required == 0U) {
        if (::GetLastError() == ERROR_ENVVAR_NOT_FOUND) {
            return std::nullopt;
        }
        return std::string{};
    }
    if (required > MaximumEnvironmentValueCharacters) {
        throw std::runtime_error{"An MCP environment value exceeds its bound."};
    }
    std::wstring buffer(static_cast<std::size_t>(required), L'\0');
    ::SetLastError(ERROR_SUCCESS);
    const DWORD written = ::GetEnvironmentVariableW(
        name, buffer.data(), required);
    const DWORD readError = ::GetLastError();
    if (written >= required || (written == 0U && readError != ERROR_SUCCESS)) {
        throw std::runtime_error{"An MCP environment value could not be read."};
    }
    if (written == 0U) {
        return std::string{};
    }
    buffer.resize(static_cast<std::size_t>(written));
    return take(strictWideToUtf8(buffer));
}

[[nodiscard]] Domain::McpRole configuredRole()
{
    const auto configured = environmentValue(L"FORGE_MCP_ROLE");
    if (!configured || configured->empty() || *configured == "primary") {
        return Domain::McpRole::Primary;
    }
    if (*configured == "fallback") {
        return Domain::McpRole::Fallback;
    }
    throw std::runtime_error{
        "invalid_request: FORGE_MCP_ROLE must be primary or fallback."};
}

[[nodiscard]] Domain::PathText currentDirectory()
{
    const DWORD required = ::GetCurrentDirectoryW(0U, nullptr);
    if (required == 0U || required > MaximumEnvironmentValueCharacters) {
        throw std::runtime_error{
            "internal_failure: The startup working directory could not be resolved."};
    }
    std::wstring buffer(static_cast<std::size_t>(required), L'\0');
    const DWORD written = ::GetCurrentDirectoryW(required, buffer.data());
    if (written == 0U || written >= required) {
        throw std::runtime_error{
            "internal_failure: The startup working directory could not be read."};
    }
    buffer.resize(static_cast<std::size_t>(written));
    return pathText(take(strictWideToUtf8(buffer)));
}

[[nodiscard]] Domain::PathText discoverExecutable(const wchar_t* const name)
{
    const DWORD required = ::SearchPathW(nullptr, name, nullptr, 0U, nullptr, nullptr);
    if (required == 0U || required > MaximumEnvironmentValueCharacters) {
        throw std::runtime_error{
            "host_capability_unavailable: A required native executable was not found."};
    }
    std::wstring buffer(static_cast<std::size_t>(required) + 1U, L'\0');
    const DWORD written = ::SearchPathW(
        nullptr, name, nullptr, static_cast<DWORD>(buffer.size()),
        buffer.data(), nullptr);
    if (written == 0U || written >= buffer.size()) {
        throw std::runtime_error{
            "host_capability_unavailable: A required native executable path could not be resolved."};
    }
    buffer.resize(static_cast<std::size_t>(written));
    return pathText(take(strictWideToUtf8(buffer)));
}

[[nodiscard]] Domain::PathText childPath(
    const Domain::PathText& root,
    const std::string_view relative)
{
    if (relative.empty() || relative.starts_with('/') || relative.starts_with('\\')) {
        throw std::runtime_error{"invalid_request: An app-owned relative path is invalid."};
    }
    std::string value = root.value();
    if (!value.ends_with('/') && !value.ends_with('\\')) {
        value.push_back('\\');
    }
    value.append(relative);
    return pathText(value);
}

void ensureDirectory(const Domain::PathText& directory)
{
    const auto wide = take(strictUtf8ToWide(directory.value()));
    std::error_code error;
    std::filesystem::create_directories(std::filesystem::path{wide}, error);
    if (error) {
        throw std::runtime_error{
            "internal_failure: An app-owned directory could not be created."};
    }
    const DWORD attributes = ::GetFileAttributesW(wide.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES ||
        (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0U ||
        (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0U) {
        throw std::runtime_error{
            "path_outside_authority: An app-owned root is not a regular directory."};
    }
}

[[nodiscard]] Domain::ResourceProfile resourceProfile()
{
    MEMORYSTATUSEX status{};
    status.dwLength = sizeof(status);
    return ::GlobalMemoryStatusEx(&status) != FALSE
        ? Domain::selectResourceProfile(status.ullTotalPhys)
        : Domain::ResourceProfile::Standard16GiB;
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

[[nodiscard]] InfrastructureWindows::WindowsWorkspaceAuthorityPolicy authorityPolicy(
    Domain::AuthorityId authorityId,
    Domain::ProjectId projectId,
    Domain::ClientId clientId,
    std::vector<Domain::PathText> roots,
    const Domain::FileAccess intent,
    std::vector<Domain::FileAccess> grants,
    std::vector<Domain::FileAccess> denials,
    const bool shellEnabled)
{
    return InfrastructureWindows::WindowsWorkspaceAuthorityPolicy{
        std::move(authorityId), std::move(projectId), std::move(clientId),
        std::move(roots), intent, std::move(grants), std::move(denials),
        shellEnabled, 1U};
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

} // namespace

class McpServeCompositionRoot::Impl final {
public:
    explicit Impl(McpServeOptions options)
        : clock_{std::make_shared<InfrastructureWindows::SystemClock>()},
          uuidGenerator_{std::make_shared<
              InfrastructureWindows::WindowsUuidGenerator>()},
          hasher_{std::make_shared<
              InfrastructureWindows::BCryptSha256Hasher>()},
          redactor_{std::make_shared<InfrastructureWindows::SecretRedactor>()},
          unicodeCanonicalizer_{std::make_shared<
              InfrastructureWindows::WindowsUnicodeCanonicalizer>()},
          atomicFileStore_{std::make_shared<
              InfrastructureWindows::WindowsAtomicFileStore>()},
          applicationPaths_{std::make_shared<
              InfrastructureWindows::WindowsApplicationPaths>(
              InfrastructureWindows::WindowsApplicationPathsOptions{
                  std::move(options.explicitHome), true})},
          profile_{resourceProfile()},
          budgets_{Domain::budgetsForProfile(profile_)},
          projectMemoryLimits_{Domain::projectMemoryLimitsForProfile(profile_)},
          runtimeDiagnostics_{std::make_shared<
              InfrastructureWindows::WindowsRuntimeDiagnostics>(
              *clock_, budgets_)},
          processSupervisor_{std::make_shared<
              InfrastructureWindows::WindowsProcessSupervisor>(
              budgets_, runtimeDiagnostics_)},
          role_{configuredRole()},
          clientId_{take(Domain::ClientId::parse(
              nextUuid(*uuidGenerator_).value()))},
          deploymentId_{deploymentIdentity()}
    {
        initialize();
    }

    ~Impl() noexcept { shutdown(); }

    [[nodiscard]] int run() noexcept
    {
        if (runInvoked_) {
            std::cerr << "conflict: MCP serve may run only once per process.\n";
            return EXIT_FAILURE;
        }
        runInvoked_ = true;

        try {
            auto startContext = makeContext(
                *uuidGenerator_, *clock_, StartupTimeout,
                "mcp-presence-start");
            requireSuccess(presenceLifecycle_->start(
                Domain::ClientPresenceIdentity{
                    clientId_,
                    role_ == Domain::McpRole::Fallback
                        ? std::string{"fallback"}
                        : std::string{"primary"},
                    deploymentId_,
                    static_cast<std::uint32_t>(::GetCurrentProcessId())},
                currentDirectory(),
                startContext));
            presenceStarted_ = true;

            Domain::OperationContext serveContext{
                Domain::OperationId{nextUuid(*uuidGenerator_)},
                Domain::MonotonicTimePoint::max(),
                {},
                take(Domain::CorrelationId::parse("mcp-stdio-serve"))};
            auto outcome = server_->run(
                *stdioTransport_, role_, deploymentId_, clientId_, serveContext);
            stopPresence();
            if (!outcome) {
                std::cerr << outcome.error().code << ": "
                          << outcome.error().message << '\n';
                return EXIT_FAILURE;
            }
            return EXIT_SUCCESS;
        } catch (const std::exception& error) {
            stopPresence();
            std::cerr << "internal_failure: " << error.what() << '\n';
            return EXIT_FAILURE;
        } catch (...) {
            stopPresence();
            std::cerr << "internal_failure: MCP serve failed safely.\n";
            return EXIT_FAILURE;
        }
    }

private:
    [[nodiscard]] Domain::DeploymentId deploymentIdentity()
    {
        const auto configured = environmentValue(L"FORGE_DEPLOYMENT_ID");
        if (configured && !configured->empty()) {
            return take(Domain::DeploymentId::parse(*configured));
        }
        return take(Domain::DeploymentId::parse(
            nextUuid(*uuidGenerator_).value()));
    }

    void initialize()
    {
        const auto startupContext = makeContext(
            *uuidGenerator_, *clock_, StartupTimeout, "mcp-serve-startup");
        const auto dataRoot = take(applicationPaths_->dataRoot(startupContext));
        ensureDirectory(dataRoot);
        const auto configurationRoot =
            take(applicationPaths_->configurationRoot(startupContext));
        ensureDirectory(configurationRoot);
        const auto projectsRoot = childPath(dataRoot, "projects");
        const auto memoryRoot = childPath(dataRoot, "memory");
        const auto handoffsRoot = childPath(memoryRoot, "handoffs");
        ensureDirectory(projectsRoot);
        ensureDirectory(memoryRoot);
        ensureDirectory(handoffsRoot);

        const auto dataAuthorityId = Domain::AuthorityId{
            nextUuid(*uuidGenerator_)};
        const auto dataProjectId = Domain::ProjectId{
            nextUuid(*uuidGenerator_)};
        dataAuthority_ = std::make_shared<
            InfrastructureWindows::WindowsWorkspaceAuthority>(
            std::vector<InfrastructureWindows::WindowsWorkspaceAuthorityPolicy>{
                authorityPolicy(
                    dataAuthorityId, dataProjectId, clientId_, {dataRoot},
                    Domain::FileAccess::Write,
                    {Domain::FileAccess::Read, Domain::FileAccess::Write,
                     Domain::FileAccess::Create, Domain::FileAccess::Delete},
                    {Domain::FileAccess::Execute}, false)});
        const auto dataScope = take(dataAuthority_->authorityFor(
            dataProjectId, startupContext));

        const auto configPath = childPath(configurationRoot, "config.json");
        configurationStore_ = std::make_unique<
            InfrastructureWindows::WindowsConfigurationStore>(
            *atomicFileStore_,
            authorizePath(
                *dataAuthority_, dataScope, configPath, dataRoot,
                Domain::FileAccess::Read, startupContext),
            authorizePath(
                *dataAuthority_, dataScope, configPath, dataRoot,
                Domain::FileAccess::Write, startupContext),
            authorizePath(
                *dataAuthority_, dataScope, configPath, dataRoot,
                Domain::FileAccess::Create, startupContext),
            authorizePath(
                *dataAuthority_, dataScope,
                pathText(configPath.value() + ".bak"), dataRoot,
                Domain::FileAccess::Read, startupContext));
        configuration_ = take(configurationStore_->load(startupContext));

        const auto registryPath = childPath(projectsRoot, "registry.json");
        projectRegistry_ = std::make_unique<
            PersistenceWindows::WindowsProjectRegistryRepository>(
            applicationPaths_, atomicFileStore_,
            PersistenceWindows::WindowsProjectRegistryStoragePaths{
                authorizePath(
                    *dataAuthority_, dataScope, registryPath, dataRoot,
                    Domain::FileAccess::Read, startupContext),
                authorizePath(
                    *dataAuthority_, dataScope, registryPath, dataRoot,
                    Domain::FileAccess::Write, startupContext),
                authorizePath(
                    *dataAuthority_, dataScope, registryPath, dataRoot,
                    Domain::FileAccess::Create, startupContext),
                authorizePath(
                    *dataAuthority_, dataScope,
                    pathText(registryPath.value() + ".bak"), dataRoot,
                    Domain::FileAccess::Read, startupContext)},
            uuidGenerator_, hasher_, clock_, projectMemoryLimits_);

        const auto initialization = take(projectRegistry_->initialize(
            Domain::InitializeProjectRequest{
                currentDirectory(), std::nullopt, std::nullopt,
                std::nullopt, std::nullopt},
            startupContext));
        defaultProjectId_ = initialization.project.id;
        if (initialization.project.aliases.empty()) {
            throw std::runtime_error{
                "integrity_failure: The startup project has no canonical alias."};
        }
        workspaceAuthority_ = std::make_unique<
            InfrastructureWindows::WindowsProjectWorkspaceAuthority>(
            *projectRegistry_, *uuidGenerator_, clientId_,
            configuration_.shell.enabled);

        const auto gitExecutable = discoverExecutable(L"git.exe");
        const auto powerShellExecutable = discoverExecutable(L"powershell.exe");

        auto centralDatabase = take(PersistenceWindows::WindowsCentralDatabase::open(
            applicationPaths_, runtimeDiagnostics_, clock_, startupContext));
        centralDatabase_ = std::shared_ptr<
            PersistenceWindows::WindowsCentralDatabase>{
            std::move(centralDatabase)};
        agentSessionRepository_ = take(
            PersistenceWindows::WindowsAgentSessionRepository::attach(
                centralDatabase_, clock_));
        legacyMemoryRepository_ = take(
            PersistenceWindows::WindowsLegacyMemoryRepository::attach(
                centralDatabase_, clock_, unicodeCanonicalizer_));
        legacyContinuityRepository_ = take(
            PersistenceWindows::WindowsLegacyContinuityRepository::attach(
                centralDatabase_, clock_, hasher_));
        auditRepository_ = take(
            PersistenceWindows::WindowsAuditRepository::attach(
                centralDatabase_));
        forgeStatusRepository_ = take(
            PersistenceWindows::WindowsForgeStatusRepository::attach(
                centralDatabase_));
        presenceRepository_ = take(
            PersistenceWindows::WindowsClientPresenceRepository::attach(
                centralDatabase_));

        fileSystem_ = std::make_shared<NativeToolsWindows::WindowsFileSystem>(
            atomicFileStore_);
        pathGlob_ = std::make_unique<
            NativeToolsWindows::WindowsPathGlobService>();
        textSearch_ = std::make_unique<
            NativeToolsWindows::WindowsTextSearchService>();
        pdf_ = std::make_unique<NativeToolsWindows::WindowsPdfService>(
            *atomicFileStore_);
        git_ = std::make_unique<NativeToolsWindows::WindowsGitService>(
            gitExecutable, processSupervisor_);
        shell_ = std::make_unique<NativeToolsWindows::WindowsShellService>(
            powerShellExecutable, processSupervisor_);

        projectArtifactStore_ = std::make_shared<
            PersistenceWindows::WindowsProjectMemoryArtifactStore>(
            applicationPaths_, uuidGenerator_);
        projectRepositoryOpener_ = std::make_shared<
            PersistenceWindows::WindowsProjectMemoryRepositoryOpener>(
            applicationPaths_, projectArtifactStore_, runtimeDiagnostics_,
            redactor_, hasher_, uuidGenerator_, clock_,
            PersistenceWindows::WindowsProjectMemoryRepositoryOptions{
                {}, projectMemoryLimits_});
        projectRepositoryCache_ = std::make_unique<
            Application::ProjectMemoryRepositoryCache>(
            projectRepositoryOpener_, budgets_.openProjectRepositoriesMaximum);
        projectMemory_ = std::make_unique<Application::ProjectMemoryService>(
            *projectRegistry_, *projectRepositoryCache_, *redactor_,
            projectMemoryLimits_);

        agentCatalog_ = take(Application::AgentCatalog::create(
            clock_, std::span<const Application::AgentDefinitionDocument>{},
            startupContext));
        reportInspector_ =
            InfrastructureWindows::createWindowsAgentCompletionReportInspector(
                *clock_);
        if (!reportInspector_) {
            throw std::runtime_error{
                "internal_failure: The agent report inspector was not created."};
        }
        agentSessions_ = std::make_unique<Application::AgentSessionService>(
            *agentCatalog_, *agentSessionRepository_, *reportInspector_,
            *workspaceAuthority_, *clock_, *uuidGenerator_,
            configuration_.sessions.idleTimeToLive);
        legacyMemory_ = std::make_unique<Application::LegacyMemoryService>(
            *legacyMemoryRepository_, unicodeCanonicalizer_);

        const auto projectionAuthorityId = Domain::AuthorityId{
            nextUuid(*uuidGenerator_)};
        const auto projectionProjectId = Domain::ProjectId{
            nextUuid(*uuidGenerator_)};
        projectionAuthority_ = std::make_shared<
            InfrastructureWindows::WindowsWorkspaceAuthority>(
            std::vector<InfrastructureWindows::WindowsWorkspaceAuthorityPolicy>{
                authorityPolicy(
                    projectionAuthorityId, projectionProjectId, clientId_,
                    {memoryRoot}, Domain::FileAccess::Write,
                    {Domain::FileAccess::Read, Domain::FileAccess::Write,
                     Domain::FileAccess::Create, Domain::FileAccess::Delete},
                    {Domain::FileAccess::Execute}, false)});
        const auto projectionScope = take(projectionAuthority_->authorityFor(
            projectionProjectId, startupContext));
        legacyProjectionStore_ = take(
            InfrastructureWindows::WindowsLegacyContinuityProjectionStore::create(
                memoryRoot, handoffsRoot, projectionScope,
                projectionAuthority_, atomicFileStore_, fileSystem_, clock_));
        legacyContinuity_ = std::make_unique<
            Application::LegacyContextContinuityService>(
            *legacyContinuityRepository_, *legacyProjectionStore_,
            *agentSessionRepository_, *clock_, *uuidGenerator_);

        continuityCodec_ = std::make_unique<
            InfrastructureWindows::WindowsContinuityDocumentCodec>(
            hasher_, clock_);
        const auto ledgerPath = childPath(dataRoot, "native-session-ledger.json");
        nativeSessionLedger_ = std::make_unique<
            InfrastructureWindows::WindowsNativeSessionLedger>(
            *atomicFileStore_, *hasher_,
            authorizePath(
                *dataAuthority_, dataScope, ledgerPath, dataRoot,
                Domain::FileAccess::Read, startupContext),
            authorizePath(
                *dataAuthority_, dataScope, ledgerPath, dataRoot,
                Domain::FileAccess::Write, startupContext),
            authorizePath(
                *dataAuthority_, dataScope, ledgerPath, dataRoot,
                Domain::FileAccess::Create, startupContext),
            authorizePath(
                *dataAuthority_, dataScope,
                pathText(ledgerPath.value() + ".bak"), dataRoot,
                Domain::FileAccess::Read, startupContext));
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
            *nativeSessionLedger_, *nativeSessionTransport_,
            *continuityCodec_, *uuidGenerator_, *clock_);
        continuity_ = std::make_unique<Application::ContinuityCoordinator>(
            *projectRegistry_, *projectRepositoryCache_,
            *nativeSessionAdapter_, *clock_);

        toolCatalog_ = take(Mcp::McpToolCatalog::create());
        clientWorkspaceContext_ = std::make_unique<
            Mcp::McpClientWorkspaceContext>(
            *projectRegistry_, *workspaceAuthority_, *clock_);
        invocationGuard_ = take(Mcp::McpInvocationGuard::create(
            *legacyContinuity_, *hasher_, *clock_));
        toolPack_ = take(Mcp::McpToolPackAdapter::create(
            Mcp::McpToolPackDependencies{
                *toolCatalog_,
                *applicationPaths_,
                *agentCatalog_,
                *agentSessions_,
                *reportInspector_,
                *legacyContinuity_,
                *clientWorkspaceContext_,
                *workspaceAuthority_,
                *fileSystem_,
                *fileSystem_,
                *pathGlob_,
                *git_,
                *legacyMemory_,
                *pdf_,
                *textSearch_,
                *shell_,
                *projectRegistry_,
                *projectMemory_,
                *continuity_,
                *continuityCodec_,
                *invocationGuard_,
                *forgeStatusRepository_,
                *clock_,
                *uuidGenerator_,
                projectMemoryLimits_,
                configuration_.shell.defaultTimeout,
                powerShellExecutable,
                std::string{ProductVersion},
                std::string{RuntimeName},
                static_cast<std::uint32_t>(::GetCurrentProcessId())}));
        toolAuthorizer_ = std::make_unique<Mcp::McpToolAuthorizer>(*clock_);
        const std::array<Contracts::IToolHandler*, 1U> handlers{toolPack_.get()};
        toolRouter_ = take(Mcp::McpToolRouter::create(
            *toolCatalog_, handlers, *toolAuthorizer_, *invocationGuard_,
            *auditRepository_, *hasher_, *clock_));
        executionContextResolver_ = std::make_unique<
            Mcp::McpExecutionContextResolver>(
            *workspaceAuthority_, defaultProjectId_, *clock_,
            clientWorkspaceContext_.get());
        server_ = std::make_unique<Mcp::McpServer>(
            *toolCatalog_, *toolRouter_, *executionContextResolver_,
            *uuidGenerator_, *clock_);
        stdioTransport_ = take(Mcp::WindowsStdioMcpTransport::create());
        presenceLifecycle_ = std::make_unique<
            Application::ClientPresenceLifecycle>(
            presenceRepository_, clock_, uuidGenerator_, runtimeDiagnostics_);
    }

    void stopPresence() noexcept
    {
        if (!presenceStarted_ || !presenceLifecycle_) {
            return;
        }
        try {
            const auto context = makeContext(
                *uuidGenerator_, *clock_, ShutdownTimeout,
                "mcp-presence-stop");
            const auto stopped = presenceLifecycle_->stop(context);
            if (!stopped) {
                std::cerr << stopped.error().code << ": "
                          << stopped.error().message << '\n';
            }
        } catch (...) {
        }
        presenceStarted_ = false;
    }

    void shutdown() noexcept
    {
        if (shutdown_) {
            return;
        }
        shutdown_ = true;
        stopPresence();
        presenceLifecycle_.reset();

        if (server_) {
            server_->shutdown();
        }
        if (stdioTransport_) {
            stdioTransport_->shutdown();
        }
        server_.reset();
        stdioTransport_.reset();
        if (toolRouter_) {
            toolRouter_->shutdown();
        }
        toolRouter_.reset();
        toolPack_.reset();
        executionContextResolver_.reset();
        toolAuthorizer_.reset();
        if (invocationGuard_) {
            invocationGuard_->shutdown();
        }
        invocationGuard_.reset();
        if (clientWorkspaceContext_) {
            clientWorkspaceContext_->shutdown();
        }
        clientWorkspaceContext_.reset();
        toolCatalog_.reset();

        if (continuity_) {
            continuity_->shutdown();
        }
        continuity_.reset();
        if (nativeSessionAdapter_) {
            nativeSessionAdapter_->shutdown();
        }
        nativeSessionAdapter_.reset();
        if (nativeSessionTransport_) {
            nativeSessionTransport_->shutdown();
        }
        nativeSessionTransport_.reset();
        if (logicalContinuationQueue_) {
            logicalContinuationQueue_->shutdown();
        }
        logicalContinuationQueue_.reset();
        if (nativeSessionLedger_) {
            nativeSessionLedger_->shutdown();
        }
        nativeSessionLedger_.reset();
        continuityCodec_.reset();

        if (legacyContinuity_) {
            legacyContinuity_->shutdown();
        }
        legacyContinuity_.reset();
        if (legacyProjectionStore_) {
            legacyProjectionStore_->close();
        }
        legacyProjectionStore_.reset();
        projectionAuthority_.reset();

        if (agentSessions_) {
            agentSessions_->shutdown();
        }
        agentSessions_.reset();
        reportInspector_.reset();
        agentCatalog_.reset();
        if (legacyMemory_) {
            legacyMemory_->shutdown();
        }
        legacyMemory_.reset();

        if (projectMemory_) {
            projectMemory_->shutdown();
        }
        projectMemory_.reset();
        if (projectRepositoryCache_) {
            projectRepositoryCache_->shutdown();
        }
        projectRepositoryCache_.reset();
        if (projectRepositoryOpener_) {
            projectRepositoryOpener_->shutdown();
        }
        projectRepositoryOpener_.reset();
        projectArtifactStore_.reset();

        shell_.reset();
        git_.reset();
        pdf_.reset();
        textSearch_.reset();
        pathGlob_.reset();
        fileSystem_.reset();
        if (processSupervisor_) {
            processSupervisor_->shutdown();
        }

        if (presenceRepository_) {
            presenceRepository_->close();
        }
        if (forgeStatusRepository_) {
            forgeStatusRepository_->close();
        }
        if (auditRepository_) {
            auditRepository_->close();
        }
        if (legacyContinuityRepository_) {
            legacyContinuityRepository_->close();
        }
        if (legacyMemoryRepository_) {
            legacyMemoryRepository_->close();
        }
        if (agentSessionRepository_) {
            agentSessionRepository_->close();
        }
        presenceRepository_.reset();
        forgeStatusRepository_.reset();
        auditRepository_.reset();
        legacyContinuityRepository_.reset();
        legacyMemoryRepository_.reset();
        agentSessionRepository_.reset();

        workspaceAuthority_.reset();
        projectRegistry_.reset();
        if (configurationStore_) {
            configurationStore_->shutdown();
        }
        configurationStore_.reset();
        dataAuthority_.reset();

        if (centralDatabase_) {
            try {
                const auto context = makeContext(
                    *uuidGenerator_, *clock_, ShutdownTimeout,
                    "mcp-central-database-close");
                static_cast<void>(centralDatabase_->close(context));
            } catch (...) {
            }
        }
        centralDatabase_.reset();
        processSupervisor_.reset();
        if (runtimeDiagnostics_) {
            runtimeDiagnostics_->shutdown();
        }
        runtimeDiagnostics_.reset();
        applicationPaths_.reset();
        atomicFileStore_.reset();
        unicodeCanonicalizer_.reset();
        redactor_.reset();
        hasher_.reset();
        uuidGenerator_.reset();
        clock_.reset();
    }

    std::shared_ptr<InfrastructureWindows::SystemClock> clock_;
    std::shared_ptr<InfrastructureWindows::WindowsUuidGenerator> uuidGenerator_;
    std::shared_ptr<InfrastructureWindows::BCryptSha256Hasher> hasher_;
    std::shared_ptr<InfrastructureWindows::SecretRedactor> redactor_;
    std::shared_ptr<InfrastructureWindows::WindowsUnicodeCanonicalizer>
        unicodeCanonicalizer_;
    std::shared_ptr<InfrastructureWindows::WindowsAtomicFileStore>
        atomicFileStore_;
    std::shared_ptr<InfrastructureWindows::WindowsApplicationPaths>
        applicationPaths_;
    Domain::ResourceProfile profile_;
    Domain::ResourceBudgets budgets_;
    Domain::ProjectMemoryLimits projectMemoryLimits_;
    std::shared_ptr<InfrastructureWindows::WindowsRuntimeDiagnostics>
        runtimeDiagnostics_;
    std::shared_ptr<InfrastructureWindows::WindowsProcessSupervisor>
        processSupervisor_;
    Domain::McpRole role_;
    Domain::ClientId clientId_;
    Domain::DeploymentId deploymentId_;
    Domain::AppConfig configuration_;
    Domain::ProjectId defaultProjectId_{Domain::Uuid::parse(
        "00000000-0000-4000-8000-000000000000").value()};

    std::shared_ptr<InfrastructureWindows::WindowsWorkspaceAuthority>
        dataAuthority_;
    std::unique_ptr<InfrastructureWindows::WindowsProjectWorkspaceAuthority>
        workspaceAuthority_;
    std::shared_ptr<InfrastructureWindows::WindowsWorkspaceAuthority>
        projectionAuthority_;
    std::unique_ptr<InfrastructureWindows::WindowsConfigurationStore>
        configurationStore_;
    std::unique_ptr<PersistenceWindows::WindowsProjectRegistryRepository>
        projectRegistry_;
    std::shared_ptr<PersistenceWindows::WindowsCentralDatabase> centralDatabase_;
    std::shared_ptr<PersistenceWindows::WindowsAgentSessionRepository>
        agentSessionRepository_;
    std::shared_ptr<PersistenceWindows::WindowsLegacyMemoryRepository>
        legacyMemoryRepository_;
    std::shared_ptr<PersistenceWindows::WindowsLegacyContinuityRepository>
        legacyContinuityRepository_;
    std::shared_ptr<PersistenceWindows::WindowsAuditRepository> auditRepository_;
    std::shared_ptr<PersistenceWindows::WindowsForgeStatusRepository>
        forgeStatusRepository_;
    std::shared_ptr<PersistenceWindows::WindowsClientPresenceRepository>
        presenceRepository_;

    std::shared_ptr<NativeToolsWindows::WindowsFileSystem> fileSystem_;
    std::unique_ptr<NativeToolsWindows::WindowsPathGlobService> pathGlob_;
    std::unique_ptr<NativeToolsWindows::WindowsTextSearchService> textSearch_;
    std::unique_ptr<NativeToolsWindows::WindowsPdfService> pdf_;
    std::unique_ptr<NativeToolsWindows::WindowsGitService> git_;
    std::unique_ptr<NativeToolsWindows::WindowsShellService> shell_;

    std::shared_ptr<PersistenceWindows::WindowsProjectMemoryArtifactStore>
        projectArtifactStore_;
    std::shared_ptr<PersistenceWindows::WindowsProjectMemoryRepositoryOpener>
        projectRepositoryOpener_;
    std::unique_ptr<Application::ProjectMemoryRepositoryCache>
        projectRepositoryCache_;
    std::unique_ptr<Application::ProjectMemoryService> projectMemory_;
    std::unique_ptr<Application::AgentCatalog> agentCatalog_;
    std::unique_ptr<Contracts::IAgentCompletionReportInspector> reportInspector_;
    std::unique_ptr<Application::AgentSessionService> agentSessions_;
    std::unique_ptr<Application::LegacyMemoryService> legacyMemory_;
    std::shared_ptr<
        InfrastructureWindows::WindowsLegacyContinuityProjectionStore>
        legacyProjectionStore_;
    std::unique_ptr<Application::LegacyContextContinuityService>
        legacyContinuity_;
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
    std::unique_ptr<Mcp::McpClientWorkspaceContext> clientWorkspaceContext_;
    std::unique_ptr<Mcp::McpInvocationGuard> invocationGuard_;
    std::unique_ptr<Mcp::McpToolPackAdapter> toolPack_;
    std::unique_ptr<Mcp::McpToolAuthorizer> toolAuthorizer_;
    std::unique_ptr<Mcp::McpToolRouter> toolRouter_;
    std::unique_ptr<Mcp::McpExecutionContextResolver>
        executionContextResolver_;
    std::unique_ptr<Mcp::McpServer> server_;
    std::unique_ptr<Mcp::WindowsStdioMcpTransport> stdioTransport_;
    std::unique_ptr<Application::ClientPresenceLifecycle> presenceLifecycle_;

    bool presenceStarted_{};
    bool runInvoked_{};
    bool shutdown_{};
};

McpServeCompositionRoot::McpServeCompositionRoot(McpServeOptions options)
    : implementation_{std::make_unique<Impl>(std::move(options))}
{
}

McpServeCompositionRoot::~McpServeCompositionRoot() noexcept = default;

int McpServeCompositionRoot::run() noexcept
{
    return implementation_ ? implementation_->run() : EXIT_FAILURE;
}

} // namespace ForgeConductor::Hosts::Cli
