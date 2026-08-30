cmake_minimum_required(VERSION 3.28)
cmake_policy(SET CMP0091 NEW)

project(
    ForgeConductorWindows
    VERSION 0.9.0
    LANGUAGES CXX RC
)

if(NOT WIN32 OR NOT MSVC)
    message(FATAL_ERROR "Forge Conductor requires native MSVC on Windows 11.")
endif()
if(NOT CMAKE_GENERATOR STREQUAL "Visual Studio 17 2022")
    message(FATAL_ERROR "Forge Conductor requires the Visual Studio 17 2022 generator.")
endif()
if(NOT CMAKE_GENERATOR_TOOLSET STREQUAL "v143")
    message(FATAL_ERROR "Forge Conductor requires the v143 platform toolset.")
endif()
if(NOT CMAKE_VS_WINDOWS_TARGET_PLATFORM_VERSION STREQUAL "10.0.26100.0")
    message(FATAL_ERROR "Forge Conductor requires Windows SDK 10.0.26100.0.")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
set(CMAKE_CONFIGURATION_TYPES "Debug;Release" CACHE STRING "Supported configurations" FORCE)

foreach(_configuration IN ITEMS Debug Release)
    string(TOUPPER "${_configuration}" _configuration_upper)
    set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY_${_configuration_upper}
        "${CMAKE_BINARY_DIR}/lib/${_configuration}")
    set(CMAKE_LIBRARY_OUTPUT_DIRECTORY_${_configuration_upper}
        "${CMAKE_BINARY_DIR}/bin/${_configuration}")
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_${_configuration_upper}
        "${CMAKE_BINARY_DIR}/bin/${_configuration}")
endforeach()

option(FORGE_ENABLE_ANALYZE "Enable MSVC static analysis" OFF)

find_package(nlohmann_json CONFIG REQUIRED)
include(cmake/ForgeForsettiExternal.cmake)
include(CTest)

function(forge_configure_standard_target target_name)
    target_compile_features(${target_name} PRIVATE cxx_std_20)
    target_compile_options(${target_name} PRIVATE
        /W4
        /WX
        /permissive-
        /Zc:__cplusplus
        /EHsc
    )
    if(FORGE_ENABLE_ANALYZE)
        target_compile_options(${target_name} PRIVATE /analyze)
    endif()
endfunction()

function(forge_configure_native_target target_name)
    forge_configure_standard_target(${target_name})
    target_compile_definitions(${target_name} PRIVATE
        UNICODE
        _UNICODE
        WIN32_LEAN_AND_MEAN
        NOMINMAX
    )
endfunction()

function(forge_add_layer target_name alias_name)
    if(TARGET ${target_name})
        get_target_property(_forge_layer_type ${target_name} TYPE)
    else()
        add_library(${target_name} INTERFACE)
        set(_forge_layer_type INTERFACE_LIBRARY)
    endif()
    add_library(${alias_name} ALIAS ${target_name})
    if(_forge_layer_type STREQUAL "INTERFACE_LIBRARY")
        set(_forge_layer_visibility INTERFACE)
    else()
        set(_forge_layer_visibility PUBLIC)
    endif()
    target_compile_features(${target_name} ${_forge_layer_visibility} cxx_std_20)
    if(ARGN)
        target_link_libraries(${target_name} ${_forge_layer_visibility} ${ARGN})
    endif()
endfunction()

set(FORGE_DOMAIN_SOURCES
    src/Domain/AgentModels.cpp
    src/Domain/ClientPresenceModels.cpp
    src/Domain/ConfigurationModels.cpp
    src/Domain/ContinuityModels.cpp
    src/Domain/DeploymentModels.cpp
    src/Domain/DiagnosticsModels.cpp
    src/Domain/EnvironmentModels.cpp
    src/Domain/FileSystemModels.cpp
    src/Domain/ForgeStatusModels.cpp
    src/Domain/GraphicsModels.cpp
    src/Domain/Identifiers.cpp
    src/Domain/LegacyContinuityModels.cpp
    src/Domain/LegacyMemoryModels.cpp
    src/Domain/ManagerModels.cpp
    src/Domain/ManagerStartupModels.cpp
    src/Domain/ProcessModels.cpp
    src/Domain/ProjectMemoryModels.cpp
    src/Domain/ResourcePolicy.cpp
    src/Domain/SessionHostModels.cpp
    src/Domain/TelemetryModels.cpp
    src/Domain/ToolModels.cpp
)
add_library(ForgeConductor.Domain STATIC ${FORGE_DOMAIN_SOURCES})

# One-way application layer graph. These target names remain stable as concrete
# implementations replace the remaining INTERFACE placeholders.
forge_add_layer(ForgeConductor.Domain ForgeConductor::Domain)
forge_configure_standard_target(ForgeConductor.Domain)
target_include_directories(ForgeConductor.Domain PUBLIC
    "${PROJECT_SOURCE_DIR}/include")
forge_add_layer(ForgeConductor.Contracts ForgeConductor::Contracts
    ForgeConductor::Domain)
target_include_directories(ForgeConductor.Contracts INTERFACE
    "${PROJECT_SOURCE_DIR}/include")

add_library(ForgeConductor.Telemetry.Windows STATIC
    src/Telemetry/Windows/WindowsRamMetricsCollector.cpp)
forge_add_layer(
    ForgeConductor.Telemetry.Windows
    ForgeConductor::Telemetry.Windows
    ForgeConductor::Contracts)
forge_configure_native_target(ForgeConductor.Telemetry.Windows)
target_include_directories(ForgeConductor.Telemetry.Windows
    PUBLIC "${PROJECT_SOURCE_DIR}/include"
    PRIVATE "${PROJECT_SOURCE_DIR}/src")
target_compile_definitions(ForgeConductor.Telemetry.Windows PRIVATE
    WINVER=0x0A00
    _WIN32_WINNT=0x0A00)
target_link_libraries(ForgeConductor.Telemetry.Windows PRIVATE
    psapi)

set(FORGE_APPLICATION_SOURCES
    src/Application/AgentCatalog.cpp
    src/Application/AgentSessionService.cpp
    src/Application/ClientPresenceLifecycle.cpp
    src/Application/ContinuityAutomation.cpp
    src/Application/ContinuityCoordinator.cpp
    src/Application/DashboardConnectionApplication.cpp
    src/Application/DashboardConnectionApplicationFactory.cpp
    src/Application/DashboardOperationalService.cpp
    src/Application/DashboardTelemetrySource.cpp
    src/Application/LegacyContextContinuityService.cpp
    src/Application/LegacyMemoryService.cpp
    src/Application/ManagerController.cpp
    src/Application/ProjectMemoryRepositoryCache.cpp
    src/Application/ProjectMemoryService.cpp
)
add_library(ForgeConductor.Application STATIC ${FORGE_APPLICATION_SOURCES})
forge_add_layer(ForgeConductor.Application ForgeConductor::Application
    ForgeConductor::Contracts)
forge_configure_standard_target(ForgeConductor.Application)
target_include_directories(ForgeConductor.Application PUBLIC
    "${PROJECT_SOURCE_DIR}/include")

add_library(ForgeConductor.Manager.Protocol STATIC
    src/Manager/ManagerDeadlineMapper.cpp
    src/Manager/ManagerRequestDispatcher.cpp
    src/Manager/ManagerProtocolCodec.cpp)
forge_add_layer(ForgeConductor.Manager.Protocol ForgeConductor::Manager.Protocol
    ForgeConductor::Contracts)
forge_configure_standard_target(ForgeConductor.Manager.Protocol)
target_include_directories(ForgeConductor.Manager.Protocol PUBLIC
    "${PROJECT_SOURCE_DIR}/include")
target_link_libraries(ForgeConductor.Manager.Protocol PRIVATE
    nlohmann_json::nlohmann_json)

add_library(ForgeConductor.Manager.Startup STATIC
    src/Manager/ManagerStartupTaskPolicy.cpp)
forge_add_layer(
    ForgeConductor.Manager.Startup
    ForgeConductor::Manager.Startup
    ForgeConductor::Contracts)
forge_configure_standard_target(ForgeConductor.Manager.Startup)
target_include_directories(ForgeConductor.Manager.Startup PUBLIC
    "${PROJECT_SOURCE_DIR}/include")

add_library(ForgeConductor.Manager.Host STATIC
    src/Hosts/Manager/ManagerControllerClient.cpp
    src/Hosts/Manager/ManagerControllerClient.h
    src/Hosts/Manager/ManagerMaintenanceWorker.cpp
    src/Hosts/Manager/ManagerMaintenanceWorker.h
    src/Hosts/Manager/ManagerProcessHost.cpp
    src/Hosts/Manager/ManagerProcessHost.h
    src/Hosts/Manager/ManagerProcessRestartSignal.cpp
    src/Hosts/Manager/ManagerProcessRestartSignal.h
    src/Hosts/Manager/ManagerProcessStopSignal.cpp
    src/Hosts/Manager/ManagerProcessStopSignal.h
    src/Hosts/Manager/ManagerProcessStopWatcher.cpp
    src/Hosts/Manager/ManagerProcessStopWatcher.h
    src/Hosts/Manager/ManagerProcessWorkerGroup.cpp
    src/Hosts/Manager/ManagerProcessWorkerGroup.h
    src/Hosts/Manager/ManagerTransitionWorker.cpp
    src/Hosts/Manager/ManagerTransitionWorker.h
    src/Hosts/Manager/ManagerWatchdogPolicy.cpp
    src/Hosts/Manager/ManagerWatchdogPolicy.h)
add_library(ForgeConductor::Manager.Host ALIAS ForgeConductor.Manager.Host)
forge_configure_standard_target(ForgeConductor.Manager.Host)
target_include_directories(ForgeConductor.Manager.Host PUBLIC
    "${PROJECT_SOURCE_DIR}/src/Hosts/Manager")
target_link_libraries(ForgeConductor.Manager.Host PUBLIC
    ForgeConductor::Manager.Protocol)

add_library(ForgeConductor.Dashboard.Protocol STATIC
    src/Dashboard/DashboardApplicationErrorMapper.cpp
    src/Dashboard/DashboardApplicationJsonCodec.cpp
    src/Dashboard/DashboardHttpParser.cpp
    src/Dashboard/DashboardHttpParserSession.cpp
    src/Dashboard/DashboardHttpResponse.cpp
    src/Dashboard/DashboardManagerJsonCodec.cpp
    src/Dashboard/DashboardRequestPolicy.cpp
    src/Dashboard/DashboardRequestPlanner.cpp
    src/Dashboard/DashboardResponseComposer.cpp
    src/Dashboard/DashboardRouteCatalog.cpp
    src/Dashboard/DashboardSessionCloseRequestDecoder.cpp
    src/Dashboard/DashboardSseBroadcaster.cpp
    src/Dashboard/DashboardSseFrameEncoder.cpp
    src/Dashboard/DashboardStaticAssetStore.cpp
    src/Dashboard/DashboardStaticResourcePath.cpp
    src/Dashboard/DashboardStreamQueryDecoder.cpp
    src/Dashboard/DashboardTelemetryJsonCodec.cpp)
forge_add_layer(
    ForgeConductor.Dashboard.Protocol
    ForgeConductor::Dashboard.Protocol
    ForgeConductor::Contracts)
forge_configure_standard_target(ForgeConductor.Dashboard.Protocol)
target_include_directories(ForgeConductor.Dashboard.Protocol PUBLIC
    "${PROJECT_SOURCE_DIR}/include")
target_link_libraries(ForgeConductor.Dashboard.Protocol PRIVATE
    nlohmann_json::nlohmann_json)
target_link_libraries(ForgeConductor.Application PUBLIC
    ForgeConductor::Dashboard.Protocol)

add_library(ForgeConductor.Dashboard.Windows STATIC
    src/Infrastructure/Windows/Detail/DashboardAdmissionController.cpp
    src/Infrastructure/Windows/Detail/DashboardAcceptSlot.cpp
    src/Infrastructure/Windows/Detail/DashboardAcceptSlotSet.cpp
    src/Infrastructure/Windows/Detail/DashboardAcceptedConnectionHandoff.cpp
    src/Infrastructure/Windows/Detail/DashboardConnectionEventBridge.cpp
    src/Infrastructure/Windows/Detail/DashboardConnectionRegistry.cpp
    src/Infrastructure/Windows/Detail/DashboardConnectionResponseCatalog.cpp
    src/Infrastructure/Windows/Detail/DashboardConnectionRuntimeServices.cpp
    src/Infrastructure/Windows/Detail/DashboardFixedIocpKeyAuthority.cpp
    src/Infrastructure/Windows/Detail/DashboardFixedIocpKeyAuthority.h
    src/Infrastructure/Windows/Detail/DashboardConnectionSocket.cpp
    src/Infrastructure/Windows/Detail/DashboardConnectionState.cpp
    src/Infrastructure/Windows/Detail/DashboardDeadlineIocpBridge.cpp
    src/Infrastructure/Windows/Detail/DashboardDeadlineNotificationMailbox.cpp
    src/Infrastructure/Windows/Detail/DashboardHandlerOperations.cpp
    src/Infrastructure/Windows/Detail/DashboardIoCompletionPort.cpp
    src/Infrastructure/Windows/Detail/DashboardIocpCompletionRouter.cpp
    src/Infrastructure/Windows/Detail/DashboardIocpWorkerKernel.cpp
    src/Infrastructure/Windows/Detail/DashboardListenerCompletionKeyLease.cpp
    src/Infrastructure/Windows/Detail/DashboardListenerCompletionKeyLease.h
    src/Infrastructure/Windows/Detail/DashboardListenerGeneration.cpp
    src/Infrastructure/Windows/Detail/DashboardListenerGenerationCoordinator.cpp
    src/Infrastructure/Windows/Detail/DashboardListeningSocket.cpp
    src/Infrastructure/Windows/Detail/DashboardLoopbackEndpoint.cpp
    src/Infrastructure/Windows/Detail/ManagerDashboardOperationalState.cpp
    src/Infrastructure/Windows/Detail/ManagerDashboardOperationalState.h
    src/Infrastructure/Windows/Detail/DashboardOverloadResponderSet.cpp
    src/Infrastructure/Windows/Detail/DashboardShutdownDrain.cpp
    src/Infrastructure/Windows/Detail/DashboardWinsockExtensions.cpp
    src/Infrastructure/Windows/Detail/DashboardWinsockRuntime.cpp
    src/Infrastructure/Windows/Detail/WindowsDashboardListenerGenerationFactory.cpp
    src/Infrastructure/Windows/Detail/WindowsDashboardListenerGenerationFactory.h
    src/Infrastructure/Windows/Detail/WindowsDashboardRuntime.cpp
    src/Infrastructure/Windows/Detail/WindowsDashboardRuntime.h
    src/Infrastructure/Windows/WindowsDashboardStaticAssetBundle.cpp
    src/Infrastructure/Windows/WindowsDashboardDeadlineScheduler.cpp
    src/Infrastructure/Windows/WindowsDashboardHandlerExecutor.cpp)
forge_add_layer(
    ForgeConductor.Dashboard.Windows
    ForgeConductor::Dashboard.Windows
    ForgeConductor::Dashboard.Protocol)
forge_configure_native_target(ForgeConductor.Dashboard.Windows)
target_include_directories(ForgeConductor.Dashboard.Windows
    PUBLIC "${PROJECT_SOURCE_DIR}/include"
    PRIVATE "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(ForgeConductor.Dashboard.Windows PRIVATE ws2_32)

add_library(ForgeConductor.Manager.Windows STATIC
    src/Hosts/Manager/WindowsManagerRuntime.cpp
    src/Hosts/Manager/WindowsManagerRuntime.h)
add_library(ForgeConductor::Manager.Windows ALIAS
    ForgeConductor.Manager.Windows)
forge_configure_native_target(ForgeConductor.Manager.Windows)
target_include_directories(ForgeConductor.Manager.Windows
    PUBLIC
        "${PROJECT_SOURCE_DIR}/include"
        "${PROJECT_SOURCE_DIR}/src/Hosts/Manager"
    PRIVATE "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(ForgeConductor.Manager.Windows PUBLIC
    ForgeConductor::Manager.Host
    ForgeConductor::Dashboard.Windows)

set(FORGE_INFRASTRUCTURE_WINDOWS_SOURCES
    src/Infrastructure/Windows/LMStudioConfigurationCodec.cpp
    src/Infrastructure/Windows/BCryptSha256Hasher.cpp
    src/Infrastructure/Windows/DeadlineScheduler.cpp
    src/Infrastructure/Windows/DpapiSecureStorage.cpp
    src/Infrastructure/Windows/SecretRedactor.cpp
    src/Infrastructure/Windows/SystemClock.cpp
    src/Infrastructure/Windows/WindowsApplicationPaths.cpp
    src/Infrastructure/Windows/WindowsAtomicFileStore.cpp
    src/Infrastructure/Windows/WindowsAgentCompletionReportInspector.cpp
    src/Infrastructure/Windows/WindowsConfigurationStore.cpp
    src/Infrastructure/Windows/WindowsContinuityDocumentCodec.cpp
    src/Infrastructure/Windows/WindowsCurrentUserIdentity.cpp
    src/Infrastructure/Windows/WindowsDashboardBearerToken.cpp
    src/Infrastructure/Windows/WindowsDashboardBrowserLauncher.cpp
    src/Infrastructure/Windows/WindowsDashboardUriActivationCommand.cpp
    src/Infrastructure/Windows/WindowsDiagnosticLogTailReader.cpp
    src/Infrastructure/Windows/WindowsDiagnosticSink.cpp
    src/Infrastructure/Windows/WindowsLegacyContinuityProjectionStore.cpp
    src/Infrastructure/Windows/WindowsLMStudioCandidateSelector.cpp
    src/Infrastructure/Windows/WindowsLMStudioDeploymentService.cpp
    src/Infrastructure/Windows/WindowsLMStudioEnvironment.cpp
    src/Infrastructure/Windows/WindowsLMStudioHostActivator.cpp
    src/Infrastructure/Windows/WindowsLMStudioServeVerifier.cpp
    src/Infrastructure/Windows/WindowsManagerAuthentication.cpp
    src/Infrastructure/Windows/WindowsManagerInstanceLease.cpp
    src/Infrastructure/Windows/WindowsManagerNamedPipeClient.cpp
    src/Infrastructure/Windows/WindowsManagerNamedPipeServer.cpp
    src/Infrastructure/Windows/WindowsManagerStartupService.cpp
    src/Infrastructure/Windows/WindowsNativeSessionLedger.cpp
    src/Infrastructure/Windows/WindowsProcessSupervisor.cpp
    src/Infrastructure/Windows/WindowsProjectWorkspaceAuthority.cpp
    src/Infrastructure/Windows/WindowsRuntimeDiagnostics.cpp
    src/Infrastructure/Windows/WindowsUnicodeCanonicalizer.cpp
    src/Infrastructure/Windows/WindowsUuidGenerator.cpp
    src/Infrastructure/Windows/WindowsWorkspaceAuthority.cpp
    src/Infrastructure/Windows/WinHttpLocalModelSessionTransport.cpp
    src/Infrastructure/Windows/Detail/AtomicReplaceEngine.cpp
    src/Infrastructure/Windows/Detail/BoundedSerialExecutor.cpp
    src/Infrastructure/Windows/Detail/CommandLineBuilder.cpp
    src/Infrastructure/Windows/Detail/CurrentUserPipeSecurity.cpp
    src/Infrastructure/Windows/Detail/WindowsDashboardUriLaunchPlatform.cpp
    src/Infrastructure/Windows/Detail/EtwProvider.cpp
    src/Infrastructure/Windows/Detail/JobObject.cpp
    src/Infrastructure/Windows/Detail/ManagerPipeIo.cpp
    src/Infrastructure/Windows/Detail/ManagerStartupComWorker.cpp
    src/Infrastructure/Windows/Detail/ManagerStartupDefinitionBuilder.cpp
    src/Infrastructure/Windows/Detail/ManagerStartupOwnershipProjector.cpp
    src/Infrastructure/Windows/Detail/OperationContextGuard.cpp
    src/Infrastructure/Windows/Detail/OperationGuard.cpp
    src/Infrastructure/Windows/Detail/OverlappedPipeReader.cpp
    src/Infrastructure/Windows/Detail/RelativeFileOperations.cpp
    src/Infrastructure/Windows/Detail/TaskSchedulerDurationCodec.cpp
    src/Infrastructure/Windows/Detail/UniqueRegistryKey.cpp
    src/Infrastructure/Windows/Detail/UtfConversion.cpp
    src/Infrastructure/Windows/Detail/Win32Error.cpp
    src/Infrastructure/Windows/Detail/WindowsManagerStartupComHandler.cpp
    src/Infrastructure/Windows/Detail/WindowsTaskSchedulerStartupPlatform.cpp
    src/Infrastructure/Windows/Detail/WindowsPathResolver.cpp
)
add_library(ForgeConductor.Infrastructure.Windows STATIC
    ${FORGE_INFRASTRUCTURE_WINDOWS_SOURCES})
forge_add_layer(ForgeConductor.Infrastructure.Windows ForgeConductor::Infrastructure.Windows
    ForgeConductor::Contracts
    ForgeConductor::Manager.Protocol)
forge_configure_native_target(ForgeConductor.Infrastructure.Windows)
target_include_directories(ForgeConductor.Infrastructure.Windows
    PUBLIC "${PROJECT_SOURCE_DIR}/include"
    PRIVATE "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(ForgeConductor.Infrastructure.Windows PRIVATE
    ForgeConductor::Manager.Startup
    nlohmann_json::nlohmann_json
    advapi32
    bcrypt
    crypt32
    normaliz
    ntdll
    ole32
    oleaut32
    shell32
    taskschd
    version
    winhttp)

set(FORGE_PERSISTENCE_WINDOWS_SOURCES
    src/Persistence/Windows/DatabaseBackupCoordinator.cpp
    src/Persistence/Windows/DatabaseQuarantine.cpp
    src/Persistence/Windows/WindowsAgentSessionRepository.cpp
    src/Persistence/Windows/WindowsAuditRepository.cpp
    src/Persistence/Windows/WindowsCentralDatabase.cpp
    src/Persistence/Windows/WindowsClientPresenceRepository.cpp
    src/Persistence/Windows/WindowsDashboardOperationalRepository.cpp
    src/Persistence/Windows/WindowsForgeStatusRepository.cpp
    src/Persistence/Windows/WindowsLegacyContinuityRepository.cpp
    src/Persistence/Windows/WindowsLegacyMemoryRepository.cpp
    src/Persistence/Windows/WindowsProjectDatabase.cpp
    src/Persistence/Windows/WindowsProjectMemoryArtifactStore.cpp
    src/Persistence/Windows/WindowsProjectMemoryRepository.cpp
    src/Persistence/Windows/WindowsProjectMemoryRepositoryOpener.cpp
    src/Persistence/Windows/WindowsProjectRegistryRepository.cpp
    src/Persistence/Windows/Detail/AnchoredSqliteVfs.cpp
    src/Persistence/Windows/Detail/DatabaseNamespaceLease.cpp
    src/Persistence/Windows/Detail/WindowsDatabaseStore.cpp
    src/Persistence/Windows/Detail/WinsqliteBackup.cpp
    src/Persistence/Windows/Detail/WinsqliteConnection.cpp
    src/Persistence/Windows/Detail/WinsqliteError.cpp
    src/Persistence/Windows/Detail/WinsqliteOperationGuard.cpp
    src/Persistence/Windows/Detail/WinsqliteStatement.cpp
    src/Persistence/Windows/Detail/WinsqliteTransaction.cpp
    src/Persistence/Windows/Migrations/CentralMigrations.cpp
    src/Persistence/Windows/Migrations/ProjectMigrations.cpp
    src/Persistence/Windows/Migrations/SchemaMigrator.cpp
)
add_library(ForgeConductor.Persistence.Windows STATIC
    ${FORGE_PERSISTENCE_WINDOWS_SOURCES})
forge_add_layer(ForgeConductor.Persistence.Windows ForgeConductor::Persistence.Windows
    ForgeConductor::Contracts
    ForgeConductor::Infrastructure.Windows)
forge_configure_native_target(ForgeConductor.Persistence.Windows)
target_include_directories(ForgeConductor.Persistence.Windows
    PUBLIC "${PROJECT_SOURCE_DIR}/include"
    PRIVATE "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(ForgeConductor.Persistence.Windows PRIVATE
    nlohmann_json::nlohmann_json
    bcrypt
    winsqlite3)

add_library(ForgeConductor.Manager.Composition.Windows STATIC
    src/Composition/Windows/ManagerDashboardOperationalDataSource.cpp
    src/Composition/Windows/ManagerDashboardOperationalDataSource.h
    src/Composition/Windows/ManagerDoctorService.cpp
    src/Composition/Windows/ManagerDoctorService.h
    src/Composition/Windows/ManagerLmStudioAuthorityRouter.cpp
    src/Composition/Windows/ManagerLmStudioAuthorityRouter.h
    src/Composition/Windows/ManagerLmStudioReadScopeResolver.cpp
    src/Composition/Windows/ManagerLmStudioReadScopeResolver.h
    src/Composition/Windows/ManagerMaintenanceService.cpp
    src/Composition/Windows/ManagerMaintenanceService.h
    src/Composition/Windows/ManagerProcessEnvironment.cpp
    src/Composition/Windows/ManagerProcessEnvironment.h
    src/Composition/Windows/UnavailableLmStudioDeploymentService.cpp
    src/Composition/Windows/UnavailableLmStudioDeploymentService.h
    src/Composition/Windows/UnavailableTelemetryService.cpp
    src/Composition/Windows/UnavailableTelemetryService.h)
add_library(ForgeConductor::Manager.Composition.Windows ALIAS
    ForgeConductor.Manager.Composition.Windows)
forge_configure_native_target(ForgeConductor.Manager.Composition.Windows)
target_include_directories(ForgeConductor.Manager.Composition.Windows PUBLIC
    "${PROJECT_SOURCE_DIR}/src/Composition/Windows")
target_link_libraries(ForgeConductor.Manager.Composition.Windows PUBLIC
    ForgeConductor::Application
    ForgeConductor::Manager.Host
    ForgeConductor::Infrastructure.Windows
    ForgeConductor::Persistence.Windows)

set(FORGE_NATIVE_TOOLS_WINDOWS_SOURCES
    src/NativeTools/Windows/NativeFileOperations.cpp
    src/NativeTools/Windows/WindowsFileSystem.cpp
    src/NativeTools/Windows/WindowsGitService.cpp
    src/NativeTools/Windows/WindowsPathGlobService.cpp
    src/NativeTools/Windows/WindowsPdfService.cpp
    src/NativeTools/Windows/WindowsShellService.cpp
    src/NativeTools/Windows/WindowsTextSearchService.cpp
)
add_library(ForgeConductor.NativeTools.Windows STATIC
    ${FORGE_NATIVE_TOOLS_WINDOWS_SOURCES})
forge_add_layer(ForgeConductor.NativeTools.Windows ForgeConductor::NativeTools.Windows
    ForgeConductor::Contracts
    ForgeConductor::Infrastructure.Windows)
forge_configure_native_target(ForgeConductor.NativeTools.Windows)
target_include_directories(ForgeConductor.NativeTools.Windows
    PUBLIC "${PROJECT_SOURCE_DIR}/include"
    PRIVATE "${PROJECT_SOURCE_DIR}/src")
add_library(ForgeConductor.SessionHost.Core STATIC
    src/SessionHost/BoundedLogicalContinuationQueue.cpp
    src/SessionHost/ForgeNativeSessionHostAdapter.cpp
    src/SessionHost/LocalLogicalSessionTransport.cpp
)
forge_add_layer(ForgeConductor.SessionHost.Core ForgeConductor::SessionHost.Core
    ForgeConductor::Application
    ForgeConductor::Contracts
    nlohmann_json::nlohmann_json)
forge_configure_standard_target(ForgeConductor.SessionHost.Core)
target_include_directories(ForgeConductor.SessionHost.Core PUBLIC
    "${PROJECT_SOURCE_DIR}/include")

add_library(ForgeNativeSessionHostPlugin SHARED
    src/SessionHost/Plugin/ForgeNativeSessionHostPlugin.cpp)
forge_configure_native_target(ForgeNativeSessionHostPlugin)
target_include_directories(ForgeNativeSessionHostPlugin PUBLIC
    "${PROJECT_SOURCE_DIR}/include")
target_link_libraries(ForgeNativeSessionHostPlugin PRIVATE
    ForgeConductor::SessionHost.Core)
add_library(ForgeConductor.Mcp STATIC
    src/Mcp/McpClientWorkspaceContext.cpp
    src/Mcp/McpExecutionServices.cpp
    src/Mcp/McpInvocationGuard.cpp
    src/Mcp/McpJsonCodec.cpp
    src/Mcp/McpProtocol.cpp
    src/Mcp/McpServer.cpp
    src/Mcp/McpToolCatalog.cpp
    src/Mcp/McpToolPackAdapter.cpp
    src/Mcp/McpToolRouter.cpp
    src/Mcp/WindowsStdioMcpTransport.cpp
)
forge_add_layer(ForgeConductor.Mcp ForgeConductor::Mcp
    ForgeConductor::Application
    ForgeConductor::Contracts)
forge_configure_standard_target(ForgeConductor.Mcp)
target_include_directories(ForgeConductor.Mcp PUBLIC
    "${PROJECT_SOURCE_DIR}/include")
target_link_libraries(ForgeConductor.Mcp PRIVATE
    nlohmann_json::nlohmann_json)
forge_add_layer(ForgeConductor.Composition.Windows ForgeConductor::Composition.Windows
    ForgeConductor::Application
    ForgeConductor::Infrastructure.Windows
    ForgeConductor::Persistence.Windows
    ForgeConductor::NativeTools.Windows
    ForgeConductor::SessionHost.Core
    ForgeConductor::Mcp
    Forsetti::Platform
    Forsetti::HostTemplate)

add_library(ForgeConductor.ForsettiModule STATIC
    src/ForsettiModule/ForgeConductorAppModule.cpp
)
add_library(ForgeConductor::ForsettiModule ALIAS ForgeConductor.ForsettiModule)
forge_configure_native_target(ForgeConductor.ForsettiModule)
target_include_directories(ForgeConductor.ForsettiModule PUBLIC
    "${PROJECT_SOURCE_DIR}/include")
target_link_libraries(ForgeConductor.ForsettiModule PUBLIC
    Forsetti::Core
    ForgeConductor::Contracts
)

add_executable(ForgeConductor.Cli
    src/Hosts/Cli/CliCompositionRoot.cpp
    src/Hosts/Cli/CliCompositionRoot.h
    src/Hosts/Cli/McpServeCompositionRoot.cpp
    src/Hosts/Cli/McpServeCompositionRoot.h
    src/Hosts/Cli/main.cpp
)
add_executable(ForgeConductor::Cli ALIAS ForgeConductor.Cli)
set_target_properties(ForgeConductor.Cli PROPERTIES OUTPUT_NAME "forge-conductor")
forge_configure_native_target(ForgeConductor.Cli)
target_link_libraries(ForgeConductor.Cli PRIVATE
    ForgeConductor::Composition.Windows)

add_library(ForgeConductor.Manager.CompositionRoot.Windows STATIC
    src/Hosts/Manager/ManagerCompositionRoot.cpp
    src/Hosts/Manager/ManagerCompositionRoot.h)
add_library(ForgeConductor::Manager.CompositionRoot.Windows ALIAS
    ForgeConductor.Manager.CompositionRoot.Windows)
forge_configure_native_target(
    ForgeConductor.Manager.CompositionRoot.Windows)
target_include_directories(
    ForgeConductor.Manager.CompositionRoot.Windows
    PUBLIC "${PROJECT_SOURCE_DIR}/src/Hosts/Manager"
    PRIVATE "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(
    ForgeConductor.Manager.CompositionRoot.Windows PUBLIC
    ForgeConductor::Composition.Windows
    ForgeConductor::Manager.Composition.Windows
    ForgeConductor::Manager.Windows)

add_executable(ForgeConductor.Manager
    src/Hosts/Manager/ManagerProcessArguments.cpp
    src/Hosts/Manager/ManagerProcessArguments.h
    src/Hosts/Manager/Resources/Dashboard/DashboardAssets.rc
    src/Hosts/Manager/main.cpp)
add_executable(ForgeConductor::Manager ALIAS ForgeConductor.Manager)
set_target_properties(ForgeConductor.Manager PROPERTIES
    OUTPUT_NAME "ForgeConductor.Manager")
forge_configure_native_target(ForgeConductor.Manager)
target_include_directories(ForgeConductor.Manager PRIVATE
    "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(ForgeConductor.Manager PRIVATE
    ForgeConductor::Manager.CompositionRoot.Windows)
add_dependencies(ForgeConductor.Manager ForgeConductor.Cli)
add_custom_command(TARGET ForgeConductor.Manager POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
        "$<TARGET_FILE:ForgeConductor.Cli>"
        "$<TARGET_FILE_DIR:ForgeConductor.Manager>/$<TARGET_FILE_NAME:ForgeConductor.Cli>"
    COMMENT "Staging the exact sibling CLI beside the production Manager"
    VERBATIM)

add_executable(ForgeConductor.SessionHost
    src/Hosts/SessionHost/SessionHostCompositionRoot.cpp
    src/Hosts/SessionHost/SessionHostCompositionRoot.h
    src/Hosts/SessionHost/main.cpp
)
add_executable(
    ForgeConductor::SessionHost ALIAS ForgeConductor.SessionHost)
forge_configure_native_target(ForgeConductor.SessionHost)
target_link_libraries(ForgeConductor.SessionHost PRIVATE
    ForgeConductor::Infrastructure.Windows
    ForgeConductor::SessionHost.Core
    nlohmann_json::nlohmann_json)

add_executable(ForgeConductor.ForsettiHostSmoke
    tests/ForsettiHostSmoke/main.cpp
)
add_executable(ForgeConductor::ForsettiHostSmoke ALIAS ForgeConductor.ForsettiHostSmoke)
forge_configure_native_target(ForgeConductor.ForsettiHostSmoke)
target_link_libraries(ForgeConductor.ForsettiHostSmoke PRIVATE
    ForgeConductor::ForsettiModule
    Forsetti::HostTemplate
)

set(FORGE_CANONICAL_MANIFEST
    "${PROJECT_SOURCE_DIR}/src/ForgeConductor.ForsettiModule/Resources/ForsettiManifests/ForgeConductorAppModule.json")
if(NOT EXISTS "${FORGE_CANONICAL_MANIFEST}")
    message(FATAL_ERROR "The canonical Forge Conductor Forsetti manifest is missing.")
endif()

set(FORGE_AGENT_RESOURCE_FILES
    src/ForgeConductor.Application/Resources/Agents/debug.md
    src/ForgeConductor.Application/Resources/Agents/docs.md
    src/ForgeConductor.Application/Resources/Agents/explore.md
    src/ForgeConductor.Application/Resources/Agents/implement.md
    src/ForgeConductor.Application/Resources/Agents/plan.md
    src/ForgeConductor.Application/Resources/Agents/precommit-audit.md
    src/ForgeConductor.Application/Resources/Agents/research.md
    src/ForgeConductor.Application/Resources/Agents/review.md
    src/ForgeConductor.Application/Resources/Agents/security.md
    src/ForgeConductor.Application/Resources/Agents/test.md
)
set(FORGE_AGENT_RESOURCE_PATHS)
foreach(_forge_agent_resource IN LISTS FORGE_AGENT_RESOURCE_FILES)
    if(NOT EXISTS "${PROJECT_SOURCE_DIR}/${_forge_agent_resource}")
        message(FATAL_ERROR
            "A mandatory Forge Conductor agent resource is missing: ${_forge_agent_resource}")
    endif()
    list(APPEND FORGE_AGENT_RESOURCE_PATHS
        "${PROJECT_SOURCE_DIR}/${_forge_agent_resource}")
endforeach()

add_custom_command(TARGET ForgeConductor.ForsettiHostSmoke POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E make_directory
        "$<TARGET_FILE_DIR:ForgeConductor.ForsettiHostSmoke>/ForsettiManifests"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
        "${FORGE_CANONICAL_MANIFEST}"
        "$<TARGET_FILE_DIR:ForgeConductor.ForsettiHostSmoke>/ForsettiManifests/ForgeConductorAppModule.json"
    COMMENT "Staging the canonical Forge Conductor manifest beside the Forsetti smoke host"
    VERBATIM
)

if(BUILD_TESTING)
    add_executable(ForgeConductor.Telemetry.WindowsTests
        tests/Telemetry/TelemetryWindowsTestMain.cpp
        tests/Telemetry/WindowsRamMetricsCollectorTests.cpp)
    forge_configure_native_target(
        ForgeConductor.Telemetry.WindowsTests)
    target_include_directories(
        ForgeConductor.Telemetry.WindowsTests PRIVATE
        "${PROJECT_SOURCE_DIR}/src"
        "${PROJECT_SOURCE_DIR}/tests")
    target_link_libraries(
        ForgeConductor.Telemetry.WindowsTests PRIVATE
        ForgeConductor::Telemetry.Windows)
    add_test(
        NAME ForgeConductor.Telemetry.WindowsTests
        COMMAND $<TARGET_FILE:ForgeConductor.Telemetry.WindowsTests>)
    set_tests_properties(
        ForgeConductor.Telemetry.WindowsTests PROPERTIES
        LABELS "G17;T-TEL;T-UNIT;T-INTEGRATION"
        TIMEOUT 60)

    add_executable(ForgeConductor.Manager.CompositionFixture
        tests/Manager/ManagerCompositionFixture.cpp
        src/Hosts/Manager/Resources/Dashboard/DashboardAssets.rc)
    forge_configure_native_target(
        ForgeConductor.Manager.CompositionFixture)
    target_link_libraries(
        ForgeConductor.Manager.CompositionFixture PRIVATE
        ForgeConductor::Manager.CompositionRoot.Windows)
    set_target_properties(
        ForgeConductor.Manager.CompositionFixture PROPERTIES
        OUTPUT_NAME "ForgeConductor.Manager"
        RUNTIME_OUTPUT_DIRECTORY_DEBUG
            "${CMAKE_BINARY_DIR}/tests/manager-composition-fixture/Debug"
        RUNTIME_OUTPUT_DIRECTORY_RELEASE
            "${CMAKE_BINARY_DIR}/tests/manager-composition-fixture/Release")
    add_custom_target(
        ForgeConductor.Manager.CompositionFixture.CliStage
        COMMAND "${CMAKE_COMMAND}" -E make_directory
            "${CMAKE_BINARY_DIR}/tests/manager-composition-fixture/$<CONFIG>"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "$<TARGET_FILE:ForgeConductor.Cli>"
            "${CMAKE_BINARY_DIR}/tests/manager-composition-fixture/$<CONFIG>/$<TARGET_FILE_NAME:ForgeConductor.Cli>"
        COMMENT "Staging the exact sibling CLI beside the controlled Manager composition fixture"
        VERBATIM)
    add_dependencies(
        ForgeConductor.Manager.CompositionFixture.CliStage
        ForgeConductor.Cli)
    add_dependencies(
        ForgeConductor.Manager.CompositionFixture
        ForgeConductor.Manager.CompositionFixture.CliStage)

    add_executable(ForgeConductor.Manager.CompositionLifecycleTests
        tests/Manager/WindowsManagerCompositionLifecycleTests.cpp)
    forge_configure_native_target(
        ForgeConductor.Manager.CompositionLifecycleTests)
    target_link_libraries(
        ForgeConductor.Manager.CompositionLifecycleTests PRIVATE
        ForgeConductor::Infrastructure.Windows
        advapi32
        winhttp
        ws2_32)
    add_dependencies(
        ForgeConductor.Manager.CompositionLifecycleTests
        ForgeConductor.Manager.CompositionFixture)
    add_test(
        NAME ForgeConductor.Manager.CompositionLifecycleTests
        COMMAND
            $<TARGET_FILE:ForgeConductor.Manager.CompositionLifecycleTests>
            $<TARGET_FILE:ForgeConductor.Manager.CompositionFixture>
            $<TARGET_FILE:ForgeConductor.Cli>)
    set_tests_properties(
        ForgeConductor.Manager.CompositionLifecycleTests PROPERTIES
        LABELS "G16;T-MGR;T-PROC;T-SEC;T-INTEGRATION;T-STRESS"
        RUN_SERIAL TRUE
        TIMEOUT 480)

    add_executable(ForgeConductor.Manager.ProcessArgumentsTests
        tests/Manager/ManagerProcessArgumentsTests.cpp
        src/Hosts/Manager/ManagerProcessArguments.cpp
        src/Hosts/Manager/ManagerProcessArguments.h)
    forge_configure_native_target(
        ForgeConductor.Manager.ProcessArgumentsTests)
    target_include_directories(
        ForgeConductor.Manager.ProcessArgumentsTests PRIVATE
        "${PROJECT_SOURCE_DIR}/src/Hosts/Manager")
    target_link_libraries(
        ForgeConductor.Manager.ProcessArgumentsTests PRIVATE
        ForgeConductor::Domain)
    add_test(
        NAME ForgeConductor.Manager.ProcessArgumentsTests
        COMMAND $<TARGET_FILE:ForgeConductor.Manager.ProcessArgumentsTests>)
    set_tests_properties(
        ForgeConductor.Manager.ProcessArgumentsTests PROPERTIES
        LABELS "G16;T-MGR;T-PROC;T-SEC;T-UNIT"
        TIMEOUT 60)

    add_executable(ForgeConductor.Manager.StartupTests
        tests/Manager/ManagerStartupTaskPolicyTests.cpp)
    forge_configure_standard_target(ForgeConductor.Manager.StartupTests)
    target_link_libraries(ForgeConductor.Manager.StartupTests PRIVATE
        ForgeConductor::Manager.Startup)
    add_test(
        NAME ForgeConductor.Manager.StartupTests
        COMMAND $<TARGET_FILE:ForgeConductor.Manager.StartupTests>)
    set_tests_properties(ForgeConductor.Manager.StartupTests PROPERTIES
        LABELS "G16;T-MGR;T-UNIT"
        TIMEOUT 60)

    add_executable(ForgeConductor.Manager.DeadlineMapperTests
        tests/Manager/ManagerDeadlineMapperTests.cpp)
    forge_configure_standard_target(ForgeConductor.Manager.DeadlineMapperTests)
    target_link_libraries(ForgeConductor.Manager.DeadlineMapperTests PRIVATE
        ForgeConductor::Manager.Protocol)
    add_test(
        NAME ForgeConductor.Manager.DeadlineMapperTests
        COMMAND $<TARGET_FILE:ForgeConductor.Manager.DeadlineMapperTests>)
    set_tests_properties(ForgeConductor.Manager.DeadlineMapperTests PROPERTIES
        LABELS "G16;T-MGR;T-UNIT"
        TIMEOUT 60)

    add_executable(ForgeConductor.Manager.RequestDispatcherTests
        tests/Manager/ManagerRequestDispatcherTests.cpp)
    forge_configure_standard_target(ForgeConductor.Manager.RequestDispatcherTests)
    target_link_libraries(ForgeConductor.Manager.RequestDispatcherTests PRIVATE
        ForgeConductor::Manager.Protocol)
    add_test(
        NAME ForgeConductor.Manager.RequestDispatcherTests
        COMMAND $<TARGET_FILE:ForgeConductor.Manager.RequestDispatcherTests>)
    set_tests_properties(ForgeConductor.Manager.RequestDispatcherTests PROPERTIES
        LABELS "G16;T-MGR;T-UNIT;T-STRESS"
        TIMEOUT 60)

    add_executable(ForgeConductor.Manager.ProtocolTests
        tests/Manager/ManagerProtocolCodecTests.cpp)
    forge_configure_standard_target(ForgeConductor.Manager.ProtocolTests)
    target_link_libraries(ForgeConductor.Manager.ProtocolTests PRIVATE
        ForgeConductor::Manager.Protocol
        nlohmann_json::nlohmann_json)
    add_test(
        NAME ForgeConductor.Manager.ProtocolTests
        COMMAND $<TARGET_FILE:ForgeConductor.Manager.ProtocolTests>)
    set_tests_properties(ForgeConductor.Manager.ProtocolTests PROPERTIES
        LABELS "G16;T-MGR;T-SEC;T-UNIT"
        TIMEOUT 60)

    add_executable(ForgeConductor.Manager.ControllerTests
        tests/Manager/ManagerControllerTests.cpp)
    forge_configure_standard_target(ForgeConductor.Manager.ControllerTests)
    target_link_libraries(ForgeConductor.Manager.ControllerTests PRIVATE
        ForgeConductor::Application)
    add_test(
        NAME ForgeConductor.Manager.ControllerTests
        COMMAND $<TARGET_FILE:ForgeConductor.Manager.ControllerTests>)
    set_tests_properties(ForgeConductor.Manager.ControllerTests PROPERTIES
        LABELS "G16;T-MGR;T-UNIT;T-STRESS"
        TIMEOUT 60)

    add_executable(ForgeConductor.Manager.WindowsInfrastructureTests
        tests/Manager/ManagerStartupComWorkerTests.cpp
        tests/Manager/ManagerStartupDefinitionBuilderTests.cpp
        tests/Manager/ManagerStartupOwnershipProjectorTests.cpp
        tests/Manager/ManagerPipeInfrastructureTests.cpp
        tests/Manager/TaskSchedulerDurationCodecTests.cpp
        tests/Manager/WindowsManagerAuthenticationTests.cpp
        tests/Manager/WindowsManagerInfrastructureTestMain.cpp
        tests/Manager/WindowsManagerStartupComRaiiTests.cpp
        tests/Manager/WindowsManagerStartupComHandlerTests.cpp
        tests/Manager/WindowsManagerStartupServiceTests.cpp
        tests/Manager/WindowsManagerOwnershipTests.cpp)
    forge_configure_native_target(
        ForgeConductor.Manager.WindowsInfrastructureTests)
    target_include_directories(
        ForgeConductor.Manager.WindowsInfrastructureTests PRIVATE
        "${PROJECT_SOURCE_DIR}/src"
        "${PROJECT_SOURCE_DIR}/tests")
    target_link_libraries(
        ForgeConductor.Manager.WindowsInfrastructureTests PRIVATE
        ForgeConductor::Infrastructure.Windows)
    add_test(
        NAME ForgeConductor.Manager.WindowsInfrastructureTests
        COMMAND $<TARGET_FILE:ForgeConductor.Manager.WindowsInfrastructureTests>)
    set_tests_properties(
        ForgeConductor.Manager.WindowsInfrastructureTests PROPERTIES
        LABELS "G16;T-MGR;T-SEC;T-UNIT;T-STRESS"
        TIMEOUT 60)

    add_executable(ForgeConductor.Manager.StartupLifecycleFixture
        tests/Manager/ManagerStartupLifecycleFixture.cpp)
    forge_configure_native_target(
        ForgeConductor.Manager.StartupLifecycleFixture)

    add_executable(ForgeConductor.Manager.StartupLifecycleIntegrationTests
        tests/Manager/WindowsManagerStartupLifecycleTests.cpp)
    forge_configure_native_target(
        ForgeConductor.Manager.StartupLifecycleIntegrationTests)
    target_include_directories(
        ForgeConductor.Manager.StartupLifecycleIntegrationTests PRIVATE
        "${PROJECT_SOURCE_DIR}/src")
    target_link_libraries(
        ForgeConductor.Manager.StartupLifecycleIntegrationTests PRIVATE
        ForgeConductor::Infrastructure.Windows)
    add_dependencies(
        ForgeConductor.Manager.StartupLifecycleIntegrationTests
        ForgeConductor.Manager.StartupLifecycleFixture)
    add_test(
        NAME ForgeConductor.Manager.StartupLifecycleIntegrationTests
        COMMAND
            $<TARGET_FILE:ForgeConductor.Manager.StartupLifecycleIntegrationTests>
            "$<TARGET_FILE:ForgeConductor.Manager.StartupLifecycleFixture>")
    set_tests_properties(
        ForgeConductor.Manager.StartupLifecycleIntegrationTests PROPERTIES
        LABELS "G16;T-MGR;T-SEC;T-INTEGRATION"
        RUN_SERIAL TRUE
        TIMEOUT 90)

    add_executable(ForgeConductor.Manager.NamedPipeRoundTripTests
        tests/Manager/WindowsManagerNamedPipeRoundTripTests.cpp)
    forge_configure_native_target(
        ForgeConductor.Manager.NamedPipeRoundTripTests)
    target_include_directories(
        ForgeConductor.Manager.NamedPipeRoundTripTests PRIVATE
        "${PROJECT_SOURCE_DIR}/src"
        "${PROJECT_SOURCE_DIR}/tests")
    target_link_libraries(
        ForgeConductor.Manager.NamedPipeRoundTripTests PRIVATE
        ForgeConductor::Infrastructure.Windows)
    add_test(
        NAME ForgeConductor.Manager.NamedPipeRoundTripTests
        COMMAND $<TARGET_FILE:ForgeConductor.Manager.NamedPipeRoundTripTests>)
    set_tests_properties(
        ForgeConductor.Manager.NamedPipeRoundTripTests PROPERTIES
        LABELS "G16;T-MGR;T-SEC;T-INTEGRATION;T-STRESS"
        TIMEOUT 120)

    add_executable(ForgeConductor.Manager.ProcessHostTests
        tests/Manager/ManagerProcessHostTests.cpp)
    forge_configure_standard_target(ForgeConductor.Manager.ProcessHostTests)
    target_link_libraries(ForgeConductor.Manager.ProcessHostTests PRIVATE
        ForgeConductor::Manager.Host)
    add_test(
        NAME ForgeConductor.Manager.ProcessHostTests
        COMMAND $<TARGET_FILE:ForgeConductor.Manager.ProcessHostTests>)
    set_tests_properties(ForgeConductor.Manager.ProcessHostTests PROPERTIES
        LABELS "G16;T-MGR;T-UNIT;T-STRESS"
        TIMEOUT 60)

    add_executable(ForgeConductor.Manager.BrowserLauncherTests
        tests/Manager/WindowsDashboardBrowserLauncherTests.cpp)
    forge_configure_native_target(
        ForgeConductor.Manager.BrowserLauncherTests)
    target_include_directories(
        ForgeConductor.Manager.BrowserLauncherTests PRIVATE
        "${PROJECT_SOURCE_DIR}/src"
        "${PROJECT_SOURCE_DIR}/tests")
    target_link_libraries(
        ForgeConductor.Manager.BrowserLauncherTests PRIVATE
        ForgeConductor::Infrastructure.Windows)
    add_dependencies(
        ForgeConductor.Manager.BrowserLauncherTests
        ForgeConductor.Cli)
    add_test(
        NAME ForgeConductor.Manager.BrowserLauncherTests
        COMMAND $<TARGET_FILE:ForgeConductor.Manager.BrowserLauncherTests>
            "$<TARGET_FILE:ForgeConductor.Cli>")
    set_tests_properties(
        ForgeConductor.Manager.BrowserLauncherTests PROPERTIES
        LABELS "G16;T-INTEGRATION;T-MGR;T-SEC;T-STRESS;T-UNIT"
        TIMEOUT 60)

    add_executable(ForgeConductor.Manager.ProcessStopWatcherTests
        tests/Manager/ManagerProcessStopWatcherTests.cpp)
    forge_configure_standard_target(
        ForgeConductor.Manager.ProcessStopWatcherTests)
    target_link_libraries(
        ForgeConductor.Manager.ProcessStopWatcherTests PRIVATE
        ForgeConductor::Manager.Host)
    add_test(
        NAME ForgeConductor.Manager.ProcessStopWatcherTests
        COMMAND $<TARGET_FILE:ForgeConductor.Manager.ProcessStopWatcherTests>)
    set_tests_properties(
        ForgeConductor.Manager.ProcessStopWatcherTests PROPERTIES
        LABELS "G16;T-MGR;T-UNIT;T-STRESS"
        TIMEOUT 60)

    add_executable(ForgeConductor.Manager.CompositionSupportTests
        tests/Manager/ManagerCompositionSupportTests.cpp)
    forge_configure_native_target(
        ForgeConductor.Manager.CompositionSupportTests)
    target_include_directories(
        ForgeConductor.Manager.CompositionSupportTests PRIVATE
        "${PROJECT_SOURCE_DIR}/src")
    target_link_libraries(
        ForgeConductor.Manager.CompositionSupportTests PRIVATE
        ForgeConductor::Manager.Host
        ForgeConductor::Dashboard.Windows)
    add_test(
        NAME ForgeConductor.Manager.CompositionSupportTests
        COMMAND $<TARGET_FILE:ForgeConductor.Manager.CompositionSupportTests>)
    set_tests_properties(
        ForgeConductor.Manager.CompositionSupportTests PROPERTIES
        LABELS "G16;T-MGR;T-UNIT;T-STRESS"
        TIMEOUT 60)

    add_executable(
        ForgeConductor.Manager.DashboardOperationalDataSourceTests
        tests/Composition/Windows/ManagerDashboardOperationalDataSourceTests.cpp)
    forge_configure_native_target(
        ForgeConductor.Manager.DashboardOperationalDataSourceTests)
    target_include_directories(
        ForgeConductor.Manager.DashboardOperationalDataSourceTests PRIVATE
        "${PROJECT_SOURCE_DIR}/src"
        "${PROJECT_SOURCE_DIR}/tests")
    target_link_libraries(
        ForgeConductor.Manager.DashboardOperationalDataSourceTests PRIVATE
        ForgeConductor::Manager.Composition.Windows)
    add_test(
        NAME ForgeConductor.Manager.DashboardOperationalDataSourceTests
        COMMAND
            $<TARGET_FILE:ForgeConductor.Manager.DashboardOperationalDataSourceTests>)
    set_tests_properties(
        ForgeConductor.Manager.DashboardOperationalDataSourceTests PROPERTIES
        LABELS "G16;T-MGR;T-DB;T-INTEGRATION;T-UNIT"
        TIMEOUT 120)

    add_executable(
        ForgeConductor.Manager.UnavailableTelemetryServiceTests
        tests/Composition/Windows/UnavailableTelemetryServiceTests.cpp)
    forge_configure_native_target(
        ForgeConductor.Manager.UnavailableTelemetryServiceTests)
    target_include_directories(
        ForgeConductor.Manager.UnavailableTelemetryServiceTests PRIVATE
        "${PROJECT_SOURCE_DIR}/src")
    target_link_libraries(
        ForgeConductor.Manager.UnavailableTelemetryServiceTests PRIVATE
        ForgeConductor::Manager.Composition.Windows)
    add_test(
        NAME ForgeConductor.Manager.UnavailableTelemetryServiceTests
        COMMAND
            $<TARGET_FILE:ForgeConductor.Manager.UnavailableTelemetryServiceTests>)
    set_tests_properties(
        ForgeConductor.Manager.UnavailableTelemetryServiceTests PROPERTIES
        LABELS "G16;T-MGR;T-TEL;T-UNIT"
        TIMEOUT 60)

    add_executable(
        ForgeConductor.Manager.DoctorServiceTests
        tests/Composition/Windows/ManagerDoctorServiceTests.cpp)
    forge_configure_native_target(
        ForgeConductor.Manager.DoctorServiceTests)
    target_include_directories(
        ForgeConductor.Manager.DoctorServiceTests PRIVATE
        "${PROJECT_SOURCE_DIR}/src"
        "${PROJECT_SOURCE_DIR}/tests")
    target_link_libraries(
        ForgeConductor.Manager.DoctorServiceTests PRIVATE
        ForgeConductor::Manager.Composition.Windows)
    add_test(
        NAME ForgeConductor.Manager.DoctorServiceTests
        COMMAND $<TARGET_FILE:ForgeConductor.Manager.DoctorServiceTests>)
    set_tests_properties(
        ForgeConductor.Manager.DoctorServiceTests PROPERTIES
        LABELS "G16;T-MGR;T-INTEGRATION;T-UNIT"
        TIMEOUT 120)

    add_executable(
        ForgeConductor.Manager.MaintenanceServiceTests
        tests/Composition/Windows/ManagerMaintenanceServiceTests.cpp)
    forge_configure_native_target(
        ForgeConductor.Manager.MaintenanceServiceTests)
    target_include_directories(
        ForgeConductor.Manager.MaintenanceServiceTests PRIVATE
        "${PROJECT_SOURCE_DIR}/src"
        "${PROJECT_SOURCE_DIR}/tests")
    target_link_libraries(
        ForgeConductor.Manager.MaintenanceServiceTests PRIVATE
        ForgeConductor::Manager.Composition.Windows)
    add_test(
        NAME ForgeConductor.Manager.MaintenanceServiceTests
        COMMAND $<TARGET_FILE:ForgeConductor.Manager.MaintenanceServiceTests>)
    set_tests_properties(
        ForgeConductor.Manager.MaintenanceServiceTests PROPERTIES
        LABELS "G16;T-MGR;T-LMS;T-CONT;T-UNIT"
        TIMEOUT 60)

    add_executable(
        ForgeConductor.Manager.ProcessEnvironmentTests
        tests/Composition/Windows/ManagerProcessEnvironmentTests.cpp)
    forge_configure_native_target(
        ForgeConductor.Manager.ProcessEnvironmentTests)
    target_include_directories(
        ForgeConductor.Manager.ProcessEnvironmentTests PRIVATE
        "${PROJECT_SOURCE_DIR}/src"
        "${PROJECT_SOURCE_DIR}/tests")
    target_link_libraries(
        ForgeConductor.Manager.ProcessEnvironmentTests PRIVATE
        ForgeConductor::Manager.Composition.Windows)
    add_test(
        NAME ForgeConductor.Manager.ProcessEnvironmentTests
        COMMAND $<TARGET_FILE:ForgeConductor.Manager.ProcessEnvironmentTests>)
    set_tests_properties(
        ForgeConductor.Manager.ProcessEnvironmentTests PROPERTIES
        LABELS "G16;T-MGR;T-INTEGRATION;T-UNIT"
        TIMEOUT 60)

    add_executable(
        ForgeConductor.Manager.ProcessEnvironmentRealTests
        tests/Composition/Windows/ManagerProcessEnvironmentRealTests.cpp)
    forge_configure_native_target(
        ForgeConductor.Manager.ProcessEnvironmentRealTests)
    target_include_directories(
        ForgeConductor.Manager.ProcessEnvironmentRealTests PRIVATE
        "${PROJECT_SOURCE_DIR}/src"
        "${PROJECT_SOURCE_DIR}/tests")
    target_link_libraries(
        ForgeConductor.Manager.ProcessEnvironmentRealTests PRIVATE
        ForgeConductor::Manager.Composition.Windows)
    set_target_properties(
        ForgeConductor.Manager.ProcessEnvironmentRealTests PROPERTIES
        OUTPUT_NAME "ForgeConductor.Manager"
        RUNTIME_OUTPUT_DIRECTORY_DEBUG
            "${CMAKE_BINARY_DIR}/tests/manager-process-environment-real/Debug"
        RUNTIME_OUTPUT_DIRECTORY_RELEASE
            "${CMAKE_BINARY_DIR}/tests/manager-process-environment-real/Release")
    add_dependencies(
        ForgeConductor.Manager.ProcessEnvironmentRealTests
        ForgeConductor.Cli)
    add_custom_command(
        TARGET ForgeConductor.Manager.ProcessEnvironmentRealTests POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "$<TARGET_FILE:ForgeConductor.Cli>"
            "$<TARGET_FILE_DIR:ForgeConductor.Manager.ProcessEnvironmentRealTests>/forge-conductor.exe"
        COMMENT "Staging the exact sibling CLI for the real Manager process environment fixture"
        VERBATIM)
    add_test(
        NAME ForgeConductor.Manager.ProcessEnvironmentRealTests
        COMMAND
            $<TARGET_FILE:ForgeConductor.Manager.ProcessEnvironmentRealTests>)
    set_tests_properties(
        ForgeConductor.Manager.ProcessEnvironmentRealTests PROPERTIES
        LABELS "G16;T-MGR;T-PROC;T-INTEGRATION"
        TIMEOUT 60)

    add_executable(
        ForgeConductor.Manager.LmStudioReadScopeResolverTests
        tests/Composition/Windows/ManagerLmStudioReadScopeResolverTests.cpp)
    forge_configure_native_target(
        ForgeConductor.Manager.LmStudioReadScopeResolverTests)
    target_include_directories(
        ForgeConductor.Manager.LmStudioReadScopeResolverTests PRIVATE
        "${PROJECT_SOURCE_DIR}/src"
        "${PROJECT_SOURCE_DIR}/tests")
    target_link_libraries(
        ForgeConductor.Manager.LmStudioReadScopeResolverTests PRIVATE
        ForgeConductor::Manager.Composition.Windows)
    add_test(
        NAME ForgeConductor.Manager.LmStudioReadScopeResolverTests
        COMMAND
            $<TARGET_FILE:ForgeConductor.Manager.LmStudioReadScopeResolverTests>)
    set_tests_properties(
        ForgeConductor.Manager.LmStudioReadScopeResolverTests PROPERTIES
        LABELS "G16;T-MGR;T-LMS;T-INTEGRATION;T-UNIT"
        TIMEOUT 60)

    add_executable(
        ForgeConductor.Manager.LmStudioCompositionSupportTests
        tests/Composition/Windows/ManagerLmStudioCompositionSupportTests.cpp)
    forge_configure_native_target(
        ForgeConductor.Manager.LmStudioCompositionSupportTests)
    target_include_directories(
        ForgeConductor.Manager.LmStudioCompositionSupportTests PRIVATE
        "${PROJECT_SOURCE_DIR}/src"
        "${PROJECT_SOURCE_DIR}/tests")
    target_link_libraries(
        ForgeConductor.Manager.LmStudioCompositionSupportTests PRIVATE
        ForgeConductor::Manager.Composition.Windows)
    add_test(
        NAME ForgeConductor.Manager.LmStudioCompositionSupportTests
        COMMAND
            $<TARGET_FILE:ForgeConductor.Manager.LmStudioCompositionSupportTests>)
    set_tests_properties(
        ForgeConductor.Manager.LmStudioCompositionSupportTests PROPERTIES
        LABELS "G16;T-MGR;T-LMS;T-INTEGRATION;T-SEC;T-UNIT"
        TIMEOUT 60)

    add_executable(ForgeConductor.Manager.ProcessRestartSignalTests
        tests/Manager/ManagerProcessRestartSignalTests.cpp)
    forge_configure_standard_target(
        ForgeConductor.Manager.ProcessRestartSignalTests)
    target_link_libraries(
        ForgeConductor.Manager.ProcessRestartSignalTests PRIVATE
        ForgeConductor::Manager.Host)
    add_test(
        NAME ForgeConductor.Manager.ProcessRestartSignalTests
        COMMAND $<TARGET_FILE:ForgeConductor.Manager.ProcessRestartSignalTests>)
    set_tests_properties(
        ForgeConductor.Manager.ProcessRestartSignalTests PROPERTIES
        LABELS "G16;T-MGR;T-UNIT;T-STRESS"
        TIMEOUT 60)

    add_executable(ForgeConductor.Manager.MaintenanceWorkerTests
        tests/Manager/ManagerMaintenanceWorkerTests.cpp)
    forge_configure_standard_target(
        ForgeConductor.Manager.MaintenanceWorkerTests)
    target_link_libraries(
        ForgeConductor.Manager.MaintenanceWorkerTests PRIVATE
        ForgeConductor::Manager.Host)
    add_test(
        NAME ForgeConductor.Manager.MaintenanceWorkerTests
        COMMAND $<TARGET_FILE:ForgeConductor.Manager.MaintenanceWorkerTests>)
    set_tests_properties(
        ForgeConductor.Manager.MaintenanceWorkerTests PROPERTIES
        LABELS "G16;T-MGR;T-UNIT;T-STRESS"
        TIMEOUT 60)

    add_executable(ForgeConductor.Manager.ProcessWorkerGroupTests
        tests/Manager/ManagerProcessWorkerGroupTests.cpp)
    forge_configure_standard_target(
        ForgeConductor.Manager.ProcessWorkerGroupTests)
    target_link_libraries(
        ForgeConductor.Manager.ProcessWorkerGroupTests PRIVATE
        ForgeConductor::Manager.Host)
    add_test(
        NAME ForgeConductor.Manager.ProcessWorkerGroupTests
        COMMAND $<TARGET_FILE:ForgeConductor.Manager.ProcessWorkerGroupTests>)
    set_tests_properties(
        ForgeConductor.Manager.ProcessWorkerGroupTests PROPERTIES
        LABELS "G16;T-MGR;T-UNIT;T-STRESS"
        TIMEOUT 60)

    add_executable(ForgeConductor.Manager.TransitionWorkerTests
        tests/Manager/ManagerTransitionWorkerTests.cpp)
    forge_configure_standard_target(
        ForgeConductor.Manager.TransitionWorkerTests)
    target_link_libraries(
        ForgeConductor.Manager.TransitionWorkerTests PRIVATE
        ForgeConductor::Manager.Host)
    add_test(
        NAME ForgeConductor.Manager.TransitionWorkerTests
        COMMAND $<TARGET_FILE:ForgeConductor.Manager.TransitionWorkerTests>)
    set_tests_properties(
        ForgeConductor.Manager.TransitionWorkerTests PROPERTIES
        LABELS "G16;T-MGR;T-UNIT;T-STRESS"
        TIMEOUT 60)

    add_executable(
        ForgeConductor.Dashboard.WindowsListenerGenerationFactoryTests
        tests/Dashboard/WindowsDashboardListenerGenerationFactoryTests.cpp)
    forge_configure_native_target(
        ForgeConductor.Dashboard.WindowsListenerGenerationFactoryTests)
    target_include_directories(
        ForgeConductor.Dashboard.WindowsListenerGenerationFactoryTests PRIVATE
        "${PROJECT_SOURCE_DIR}/src")
    target_link_libraries(
        ForgeConductor.Dashboard.WindowsListenerGenerationFactoryTests PRIVATE
        ForgeConductor::Dashboard.Windows)
    add_test(
        NAME ForgeConductor.Dashboard.WindowsListenerGenerationFactoryTests
        COMMAND $<TARGET_FILE:ForgeConductor.Dashboard.WindowsListenerGenerationFactoryTests>)
    set_tests_properties(
        ForgeConductor.Dashboard.WindowsListenerGenerationFactoryTests
        PROPERTIES
        LABELS "G16;T-MGR;T-INTEGRATION;T-UNIT"
        TIMEOUT 60)

    add_executable(ForgeConductor.Dashboard.WindowsRuntimeTests
        tests/Dashboard/WindowsDashboardRuntimeTests.cpp)
    forge_configure_native_target(
        ForgeConductor.Dashboard.WindowsRuntimeTests)
    target_include_directories(
        ForgeConductor.Dashboard.WindowsRuntimeTests PRIVATE
        "${PROJECT_SOURCE_DIR}/src")
    target_link_libraries(
        ForgeConductor.Dashboard.WindowsRuntimeTests PRIVATE
        ForgeConductor::Dashboard.Windows)
    add_test(
        NAME ForgeConductor.Dashboard.WindowsRuntimeTests
        COMMAND $<TARGET_FILE:ForgeConductor.Dashboard.WindowsRuntimeTests>)
    set_tests_properties(
        ForgeConductor.Dashboard.WindowsRuntimeTests PROPERTIES
        LABELS "G16;T-MGR;T-INTEGRATION;T-STRESS"
        TIMEOUT 120)

    add_executable(ForgeConductor.Manager.WindowsRuntimeTests
        tests/Manager/WindowsManagerRuntimeTests.cpp)
    forge_configure_native_target(
        ForgeConductor.Manager.WindowsRuntimeTests)
    target_include_directories(
        ForgeConductor.Manager.WindowsRuntimeTests PRIVATE
        "${PROJECT_SOURCE_DIR}/src")
    target_link_libraries(
        ForgeConductor.Manager.WindowsRuntimeTests PRIVATE
        ForgeConductor::Manager.Windows)
    add_test(
        NAME ForgeConductor.Manager.WindowsRuntimeTests
        COMMAND $<TARGET_FILE:ForgeConductor.Manager.WindowsRuntimeTests>)
    set_tests_properties(
        ForgeConductor.Manager.WindowsRuntimeTests PROPERTIES
        LABELS "G16;T-MGR;T-INTEGRATION;T-STRESS"
        TIMEOUT 180)

    add_executable(ForgeConductor.Dashboard.PreparedExchangeTests
        tests/Dashboard/DashboardPreparedExchangeTests.cpp)
    forge_configure_standard_target(
        ForgeConductor.Dashboard.PreparedExchangeTests)
    target_link_libraries(
        ForgeConductor.Dashboard.PreparedExchangeTests PRIVATE
        ForgeConductor::Dashboard.Protocol)
    add_test(
        NAME ForgeConductor.Dashboard.PreparedExchangeTests
        COMMAND $<TARGET_FILE:ForgeConductor.Dashboard.PreparedExchangeTests>)
    set_tests_properties(
        ForgeConductor.Dashboard.PreparedExchangeTests PROPERTIES
        LABELS "G16;T-MGR;T-TEL;T-UNIT"
        TIMEOUT 60)

    add_executable(ForgeConductor.Dashboard.ApplicationErrorMapperTests
        tests/Dashboard/DashboardApplicationErrorMapperTests.cpp)
    forge_configure_standard_target(
        ForgeConductor.Dashboard.ApplicationErrorMapperTests)
    target_link_libraries(
        ForgeConductor.Dashboard.ApplicationErrorMapperTests PRIVATE
        ForgeConductor::Dashboard.Protocol)
    add_test(
        NAME ForgeConductor.Dashboard.ApplicationErrorMapperTests
        COMMAND $<TARGET_FILE:ForgeConductor.Dashboard.ApplicationErrorMapperTests>)
    set_tests_properties(
        ForgeConductor.Dashboard.ApplicationErrorMapperTests PROPERTIES
        LABELS "G16;T-MGR;T-PARITY;T-UNIT"
        TIMEOUT 60)

    add_executable(ForgeConductor.Dashboard.ApplicationJsonCodecTests
        tests/Dashboard/DashboardApplicationJsonCodecTests.cpp)
    forge_configure_standard_target(
        ForgeConductor.Dashboard.ApplicationJsonCodecTests)
    target_link_libraries(
        ForgeConductor.Dashboard.ApplicationJsonCodecTests PRIVATE
        ForgeConductor::Dashboard.Protocol
        nlohmann_json::nlohmann_json)
    add_test(
        NAME ForgeConductor.Dashboard.ApplicationJsonCodecTests
        COMMAND $<TARGET_FILE:ForgeConductor.Dashboard.ApplicationJsonCodecTests>)
    set_tests_properties(
        ForgeConductor.Dashboard.ApplicationJsonCodecTests PROPERTIES
        LABELS "G16;T-MGR;T-PARITY;T-SEC;T-TEL;T-UNIT"
        TIMEOUT 60)

    add_executable(ForgeConductor.Dashboard.SessionCloseRequestDecoderTests
        tests/Dashboard/DashboardSessionCloseRequestDecoderTests.cpp)
    forge_configure_standard_target(
        ForgeConductor.Dashboard.SessionCloseRequestDecoderTests)
    target_link_libraries(
        ForgeConductor.Dashboard.SessionCloseRequestDecoderTests PRIVATE
        ForgeConductor::Dashboard.Protocol)
    add_test(
        NAME ForgeConductor.Dashboard.SessionCloseRequestDecoderTests
        COMMAND
            $<TARGET_FILE:ForgeConductor.Dashboard.SessionCloseRequestDecoderTests>)
    set_tests_properties(
        ForgeConductor.Dashboard.SessionCloseRequestDecoderTests PROPERTIES
        LABELS "G16;T-MGR;T-PARITY;T-SEC;T-UNIT"
        TIMEOUT 60)

    add_executable(ForgeConductor.Dashboard.ConnectionApplicationTests
        tests/Application/DashboardConnectionApplicationTests.cpp)
    forge_configure_standard_target(
        ForgeConductor.Dashboard.ConnectionApplicationTests)
    target_link_libraries(
        ForgeConductor.Dashboard.ConnectionApplicationTests PRIVATE
        ForgeConductor::Application
        nlohmann_json::nlohmann_json)
    add_test(
        NAME ForgeConductor.Dashboard.ConnectionApplicationTests
        COMMAND $<TARGET_FILE:ForgeConductor.Dashboard.ConnectionApplicationTests>)
    set_tests_properties(
        ForgeConductor.Dashboard.ConnectionApplicationTests PROPERTIES
        LABELS "G16;T-MGR;T-PARITY;T-SEC;T-TEL;T-UNIT;T-STRESS"
        TIMEOUT 120)

    add_executable(ForgeConductor.Dashboard.ConnectionApplicationFactoryTests
        tests/Application/DashboardConnectionApplicationFactoryTests.cpp)
    forge_configure_standard_target(
        ForgeConductor.Dashboard.ConnectionApplicationFactoryTests)
    target_link_libraries(
        ForgeConductor.Dashboard.ConnectionApplicationFactoryTests PRIVATE
        ForgeConductor::Application)
    add_test(
        NAME ForgeConductor.Dashboard.ConnectionApplicationFactoryTests
        COMMAND $<TARGET_FILE:ForgeConductor.Dashboard.ConnectionApplicationFactoryTests>)
    set_tests_properties(
        ForgeConductor.Dashboard.ConnectionApplicationFactoryTests PROPERTIES
        LABELS "G16;T-MGR;T-PARITY;T-SEC;T-UNIT"
        TIMEOUT 60)

    add_executable(ForgeConductor.Dashboard.OperationalServiceTests
        tests/Application/DashboardOperationalServiceTests.cpp)
    forge_configure_standard_target(
        ForgeConductor.Dashboard.OperationalServiceTests)
    target_link_libraries(
        ForgeConductor.Dashboard.OperationalServiceTests PRIVATE
        ForgeConductor::Application)
    add_test(
        NAME ForgeConductor.Dashboard.OperationalServiceTests
        COMMAND $<TARGET_FILE:ForgeConductor.Dashboard.OperationalServiceTests>)
    set_tests_properties(
        ForgeConductor.Dashboard.OperationalServiceTests PROPERTIES
        LABELS "G16;T-MGR;T-PARITY;T-UNIT;T-STRESS"
        TIMEOUT 120)

    add_executable(ForgeConductor.Dashboard.TelemetrySourceTests
        tests/Application/DashboardTelemetrySourceTests.cpp)
    forge_configure_standard_target(
        ForgeConductor.Dashboard.TelemetrySourceTests)
    target_link_libraries(
        ForgeConductor.Dashboard.TelemetrySourceTests PRIVATE
        ForgeConductor::Application)
    add_test(
        NAME ForgeConductor.Dashboard.TelemetrySourceTests
        COMMAND $<TARGET_FILE:ForgeConductor.Dashboard.TelemetrySourceTests>)
    set_tests_properties(
        ForgeConductor.Dashboard.TelemetrySourceTests PROPERTIES
        LABELS "G16;T-MGR;T-PARITY;T-TEL;T-UNIT;T-STRESS"
        TIMEOUT 120)

    add_executable(ForgeConductor.Dashboard.ResponseComposerTests
        tests/Dashboard/DashboardResponseComposerTests.cpp)
    forge_configure_standard_target(
        ForgeConductor.Dashboard.ResponseComposerTests)
    target_link_libraries(
        ForgeConductor.Dashboard.ResponseComposerTests PRIVATE
        ForgeConductor::Dashboard.Protocol
        nlohmann_json::nlohmann_json)
    add_test(
        NAME ForgeConductor.Dashboard.ResponseComposerTests
        COMMAND $<TARGET_FILE:ForgeConductor.Dashboard.ResponseComposerTests>)
    set_tests_properties(
        ForgeConductor.Dashboard.ResponseComposerTests PROPERTIES
        LABELS "G16;T-MGR;T-PARITY;T-SEC;T-TEL;T-UNIT"
        TIMEOUT 60)

    add_executable(ForgeConductor.Dashboard.SseBroadcasterTests
        tests/Dashboard/DashboardSseBroadcasterTests.cpp)
    forge_configure_standard_target(
        ForgeConductor.Dashboard.SseBroadcasterTests)
    target_link_libraries(
        ForgeConductor.Dashboard.SseBroadcasterTests PRIVATE
        ForgeConductor::Dashboard.Protocol)
    add_test(
        NAME ForgeConductor.Dashboard.SseBroadcasterTests
        COMMAND $<TARGET_FILE:ForgeConductor.Dashboard.SseBroadcasterTests>)
    set_tests_properties(
        ForgeConductor.Dashboard.SseBroadcasterTests PROPERTIES
        LABELS "G16;T-MGR;T-TEL;T-UNIT;T-STRESS"
        TIMEOUT 120)

    add_executable(ForgeConductor.Dashboard.StaticAssetStoreTests
        tests/Dashboard/DashboardStaticAssetStoreTests.cpp)
    forge_configure_standard_target(
        ForgeConductor.Dashboard.StaticAssetStoreTests)
    target_link_libraries(
        ForgeConductor.Dashboard.StaticAssetStoreTests PRIVATE
        ForgeConductor::Dashboard.Protocol)
    add_test(
        NAME ForgeConductor.Dashboard.StaticAssetStoreTests
        COMMAND $<TARGET_FILE:ForgeConductor.Dashboard.StaticAssetStoreTests>)
    set_tests_properties(
        ForgeConductor.Dashboard.StaticAssetStoreTests PROPERTIES
        LABELS "G16;T-MGR;T-PARITY;T-UNIT;T-STRESS"
        TIMEOUT 120)

    add_executable(ForgeConductor.Dashboard.WindowsStaticAssetBundleTests
        tests/Dashboard/WindowsDashboardStaticAssetBundleTests.cpp
        src/Hosts/Manager/Resources/Dashboard/DashboardAssets.rc)
    forge_configure_native_target(
        ForgeConductor.Dashboard.WindowsStaticAssetBundleTests)
    target_link_libraries(
        ForgeConductor.Dashboard.WindowsStaticAssetBundleTests PRIVATE
        ForgeConductor::Dashboard.Windows)
    add_test(
        NAME ForgeConductor.Dashboard.WindowsStaticAssetBundleTests
        COMMAND
            $<TARGET_FILE:ForgeConductor.Dashboard.WindowsStaticAssetBundleTests>)
    set_tests_properties(
        ForgeConductor.Dashboard.WindowsStaticAssetBundleTests PROPERTIES
        LABELS "G16;T-MGR;T-PARITY;T-UNIT"
        TIMEOUT 60)

    add_executable(ForgeConductor.Dashboard.StaticShellContractTests
        tests/Dashboard/DashboardStaticShellContractTests.cpp
        src/Hosts/Manager/Resources/Dashboard/DashboardAssets.rc)
    forge_configure_native_target(
        ForgeConductor.Dashboard.StaticShellContractTests)
    target_link_libraries(
        ForgeConductor.Dashboard.StaticShellContractTests PRIVATE
        ForgeConductor::Dashboard.Windows)
    add_test(
        NAME ForgeConductor.Dashboard.StaticShellContractTests
        COMMAND $<TARGET_FILE:ForgeConductor.Dashboard.StaticShellContractTests>)
    set_tests_properties(
        ForgeConductor.Dashboard.StaticShellContractTests PROPERTIES
        LABELS "G16;T-MGR;T-PARITY;T-UNIT"
        TIMEOUT 60)

    add_executable(ForgeConductor.Dashboard.AdmissionControllerTests
        tests/Dashboard/DashboardAdmissionControllerTests.cpp)
    forge_configure_native_target(
        ForgeConductor.Dashboard.AdmissionControllerTests)
    target_include_directories(
        ForgeConductor.Dashboard.AdmissionControllerTests PRIVATE
        "${PROJECT_SOURCE_DIR}/src")
    target_link_libraries(
        ForgeConductor.Dashboard.AdmissionControllerTests PRIVATE
        ForgeConductor::Dashboard.Windows)
    add_test(
        NAME ForgeConductor.Dashboard.AdmissionControllerTests
        COMMAND $<TARGET_FILE:ForgeConductor.Dashboard.AdmissionControllerTests>)
    set_tests_properties(
        ForgeConductor.Dashboard.AdmissionControllerTests PROPERTIES
        LABELS "G16;T-MGR;T-SEC;T-STRESS;T-UNIT"
        TIMEOUT 120)

    add_executable(ForgeConductor.Dashboard.DeadlineNotificationMailboxTests
        tests/Dashboard/DashboardDeadlineNotificationMailboxTests.cpp)
    forge_configure_native_target(
        ForgeConductor.Dashboard.DeadlineNotificationMailboxTests)
    target_include_directories(
        ForgeConductor.Dashboard.DeadlineNotificationMailboxTests PRIVATE
        "${PROJECT_SOURCE_DIR}/src")
    target_link_libraries(
        ForgeConductor.Dashboard.DeadlineNotificationMailboxTests PRIVATE
        ForgeConductor::Dashboard.Windows)
    add_test(
        NAME ForgeConductor.Dashboard.DeadlineNotificationMailboxTests
        COMMAND
            $<TARGET_FILE:ForgeConductor.Dashboard.DeadlineNotificationMailboxTests>)
    set_tests_properties(
        ForgeConductor.Dashboard.DeadlineNotificationMailboxTests PROPERTIES
        LABELS "G16;T-MGR;T-STRESS;T-UNIT"
        TIMEOUT 120)

    add_executable(ForgeConductor.Dashboard.DeadlineIocpBridgeTests
        tests/Dashboard/DashboardDeadlineIocpBridgeTests.cpp)
    forge_configure_native_target(
        ForgeConductor.Dashboard.DeadlineIocpBridgeTests)
    target_include_directories(
        ForgeConductor.Dashboard.DeadlineIocpBridgeTests PRIVATE
        "${PROJECT_SOURCE_DIR}/src")
    target_link_libraries(
        ForgeConductor.Dashboard.DeadlineIocpBridgeTests PRIVATE
        ForgeConductor::Dashboard.Windows)
    add_test(
        NAME ForgeConductor.Dashboard.DeadlineIocpBridgeTests
        COMMAND
            $<TARGET_FILE:ForgeConductor.Dashboard.DeadlineIocpBridgeTests>)
    set_tests_properties(
        ForgeConductor.Dashboard.DeadlineIocpBridgeTests PROPERTIES
        LABELS "G16;T-MGR;T-STRESS;T-UNIT"
        TIMEOUT 120)

    add_executable(ForgeConductor.Dashboard.LoopbackEndpointTests
        tests/Dashboard/DashboardLoopbackEndpointTests.cpp)
    forge_configure_native_target(
        ForgeConductor.Dashboard.LoopbackEndpointTests)
    target_include_directories(
        ForgeConductor.Dashboard.LoopbackEndpointTests PRIVATE
        "${PROJECT_SOURCE_DIR}/src")
    target_link_libraries(
        ForgeConductor.Dashboard.LoopbackEndpointTests PRIVATE
        ForgeConductor::Dashboard.Windows)
    add_test(
        NAME ForgeConductor.Dashboard.LoopbackEndpointTests
        COMMAND $<TARGET_FILE:ForgeConductor.Dashboard.LoopbackEndpointTests>)
    set_tests_properties(
        ForgeConductor.Dashboard.LoopbackEndpointTests PROPERTIES
        LABELS "G16;T-MGR;T-SEC;T-STRESS;T-UNIT"
        TIMEOUT 120)

    add_executable(ForgeConductor.Dashboard.WinsockRuntimeTests
        tests/Dashboard/DashboardWinsockRuntimeTests.cpp)
    forge_configure_native_target(
        ForgeConductor.Dashboard.WinsockRuntimeTests)
    target_include_directories(
        ForgeConductor.Dashboard.WinsockRuntimeTests PRIVATE
        "${PROJECT_SOURCE_DIR}/src")
    target_link_libraries(
        ForgeConductor.Dashboard.WinsockRuntimeTests PRIVATE
        ForgeConductor::Dashboard.Windows)
    add_test(
        NAME ForgeConductor.Dashboard.WinsockRuntimeTests
        COMMAND $<TARGET_FILE:ForgeConductor.Dashboard.WinsockRuntimeTests>)
    set_tests_properties(
        ForgeConductor.Dashboard.WinsockRuntimeTests PROPERTIES
        LABELS "G16;T-MGR;T-STRESS;T-UNIT"
        TIMEOUT 120)

    add_executable(ForgeConductor.Dashboard.IoCompletionPortTests
        tests/Dashboard/DashboardIoCompletionPortTests.cpp)
    forge_configure_native_target(
        ForgeConductor.Dashboard.IoCompletionPortTests)
    target_include_directories(
        ForgeConductor.Dashboard.IoCompletionPortTests PRIVATE
        "${PROJECT_SOURCE_DIR}/src")
    target_link_libraries(
        ForgeConductor.Dashboard.IoCompletionPortTests PRIVATE
        ForgeConductor::Dashboard.Windows)
    add_test(
        NAME ForgeConductor.Dashboard.IoCompletionPortTests
        COMMAND $<TARGET_FILE:ForgeConductor.Dashboard.IoCompletionPortTests>)
    set_tests_properties(
        ForgeConductor.Dashboard.IoCompletionPortTests PROPERTIES
        LABELS "G16;T-MGR;T-STRESS;T-UNIT"
        TIMEOUT 120)

    add_executable(ForgeConductor.Dashboard.IocpWorkerKernelTests
        tests/Dashboard/DashboardIocpWorkerKernelTests.cpp)
    forge_configure_native_target(
        ForgeConductor.Dashboard.IocpWorkerKernelTests)
    target_include_directories(
        ForgeConductor.Dashboard.IocpWorkerKernelTests PRIVATE
        "${PROJECT_SOURCE_DIR}/src")
    target_link_libraries(
        ForgeConductor.Dashboard.IocpWorkerKernelTests PRIVATE
        ForgeConductor::Dashboard.Windows)
    add_test(
        NAME ForgeConductor.Dashboard.IocpWorkerKernelTests
        COMMAND $<TARGET_FILE:ForgeConductor.Dashboard.IocpWorkerKernelTests>)
    set_tests_properties(
        ForgeConductor.Dashboard.IocpWorkerKernelTests PROPERTIES
        LABELS "G16;T-MGR;T-STRESS;T-UNIT"
        TIMEOUT 120)

    add_executable(ForgeConductor.Dashboard.IocpCompletionRouterTests
        tests/Dashboard/DashboardIocpCompletionRouterTests.cpp)
    forge_configure_native_target(
        ForgeConductor.Dashboard.IocpCompletionRouterTests)
    target_include_directories(
        ForgeConductor.Dashboard.IocpCompletionRouterTests PRIVATE
        "${PROJECT_SOURCE_DIR}/src")
    target_link_libraries(
        ForgeConductor.Dashboard.IocpCompletionRouterTests PRIVATE
        ForgeConductor::Dashboard.Windows)
    add_test(
        NAME ForgeConductor.Dashboard.IocpCompletionRouterTests
        COMMAND $<TARGET_FILE:ForgeConductor.Dashboard.IocpCompletionRouterTests>)
    set_tests_properties(
        ForgeConductor.Dashboard.IocpCompletionRouterTests PROPERTIES
        LABELS "G16;T-MGR;T-STRESS;T-UNIT"
        TIMEOUT 120)

    add_executable(ForgeConductor.Dashboard.ListeningSocketTests
        tests/Dashboard/DashboardListeningSocketTests.cpp)
    forge_configure_native_target(
        ForgeConductor.Dashboard.ListeningSocketTests)
    target_include_directories(
        ForgeConductor.Dashboard.ListeningSocketTests PRIVATE
        "${PROJECT_SOURCE_DIR}/src")
    target_link_libraries(
        ForgeConductor.Dashboard.ListeningSocketTests PRIVATE
        ForgeConductor::Dashboard.Windows)
    add_test(
        NAME ForgeConductor.Dashboard.ListeningSocketTests
        COMMAND $<TARGET_FILE:ForgeConductor.Dashboard.ListeningSocketTests>)
    set_tests_properties(
        ForgeConductor.Dashboard.ListeningSocketTests PROPERTIES
        LABELS "G16;T-MGR;T-SEC;T-STRESS;T-UNIT"
        TIMEOUT 120)

    add_executable(ForgeConductor.Dashboard.WinsockExtensionsTests
        tests/Dashboard/DashboardWinsockExtensionsTests.cpp)
    forge_configure_native_target(
        ForgeConductor.Dashboard.WinsockExtensionsTests)
    target_include_directories(
        ForgeConductor.Dashboard.WinsockExtensionsTests PRIVATE
        "${PROJECT_SOURCE_DIR}/src")
    target_link_libraries(
        ForgeConductor.Dashboard.WinsockExtensionsTests PRIVATE
        ForgeConductor::Dashboard.Windows)
    add_test(
        NAME ForgeConductor.Dashboard.WinsockExtensionsTests
        COMMAND $<TARGET_FILE:ForgeConductor.Dashboard.WinsockExtensionsTests>)
    set_tests_properties(
        ForgeConductor.Dashboard.WinsockExtensionsTests PROPERTIES
        LABELS "G16;T-MGR;T-SEC;T-STRESS;T-UNIT"
        TIMEOUT 120)

    add_executable(ForgeConductor.Dashboard.AcceptSlotTests
        tests/Dashboard/DashboardAcceptSlotTests.cpp)
    forge_configure_native_target(
        ForgeConductor.Dashboard.AcceptSlotTests)
    target_include_directories(
        ForgeConductor.Dashboard.AcceptSlotTests PRIVATE
        "${PROJECT_SOURCE_DIR}/src")
    target_link_libraries(
        ForgeConductor.Dashboard.AcceptSlotTests PRIVATE
        ForgeConductor::Dashboard.Windows)
    add_test(
        NAME ForgeConductor.Dashboard.AcceptSlotTests
        COMMAND $<TARGET_FILE:ForgeConductor.Dashboard.AcceptSlotTests>)
    set_tests_properties(
        ForgeConductor.Dashboard.AcceptSlotTests PROPERTIES
        LABELS "G16;T-MGR;T-SEC;T-STRESS;T-UNIT"
        TIMEOUT 120)

    add_executable(ForgeConductor.Dashboard.AcceptSlotSetTests
        tests/Dashboard/DashboardAcceptSlotSetTests.cpp)
    forge_configure_native_target(
        ForgeConductor.Dashboard.AcceptSlotSetTests)
    target_include_directories(
        ForgeConductor.Dashboard.AcceptSlotSetTests PRIVATE
        "${PROJECT_SOURCE_DIR}/src")
    target_link_libraries(
        ForgeConductor.Dashboard.AcceptSlotSetTests PRIVATE
        ForgeConductor::Dashboard.Windows)
    add_test(
        NAME ForgeConductor.Dashboard.AcceptSlotSetTests
        COMMAND $<TARGET_FILE:ForgeConductor.Dashboard.AcceptSlotSetTests>)
    set_tests_properties(
        ForgeConductor.Dashboard.AcceptSlotSetTests PROPERTIES
        LABELS "G16;T-MGR;T-SEC;T-STRESS;T-UNIT"
        TIMEOUT 120)

    add_executable(ForgeConductor.Dashboard.AcceptedConnectionHandoffTests
        tests/Dashboard/DashboardAcceptedConnectionHandoffTests.cpp)
    forge_configure_native_target(
        ForgeConductor.Dashboard.AcceptedConnectionHandoffTests)
    target_include_directories(
        ForgeConductor.Dashboard.AcceptedConnectionHandoffTests PRIVATE
        "${PROJECT_SOURCE_DIR}/src")
    target_link_libraries(
        ForgeConductor.Dashboard.AcceptedConnectionHandoffTests PRIVATE
        ForgeConductor::Dashboard.Windows)
    add_test(
        NAME ForgeConductor.Dashboard.AcceptedConnectionHandoffTests
        COMMAND $<TARGET_FILE:ForgeConductor.Dashboard.AcceptedConnectionHandoffTests>)
    set_tests_properties(
        ForgeConductor.Dashboard.AcceptedConnectionHandoffTests PROPERTIES
        LABELS "G16;T-MGR;T-SEC;T-STRESS;T-UNIT"
        TIMEOUT 120)

    add_executable(ForgeConductor.Dashboard.OverloadResponderSetTests
        tests/Dashboard/DashboardOverloadResponderSetTests.cpp)
    forge_configure_native_target(
        ForgeConductor.Dashboard.OverloadResponderSetTests)
    target_include_directories(
        ForgeConductor.Dashboard.OverloadResponderSetTests PRIVATE
        "${PROJECT_SOURCE_DIR}/src")
    target_link_libraries(
        ForgeConductor.Dashboard.OverloadResponderSetTests PRIVATE
        ForgeConductor::Dashboard.Windows)
    add_test(
        NAME ForgeConductor.Dashboard.OverloadResponderSetTests
        COMMAND $<TARGET_FILE:ForgeConductor.Dashboard.OverloadResponderSetTests>)
    set_tests_properties(
        ForgeConductor.Dashboard.OverloadResponderSetTests PROPERTIES
        LABELS "G16;T-MGR;T-SEC;T-STRESS;T-UNIT"
        TIMEOUT 120)

    add_executable(
        ForgeConductor.Dashboard.ListenerCompletionKeyLeaseTests
        tests/Dashboard/DashboardListenerCompletionKeyLeaseTests.cpp)
    forge_configure_native_target(
        ForgeConductor.Dashboard.ListenerCompletionKeyLeaseTests)
    target_include_directories(
        ForgeConductor.Dashboard.ListenerCompletionKeyLeaseTests PRIVATE
        "${PROJECT_SOURCE_DIR}/src")
    target_link_libraries(
        ForgeConductor.Dashboard.ListenerCompletionKeyLeaseTests PRIVATE
        ForgeConductor::Dashboard.Windows)
    add_test(
        NAME ForgeConductor.Dashboard.ListenerCompletionKeyLeaseTests
        COMMAND
            $<TARGET_FILE:ForgeConductor.Dashboard.ListenerCompletionKeyLeaseTests>)
    set_tests_properties(
        ForgeConductor.Dashboard.ListenerCompletionKeyLeaseTests PROPERTIES
        LABELS "G16;T-MGR;T-SEC;T-STRESS;T-UNIT"
        TIMEOUT 120)

    add_executable(ForgeConductor.Dashboard.ListenerGenerationTests
        tests/Dashboard/DashboardListenerGenerationTests.cpp)
    forge_configure_native_target(
        ForgeConductor.Dashboard.ListenerGenerationTests)
    target_include_directories(
        ForgeConductor.Dashboard.ListenerGenerationTests PRIVATE
        "${PROJECT_SOURCE_DIR}/src")
    target_link_libraries(
        ForgeConductor.Dashboard.ListenerGenerationTests PRIVATE
        ForgeConductor::Dashboard.Windows)
    add_test(
        NAME ForgeConductor.Dashboard.ListenerGenerationTests
        COMMAND $<TARGET_FILE:ForgeConductor.Dashboard.ListenerGenerationTests>)
    set_tests_properties(
        ForgeConductor.Dashboard.ListenerGenerationTests PROPERTIES
        LABELS "G16;T-MGR;T-SEC;T-STRESS;T-UNIT"
        TIMEOUT 120)

    add_executable(ForgeConductor.Dashboard.ConnectionSocketTests
        tests/Dashboard/DashboardConnectionSocketTests.cpp)
    forge_configure_native_target(
        ForgeConductor.Dashboard.ConnectionSocketTests)
    target_include_directories(
        ForgeConductor.Dashboard.ConnectionSocketTests PRIVATE
        "${PROJECT_SOURCE_DIR}/src")
    target_link_libraries(
        ForgeConductor.Dashboard.ConnectionSocketTests PRIVATE
        ForgeConductor::Dashboard.Windows)
    add_test(
        NAME ForgeConductor.Dashboard.ConnectionSocketTests
        COMMAND $<TARGET_FILE:ForgeConductor.Dashboard.ConnectionSocketTests>)
    set_tests_properties(
        ForgeConductor.Dashboard.ConnectionSocketTests PROPERTIES
        LABELS "G16;T-MGR;T-SEC;T-STRESS;T-UNIT"
        TIMEOUT 120)

    add_executable(ForgeConductor.Dashboard.ConnectionEventBridgeTests
        tests/Dashboard/DashboardConnectionEventBridgeTests.cpp)
    forge_configure_native_target(
        ForgeConductor.Dashboard.ConnectionEventBridgeTests)
    target_include_directories(
        ForgeConductor.Dashboard.ConnectionEventBridgeTests PRIVATE
        "${PROJECT_SOURCE_DIR}/src")
    target_link_libraries(
        ForgeConductor.Dashboard.ConnectionEventBridgeTests PRIVATE
        ForgeConductor::Dashboard.Windows)
    add_test(
        NAME ForgeConductor.Dashboard.ConnectionEventBridgeTests
        COMMAND
            $<TARGET_FILE:ForgeConductor.Dashboard.ConnectionEventBridgeTests>)
    set_tests_properties(
        ForgeConductor.Dashboard.ConnectionEventBridgeTests PROPERTIES
        LABELS "G16;T-MGR;T-STRESS;T-UNIT"
        TIMEOUT 120)

    add_executable(ForgeConductor.Dashboard.ConnectionRuntimeServicesTests
        tests/Dashboard/DashboardConnectionRuntimeServicesTests.cpp)
    forge_configure_native_target(
        ForgeConductor.Dashboard.ConnectionRuntimeServicesTests)
    target_include_directories(
        ForgeConductor.Dashboard.ConnectionRuntimeServicesTests PRIVATE
        "${PROJECT_SOURCE_DIR}/src")
    target_link_libraries(
        ForgeConductor.Dashboard.ConnectionRuntimeServicesTests PRIVATE
        ForgeConductor::Dashboard.Windows)
    add_test(
        NAME ForgeConductor.Dashboard.ConnectionRuntimeServicesTests
        COMMAND
            $<TARGET_FILE:ForgeConductor.Dashboard.ConnectionRuntimeServicesTests>)
    set_tests_properties(
        ForgeConductor.Dashboard.ConnectionRuntimeServicesTests PROPERTIES
        LABELS "G16;T-MGR;T-SEC;T-STRESS;T-UNIT"
        TIMEOUT 120)

    add_executable(ForgeConductor.Dashboard.FixedIocpKeyAuthorityTests
        tests/Dashboard/DashboardFixedIocpKeyAuthorityTests.cpp)
    forge_configure_native_target(
        ForgeConductor.Dashboard.FixedIocpKeyAuthorityTests)
    target_include_directories(
        ForgeConductor.Dashboard.FixedIocpKeyAuthorityTests PRIVATE
        "${PROJECT_SOURCE_DIR}/src")
    target_link_libraries(
        ForgeConductor.Dashboard.FixedIocpKeyAuthorityTests PRIVATE
        ForgeConductor::Dashboard.Windows)
    add_test(
        NAME ForgeConductor.Dashboard.FixedIocpKeyAuthorityTests
        COMMAND
            $<TARGET_FILE:ForgeConductor.Dashboard.FixedIocpKeyAuthorityTests>)
    set_tests_properties(
        ForgeConductor.Dashboard.FixedIocpKeyAuthorityTests PROPERTIES
        LABELS "G16;T-MGR;T-SEC;T-STRESS;T-UNIT"
        TIMEOUT 120)

    add_executable(ForgeConductor.Dashboard.HandlerOperationsTests
        tests/Dashboard/DashboardHandlerOperationsTests.cpp)
    forge_configure_native_target(
        ForgeConductor.Dashboard.HandlerOperationsTests)
    target_include_directories(
        ForgeConductor.Dashboard.HandlerOperationsTests PRIVATE
        "${PROJECT_SOURCE_DIR}/src")
    target_link_libraries(
        ForgeConductor.Dashboard.HandlerOperationsTests PRIVATE
        ForgeConductor::Dashboard.Windows)
    add_test(
        NAME ForgeConductor.Dashboard.HandlerOperationsTests
        COMMAND
            $<TARGET_FILE:ForgeConductor.Dashboard.HandlerOperationsTests>)
    set_tests_properties(
        ForgeConductor.Dashboard.HandlerOperationsTests PROPERTIES
        LABELS "G16;T-MGR;T-STRESS;T-UNIT"
        TIMEOUT 120)

    add_executable(ForgeConductor.Dashboard.ConnectionResponseCatalogTests
        tests/Dashboard/DashboardConnectionResponseCatalogTests.cpp)
    forge_configure_native_target(
        ForgeConductor.Dashboard.ConnectionResponseCatalogTests)
    target_include_directories(
        ForgeConductor.Dashboard.ConnectionResponseCatalogTests PRIVATE
        "${PROJECT_SOURCE_DIR}/src")
    target_link_libraries(
        ForgeConductor.Dashboard.ConnectionResponseCatalogTests PRIVATE
        ForgeConductor::Dashboard.Windows)
    add_test(
        NAME ForgeConductor.Dashboard.ConnectionResponseCatalogTests
        COMMAND
            $<TARGET_FILE:ForgeConductor.Dashboard.ConnectionResponseCatalogTests>)
    set_tests_properties(
        ForgeConductor.Dashboard.ConnectionResponseCatalogTests PROPERTIES
        LABELS "G16;T-MGR;T-STRESS;T-UNIT"
        TIMEOUT 120)

    add_executable(ForgeConductor.Dashboard.ConnectionStateTests
        tests/Dashboard/DashboardConnectionStateTests.cpp)
    forge_configure_native_target(
        ForgeConductor.Dashboard.ConnectionStateTests)
    target_include_directories(
        ForgeConductor.Dashboard.ConnectionStateTests PRIVATE
        "${PROJECT_SOURCE_DIR}/src")
    target_link_libraries(
        ForgeConductor.Dashboard.ConnectionStateTests PRIVATE
        ForgeConductor::Dashboard.Windows)
    add_test(
        NAME ForgeConductor.Dashboard.ConnectionStateTests
        COMMAND $<TARGET_FILE:ForgeConductor.Dashboard.ConnectionStateTests>)
    set_tests_properties(
        ForgeConductor.Dashboard.ConnectionStateTests PROPERTIES
        LABELS "G16;T-MGR;T-SEC;T-STRESS;T-UNIT"
        TIMEOUT 120)

    add_executable(ForgeConductor.Dashboard.ConnectionRegistryTests
        tests/Dashboard/DashboardConnectionRegistryTests.cpp)
    forge_configure_native_target(
        ForgeConductor.Dashboard.ConnectionRegistryTests)
    target_include_directories(
        ForgeConductor.Dashboard.ConnectionRegistryTests PRIVATE
        "${PROJECT_SOURCE_DIR}/src")
    target_link_libraries(
        ForgeConductor.Dashboard.ConnectionRegistryTests PRIVATE
        ForgeConductor::Dashboard.Windows)
    add_test(
        NAME ForgeConductor.Dashboard.ConnectionRegistryTests
        COMMAND $<TARGET_FILE:ForgeConductor.Dashboard.ConnectionRegistryTests>)
    set_tests_properties(
        ForgeConductor.Dashboard.ConnectionRegistryTests PROPERTIES
        LABELS "G16;T-MGR;T-SEC;T-STRESS;T-UNIT"
        TIMEOUT 120)

    add_executable(ForgeConductor.Dashboard.ShutdownDrainTests
        tests/Dashboard/DashboardShutdownDrainTests.cpp)
    forge_configure_native_target(
        ForgeConductor.Dashboard.ShutdownDrainTests)
    target_include_directories(
        ForgeConductor.Dashboard.ShutdownDrainTests PRIVATE
        "${PROJECT_SOURCE_DIR}/src")
    target_link_libraries(
        ForgeConductor.Dashboard.ShutdownDrainTests PRIVATE
        ForgeConductor::Dashboard.Windows)
    add_test(
        NAME ForgeConductor.Dashboard.ShutdownDrainTests
        COMMAND $<TARGET_FILE:ForgeConductor.Dashboard.ShutdownDrainTests>)
    set_tests_properties(
        ForgeConductor.Dashboard.ShutdownDrainTests PROPERTIES
        LABELS "G16;T-MGR;T-STRESS;T-UNIT"
        TIMEOUT 120)

    add_executable(
        ForgeConductor.Dashboard.ShutdownDrainTombstoneIntegrationTests
        tests/Dashboard/DashboardShutdownDrainTombstoneIntegrationTests.cpp)
    forge_configure_native_target(
        ForgeConductor.Dashboard.ShutdownDrainTombstoneIntegrationTests)
    target_include_directories(
        ForgeConductor.Dashboard.ShutdownDrainTombstoneIntegrationTests PRIVATE
        "${PROJECT_SOURCE_DIR}/src")
    target_link_libraries(
        ForgeConductor.Dashboard.ShutdownDrainTombstoneIntegrationTests PRIVATE
        ForgeConductor::Dashboard.Windows)
    add_test(
        NAME
            ForgeConductor.Dashboard.ShutdownDrainTombstoneIntegrationTests
        COMMAND
            $<TARGET_FILE:ForgeConductor.Dashboard.ShutdownDrainTombstoneIntegrationTests>)
    set_tests_properties(
        ForgeConductor.Dashboard.ShutdownDrainTombstoneIntegrationTests
        PROPERTIES
        LABELS "G16;T-MGR;T-STRESS;T-UNIT"
        TIMEOUT 120)

    add_executable(ForgeConductor.Dashboard.DeadlineSchedulerTests
        tests/Dashboard/WindowsDashboardDeadlineSchedulerTests.cpp)
    forge_configure_native_target(
        ForgeConductor.Dashboard.DeadlineSchedulerTests)
    target_link_libraries(
        ForgeConductor.Dashboard.DeadlineSchedulerTests PRIVATE
        ForgeConductor::Dashboard.Windows
        ForgeConductor::Infrastructure.Windows)
    add_test(
        NAME ForgeConductor.Dashboard.DeadlineSchedulerTests
        COMMAND $<TARGET_FILE:ForgeConductor.Dashboard.DeadlineSchedulerTests>)
    set_tests_properties(
        ForgeConductor.Dashboard.DeadlineSchedulerTests PROPERTIES
        LABELS "G16;T-MGR;T-STRESS;T-UNIT"
        TIMEOUT 60)

    add_executable(ForgeConductor.Dashboard.HandlerExecutorTests
        tests/Dashboard/WindowsDashboardHandlerExecutorTests.cpp)
    forge_configure_native_target(
        ForgeConductor.Dashboard.HandlerExecutorTests)
    target_link_libraries(
        ForgeConductor.Dashboard.HandlerExecutorTests PRIVATE
        ForgeConductor::Dashboard.Windows)
    add_test(
        NAME ForgeConductor.Dashboard.HandlerExecutorTests
        COMMAND $<TARGET_FILE:ForgeConductor.Dashboard.HandlerExecutorTests>)
    set_tests_properties(
        ForgeConductor.Dashboard.HandlerExecutorTests PROPERTIES
        LABELS "G16;T-MGR;T-STRESS;T-UNIT"
        TIMEOUT 120)

    add_executable(ForgeConductor.Dashboard.HttpParserTests
        tests/Dashboard/DashboardHttpParserTests.cpp)
    forge_configure_standard_target(ForgeConductor.Dashboard.HttpParserTests)
    target_link_libraries(ForgeConductor.Dashboard.HttpParserTests PRIVATE
        ForgeConductor::Dashboard.Protocol)
    add_test(
        NAME ForgeConductor.Dashboard.HttpParserTests
        COMMAND $<TARGET_FILE:ForgeConductor.Dashboard.HttpParserTests>)
    set_tests_properties(ForgeConductor.Dashboard.HttpParserTests PROPERTIES
        LABELS "G16;T-MGR;T-SEC;T-UNIT"
        TIMEOUT 60)

    add_executable(ForgeConductor.Dashboard.HttpParserSessionTests
        tests/Dashboard/DashboardHttpParserSessionTests.cpp)
    forge_configure_standard_target(
        ForgeConductor.Dashboard.HttpParserSessionTests)
    target_link_libraries(
        ForgeConductor.Dashboard.HttpParserSessionTests PRIVATE
        ForgeConductor::Dashboard.Protocol)
    add_test(
        NAME ForgeConductor.Dashboard.HttpParserSessionTests
        COMMAND $<TARGET_FILE:ForgeConductor.Dashboard.HttpParserSessionTests>)
    set_tests_properties(
        ForgeConductor.Dashboard.HttpParserSessionTests PROPERTIES
        LABELS "G16;T-MGR;T-SEC;T-STRESS;T-UNIT"
        TIMEOUT 120)

    add_executable(ForgeConductor.Dashboard.RequestPolicyTests
        tests/Dashboard/DashboardRequestPolicyTests.cpp)
    forge_configure_standard_target(ForgeConductor.Dashboard.RequestPolicyTests)
    target_link_libraries(ForgeConductor.Dashboard.RequestPolicyTests PRIVATE
        ForgeConductor::Dashboard.Protocol)
    add_test(
        NAME ForgeConductor.Dashboard.RequestPolicyTests
        COMMAND $<TARGET_FILE:ForgeConductor.Dashboard.RequestPolicyTests>)
    set_tests_properties(ForgeConductor.Dashboard.RequestPolicyTests PROPERTIES
        LABELS "G16;T-MGR;T-SEC;T-UNIT"
        TIMEOUT 60)

    add_executable(ForgeConductor.Dashboard.WindowsBearerTokenTests
        tests/Dashboard/WindowsDashboardBearerTokenTests.cpp)
    forge_configure_native_target(
        ForgeConductor.Dashboard.WindowsBearerTokenTests)
    target_link_libraries(
        ForgeConductor.Dashboard.WindowsBearerTokenTests PRIVATE
        ForgeConductor::Infrastructure.Windows)
    add_test(
        NAME ForgeConductor.Dashboard.WindowsBearerTokenTests
        COMMAND $<TARGET_FILE:ForgeConductor.Dashboard.WindowsBearerTokenTests>)
    set_tests_properties(
        ForgeConductor.Dashboard.WindowsBearerTokenTests PROPERTIES
        LABELS "G16;T-MGR;T-SEC;T-UNIT;T-STRESS"
        TIMEOUT 60)

    add_executable(ForgeConductor.Dashboard.RouteCatalogTests
        tests/Dashboard/DashboardRouteCatalogTests.cpp)
    forge_configure_standard_target(ForgeConductor.Dashboard.RouteCatalogTests)
    target_link_libraries(ForgeConductor.Dashboard.RouteCatalogTests PRIVATE
        ForgeConductor::Dashboard.Protocol)
    add_test(
        NAME ForgeConductor.Dashboard.RouteCatalogTests
        COMMAND $<TARGET_FILE:ForgeConductor.Dashboard.RouteCatalogTests>)
    set_tests_properties(ForgeConductor.Dashboard.RouteCatalogTests PROPERTIES
        LABELS "G16;T-MGR;T-PARITY;T-SEC;T-UNIT"
        TIMEOUT 60)

    add_executable(ForgeConductor.Dashboard.ManagerJsonCodecTests
        tests/Dashboard/DashboardManagerJsonCodecTests.cpp)
    forge_configure_standard_target(
        ForgeConductor.Dashboard.ManagerJsonCodecTests)
    target_link_libraries(
        ForgeConductor.Dashboard.ManagerJsonCodecTests PRIVATE
        ForgeConductor::Dashboard.Protocol
        nlohmann_json::nlohmann_json)
    add_test(
        NAME ForgeConductor.Dashboard.ManagerJsonCodecTests
        COMMAND $<TARGET_FILE:ForgeConductor.Dashboard.ManagerJsonCodecTests>)
    set_tests_properties(
        ForgeConductor.Dashboard.ManagerJsonCodecTests PROPERTIES
        LABELS "G16;T-MGR;T-PARITY;T-SEC;T-UNIT"
        TIMEOUT 60)

    add_executable(ForgeConductor.Dashboard.TelemetryJsonCodecTests
        tests/Dashboard/DashboardTelemetryJsonCodecTests.cpp)
    forge_configure_standard_target(
        ForgeConductor.Dashboard.TelemetryJsonCodecTests)
    target_link_libraries(
        ForgeConductor.Dashboard.TelemetryJsonCodecTests PRIVATE
        ForgeConductor::Dashboard.Protocol
        nlohmann_json::nlohmann_json)
    add_test(
        NAME ForgeConductor.Dashboard.TelemetryJsonCodecTests
        COMMAND $<TARGET_FILE:ForgeConductor.Dashboard.TelemetryJsonCodecTests>)
    set_tests_properties(
        ForgeConductor.Dashboard.TelemetryJsonCodecTests PROPERTIES
        LABELS "G16;T-MGR;T-PARITY;T-SEC;T-TEL;T-UNIT"
        TIMEOUT 60)

    add_executable(ForgeConductor.Dashboard.HttpResponseTests
        tests/Dashboard/DashboardHttpResponseTests.cpp)
    forge_configure_standard_target(ForgeConductor.Dashboard.HttpResponseTests)
    target_link_libraries(ForgeConductor.Dashboard.HttpResponseTests PRIVATE
        ForgeConductor::Dashboard.Protocol)
    add_test(
        NAME ForgeConductor.Dashboard.HttpResponseTests
        COMMAND $<TARGET_FILE:ForgeConductor.Dashboard.HttpResponseTests>)
    set_tests_properties(ForgeConductor.Dashboard.HttpResponseTests PROPERTIES
        LABELS "G16;T-MGR;T-PARITY;T-SEC;T-UNIT"
        TIMEOUT 60)

    add_executable(ForgeConductor.Dashboard.StaticResourcePathTests
        tests/Dashboard/DashboardStaticResourcePathTests.cpp)
    forge_configure_standard_target(
        ForgeConductor.Dashboard.StaticResourcePathTests)
    target_link_libraries(
        ForgeConductor.Dashboard.StaticResourcePathTests PRIVATE
        ForgeConductor::Dashboard.Protocol)
    add_test(
        NAME ForgeConductor.Dashboard.StaticResourcePathTests
        COMMAND $<TARGET_FILE:ForgeConductor.Dashboard.StaticResourcePathTests>)
    set_tests_properties(
        ForgeConductor.Dashboard.StaticResourcePathTests PROPERTIES
        LABELS "G16;T-MGR;T-PARITY;T-SEC;T-UNIT"
        TIMEOUT 60)

    add_executable(ForgeConductor.Dashboard.StreamQueryDecoderTests
        tests/Dashboard/DashboardStreamQueryDecoderTests.cpp)
    forge_configure_standard_target(
        ForgeConductor.Dashboard.StreamQueryDecoderTests)
    target_link_libraries(
        ForgeConductor.Dashboard.StreamQueryDecoderTests PRIVATE
        ForgeConductor::Dashboard.Protocol)
    add_test(
        NAME ForgeConductor.Dashboard.StreamQueryDecoderTests
        COMMAND $<TARGET_FILE:ForgeConductor.Dashboard.StreamQueryDecoderTests>)
    set_tests_properties(
        ForgeConductor.Dashboard.StreamQueryDecoderTests PROPERTIES
        LABELS "G16;T-MGR;T-PARITY;T-SEC;T-TEL;T-UNIT"
        TIMEOUT 60)

    add_executable(ForgeConductor.Dashboard.RequestPlannerTests
        tests/Dashboard/DashboardRequestPlannerTests.cpp)
    forge_configure_standard_target(
        ForgeConductor.Dashboard.RequestPlannerTests)
    target_link_libraries(
        ForgeConductor.Dashboard.RequestPlannerTests PRIVATE
        ForgeConductor::Dashboard.Protocol)
    add_test(
        NAME ForgeConductor.Dashboard.RequestPlannerTests
        COMMAND $<TARGET_FILE:ForgeConductor.Dashboard.RequestPlannerTests>)
    set_tests_properties(
        ForgeConductor.Dashboard.RequestPlannerTests PROPERTIES
        LABELS "G16;T-MGR;T-PARITY;T-SEC;T-TEL;T-UNIT"
        TIMEOUT 60)

    add_executable(ForgeConductor.Mcp.CatalogCodecTests
        tests/Mcp/McpCatalogCodecTests.cpp)
    forge_configure_standard_target(ForgeConductor.Mcp.CatalogCodecTests)
    target_link_libraries(ForgeConductor.Mcp.CatalogCodecTests PRIVATE
        ForgeConductor::Mcp
        nlohmann_json::nlohmann_json)
    add_test(
        NAME ForgeConductor.Mcp.CatalogCodecTests
        COMMAND $<TARGET_FILE:ForgeConductor.Mcp.CatalogCodecTests>)
    set_tests_properties(ForgeConductor.Mcp.CatalogCodecTests PROPERTIES
        LABELS "G14;T-MCP;T-UNIT"
        TIMEOUT 60)

    add_executable(ForgeConductor.Mcp.StdioTransportTests
        tests/Mcp/McpStdioTransportTests.cpp)
    forge_configure_native_target(ForgeConductor.Mcp.StdioTransportTests)
    target_link_libraries(ForgeConductor.Mcp.StdioTransportTests PRIVATE
        ForgeConductor::Mcp)
    add_test(
        NAME ForgeConductor.Mcp.StdioTransportTests
        COMMAND $<TARGET_FILE:ForgeConductor.Mcp.StdioTransportTests>)
    set_tests_properties(ForgeConductor.Mcp.StdioTransportTests PROPERTIES
        LABELS "G14;T-MCP;T-UNIT;T-INTEGRATION"
        TIMEOUT 60)

    add_executable(ForgeConductor.Mcp.ProtocolServerTests
        tests/Mcp/McpProtocolServerTests.cpp)
    forge_configure_standard_target(ForgeConductor.Mcp.ProtocolServerTests)
    target_link_libraries(ForgeConductor.Mcp.ProtocolServerTests PRIVATE
        ForgeConductor::Mcp
        nlohmann_json::nlohmann_json)
    add_test(
        NAME ForgeConductor.Mcp.ProtocolServerTests
        COMMAND $<TARGET_FILE:ForgeConductor.Mcp.ProtocolServerTests>)
    set_tests_properties(ForgeConductor.Mcp.ProtocolServerTests PROPERTIES
        LABELS "G14;T-MCP;T-UNIT;T-STRESS"
        TIMEOUT 60)

    add_executable(ForgeConductor.Mcp.ServeProcessSnapshotTests
        tests/Mcp/McpServeProcessSnapshotTests.cpp)
    forge_configure_native_target(
        ForgeConductor.Mcp.ServeProcessSnapshotTests)
    target_link_libraries(
        ForgeConductor.Mcp.ServeProcessSnapshotTests PRIVATE
        nlohmann_json::nlohmann_json)
    add_dependencies(
        ForgeConductor.Mcp.ServeProcessSnapshotTests
        ForgeConductor.Cli)
    add_test(
        NAME ForgeConductor.Mcp.ServeProcessSnapshotTests
        COMMAND $<TARGET_FILE:ForgeConductor.Mcp.ServeProcessSnapshotTests>
            "$<TARGET_FILE:ForgeConductor.Cli>"
            "${PROJECT_SOURCE_DIR}/tests/fixtures/Mcp/mcp-tools-semantic-golden.json")
    set_tests_properties(
        ForgeConductor.Mcp.ServeProcessSnapshotTests PROPERTIES
        LABELS "G14;T-MCP;T-PROC;T-INTEGRATION;T-PARITY"
        TIMEOUT 90)

    add_executable(ForgeConductor.Mcp.ClientWorkspaceContextTests
        tests/Mcp/McpClientWorkspaceContextTests.cpp)
    forge_configure_standard_target(
        ForgeConductor.Mcp.ClientWorkspaceContextTests)
    target_include_directories(
        ForgeConductor.Mcp.ClientWorkspaceContextTests PRIVATE
        "${PROJECT_SOURCE_DIR}/tests")
    target_link_libraries(
        ForgeConductor.Mcp.ClientWorkspaceContextTests PRIVATE
        ForgeConductor::Mcp)
    add_test(
        NAME ForgeConductor.Mcp.ClientWorkspaceContextTests
        COMMAND $<TARGET_FILE:ForgeConductor.Mcp.ClientWorkspaceContextTests>)
    set_tests_properties(
        ForgeConductor.Mcp.ClientWorkspaceContextTests PROPERTIES
        LABELS "G14;T-MCP;T-UNIT;T-STRESS;T-SEC;T-CONT"
        TIMEOUT 60)

    add_executable(ForgeConductor.Mcp.ExecutionServicesTests
        tests/Mcp/McpExecutionServicesTests.cpp)
    forge_configure_standard_target(ForgeConductor.Mcp.ExecutionServicesTests)
    target_include_directories(
        ForgeConductor.Mcp.ExecutionServicesTests PRIVATE
        "${PROJECT_SOURCE_DIR}/tests")
    target_link_libraries(ForgeConductor.Mcp.ExecutionServicesTests PRIVATE
        ForgeConductor::Mcp)
    add_test(
        NAME ForgeConductor.Mcp.ExecutionServicesTests
        COMMAND $<TARGET_FILE:ForgeConductor.Mcp.ExecutionServicesTests>)
    set_tests_properties(ForgeConductor.Mcp.ExecutionServicesTests PROPERTIES
        LABELS "G14;T-MCP;T-UNIT;T-SEC"
        TIMEOUT 60)

    add_executable(ForgeConductor.Mcp.ToolRouterTests
        tests/Mcp/McpToolRouterTests.cpp)
    forge_configure_standard_target(ForgeConductor.Mcp.ToolRouterTests)
    target_include_directories(ForgeConductor.Mcp.ToolRouterTests PRIVATE
        "${PROJECT_SOURCE_DIR}/tests")
    target_link_libraries(ForgeConductor.Mcp.ToolRouterTests PRIVATE
        ForgeConductor::Mcp)
    add_test(
        NAME ForgeConductor.Mcp.ToolRouterTests
        COMMAND $<TARGET_FILE:ForgeConductor.Mcp.ToolRouterTests>)
    set_tests_properties(ForgeConductor.Mcp.ToolRouterTests PROPERTIES
        LABELS "G14;T-MCP;T-UNIT;T-STRESS;T-SEC"
        TIMEOUT 60)

    add_executable(ForgeConductor.Mcp.InvocationGuardTests
        tests/Mcp/McpInvocationGuardTests.cpp)
    forge_configure_standard_target(ForgeConductor.Mcp.InvocationGuardTests)
    target_link_libraries(ForgeConductor.Mcp.InvocationGuardTests PRIVATE
        ForgeConductor::Mcp
        nlohmann_json::nlohmann_json)
    add_test(
        NAME ForgeConductor.Mcp.InvocationGuardTests
        COMMAND $<TARGET_FILE:ForgeConductor.Mcp.InvocationGuardTests>)
    set_tests_properties(ForgeConductor.Mcp.InvocationGuardTests PROPERTIES
        LABELS "G14;T-MCP;T-UNIT;T-STRESS;T-CONT"
        TIMEOUT 60)

    add_executable(ForgeConductor.Mcp.ToolPackAdapterTests
        tests/Mcp/McpToolPackAdapterTests.cpp)
    forge_configure_standard_target(ForgeConductor.Mcp.ToolPackAdapterTests)
    target_include_directories(
        ForgeConductor.Mcp.ToolPackAdapterTests PRIVATE
        "${PROJECT_SOURCE_DIR}/tests")
    target_link_libraries(ForgeConductor.Mcp.ToolPackAdapterTests PRIVATE
        ForgeConductor::Mcp
        nlohmann_json::nlohmann_json)
    add_test(
        NAME ForgeConductor.Mcp.ToolPackAdapterTests
        COMMAND $<TARGET_FILE:ForgeConductor.Mcp.ToolPackAdapterTests>)
    set_tests_properties(ForgeConductor.Mcp.ToolPackAdapterTests PROPERTIES
        LABELS "G14;T-MCP;T-UNIT;T-INTEGRATION;T-PARITY"
        TIMEOUT 60)

    add_executable(ForgeConductor.Mcp.ToolContractMatrixTests
        tests/Mcp/McpToolContractMatrixTests.cpp
        tests/Mcp/McpToolContractTestSupport.cpp)
    forge_configure_standard_target(
        ForgeConductor.Mcp.ToolContractMatrixTests)
    target_include_directories(
        ForgeConductor.Mcp.ToolContractMatrixTests PRIVATE
        "${PROJECT_SOURCE_DIR}/tests")
    target_link_libraries(
        ForgeConductor.Mcp.ToolContractMatrixTests PRIVATE
        ForgeConductor::Mcp
        nlohmann_json::nlohmann_json)
    add_test(
        NAME ForgeConductor.Mcp.ToolContractMatrixTests
        COMMAND $<TARGET_FILE:ForgeConductor.Mcp.ToolContractMatrixTests>
            "${PROJECT_SOURCE_DIR}/tests/fixtures/Mcp/mcp-tool-contract-cases.json")
    set_tests_properties(
        ForgeConductor.Mcp.ToolContractMatrixTests PROPERTIES
        LABELS "G14;T-MCP;T-UNIT;T-PARITY"
        TIMEOUT 60)

    add_executable(ForgeConductor.Agents.CatalogTests
        tests/Agents/AgentCatalogTests.cpp)
    forge_configure_standard_target(ForgeConductor.Agents.CatalogTests)
    target_link_libraries(ForgeConductor.Agents.CatalogTests PRIVATE
        ForgeConductor::Application)
    add_custom_command(TARGET ForgeConductor.Agents.CatalogTests POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E make_directory
            "$<TARGET_FILE_DIR:ForgeConductor.Agents.CatalogTests>/Resources/Agents"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            ${FORGE_AGENT_RESOURCE_PATHS}
            "$<TARGET_FILE_DIR:ForgeConductor.Agents.CatalogTests>/Resources/Agents"
        COMMENT "Staging the bounded agent catalog resources"
        COMMAND_EXPAND_LISTS
        VERBATIM
    )

    add_test(
        NAME ForgeConductor.Agents.CatalogTests
        COMMAND $<TARGET_FILE:ForgeConductor.Agents.CatalogTests>
            "$<TARGET_FILE_DIR:ForgeConductor.Agents.CatalogTests>/Resources/Agents"
    )
    set_tests_properties(ForgeConductor.Agents.CatalogTests PROPERTIES
        LABELS "T-UNIT;T-AGENT;T-SEC;G10"
        TIMEOUT 60
    )

    add_executable(ForgeConductor.Agents.SessionServiceTests
        tests/Agents/AgentSessionServiceTests.cpp)
    forge_configure_standard_target(
        ForgeConductor.Agents.SessionServiceTests)
    target_include_directories(
        ForgeConductor.Agents.SessionServiceTests PRIVATE
        "${PROJECT_SOURCE_DIR}/tests")
    target_link_libraries(
        ForgeConductor.Agents.SessionServiceTests PRIVATE
        ForgeConductor::Application
        ForgeConductor::Infrastructure.Windows)

    add_test(
        NAME ForgeConductor.Agents.SessionServiceTests
        COMMAND $<TARGET_FILE:ForgeConductor.Agents.SessionServiceTests>
    )
    set_tests_properties(
        ForgeConductor.Agents.SessionServiceTests PROPERTIES
        LABELS "T-UNIT;T-AGENT;T-STRESS;G10"
        TIMEOUT 120
    )

    add_executable(ForgeConductor.Agents.ProcessFixture
        tests/Agents/AgentSessionProcessFixture.cpp)
    forge_configure_native_target(ForgeConductor.Agents.ProcessFixture)
    target_include_directories(ForgeConductor.Agents.ProcessFixture PRIVATE
        "${PROJECT_SOURCE_DIR}/src"
        "${PROJECT_SOURCE_DIR}/tests")
    target_link_libraries(ForgeConductor.Agents.ProcessFixture PRIVATE
        ForgeConductor::Persistence.Windows
        ForgeConductor::Infrastructure.Windows)

    add_executable(ForgeConductor.Agents.RepositoryWindowsTests
        tests/Agents/AgentSessionRepositoryWindowsTests.cpp)
    forge_configure_native_target(
        ForgeConductor.Agents.RepositoryWindowsTests)
    target_include_directories(
        ForgeConductor.Agents.RepositoryWindowsTests PRIVATE
        "${PROJECT_SOURCE_DIR}/src"
        "${PROJECT_SOURCE_DIR}/tests")
    target_link_libraries(
        ForgeConductor.Agents.RepositoryWindowsTests PRIVATE
        ForgeConductor::Persistence.Windows
        ForgeConductor::Infrastructure.Windows)
    add_dependencies(
        ForgeConductor.Agents.RepositoryWindowsTests
        ForgeConductor.Agents.ProcessFixture)

    add_test(
        NAME ForgeConductor.Agents.RepositoryWindowsTests
        COMMAND $<TARGET_FILE:ForgeConductor.Agents.RepositoryWindowsTests>
            $<TARGET_FILE:ForgeConductor.Agents.ProcessFixture>
            "${PROJECT_SOURCE_DIR}/tests/Persistence/Fixtures"
    )
    set_tests_properties(
        ForgeConductor.Agents.RepositoryWindowsTests PROPERTIES
        LABELS "T-UNIT;T-DB;T-AGENT;T-SEC;G10"
        TIMEOUT 180
    )

    add_executable(ForgeConductor.Domain.UnitTests
        tests/Domain/DomainTests.cpp)
    forge_configure_standard_target(ForgeConductor.Domain.UnitTests)
    target_link_libraries(ForgeConductor.Domain.UnitTests PRIVATE
        ForgeConductor::Domain)

    add_test(
        NAME ForgeConductor.Domain.UnitTests
        COMMAND $<TARGET_FILE:ForgeConductor.Domain.UnitTests>
    )
    set_tests_properties(ForgeConductor.Domain.UnitTests PROPERTIES
        LABELS "T-UNIT;G05"
    )

    add_test(
        NAME ForgeConductor.Continuity.DomainUnitTests
        COMMAND $<TARGET_FILE:ForgeConductor.Domain.UnitTests>
    )
    set_tests_properties(ForgeConductor.Continuity.DomainUnitTests PROPERTIES
        LABELS "T-UNIT;T-CONT;G11"
        TIMEOUT 60
    )

    add_executable(ForgeConductor.Contracts.ContractTests
        tests/Contracts/FoundationTelemetryFakeContractTests.cpp
        tests/Contracts/GroupedFakeContractTests.cpp
        tests/Contracts/main.cpp)
    forge_configure_standard_target(ForgeConductor.Contracts.ContractTests)
    target_compile_options(ForgeConductor.Contracts.ContractTests PRIVATE /bigobj)
    target_include_directories(ForgeConductor.Contracts.ContractTests PRIVATE
        "${PROJECT_SOURCE_DIR}/tests")
    target_link_libraries(ForgeConductor.Contracts.ContractTests PRIVATE
        ForgeConductor::Contracts)

    add_test(
        NAME ForgeConductor.Contracts.ContractTests
        COMMAND $<TARGET_FILE:ForgeConductor.Contracts.ContractTests>
    )
    set_tests_properties(ForgeConductor.Contracts.ContractTests PROPERTIES
        LABELS "T-UNIT;T-LMS;G05;G15"
        TIMEOUT 60
    )

    add_test(
        NAME ForgeConductor.Continuity.ContractTests
        COMMAND $<TARGET_FILE:ForgeConductor.Contracts.ContractTests>
    )
    set_tests_properties(ForgeConductor.Continuity.ContractTests PROPERTIES
        LABELS "T-UNIT;T-CONT;G11"
        TIMEOUT 60
    )

    file(GLOB _forge_domain_contract_headers CONFIGURE_DEPENDS
        "${PROJECT_SOURCE_DIR}/include/ForgeConductor/Domain/*.h"
        "${PROJECT_SOURCE_DIR}/include/ForgeConductor/Contracts/*.h")
    if(NOT _forge_domain_contract_headers)
        message(FATAL_ERROR "P05 public Domain/Contracts header inventory is empty.")
    endif()

    set(_forge_header_isolation_sources)
    file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/generated/p05-header-isolation")
    foreach(_header IN LISTS _forge_domain_contract_headers)
        file(RELATIVE_PATH _header_relative "${PROJECT_SOURCE_DIR}/include" "${_header}")
        string(REPLACE "\\" "/" _header_relative "${_header_relative}")
        string(MAKE_C_IDENTIFIER "${_header_relative}" _header_identifier)
        set(_isolation_source
            "${CMAKE_CURRENT_BINARY_DIR}/generated/p05-header-isolation/${_header_identifier}.cpp")
        file(GENERATE OUTPUT "${_isolation_source}" CONTENT
            "#include <${_header_relative}>\nint ${_header_identifier}_isolated() noexcept { return 0; }\n")
        list(APPEND _forge_header_isolation_sources "${_isolation_source}")
    endforeach()

    add_library(ForgeConductor.DomainContracts.HeaderObjects OBJECT
        ${_forge_header_isolation_sources})
    forge_configure_standard_target(ForgeConductor.DomainContracts.HeaderObjects)
    target_link_libraries(ForgeConductor.DomainContracts.HeaderObjects PRIVATE
        ForgeConductor::Contracts)

    add_executable(ForgeConductor.Contracts.HeaderSelfContainment
        tests/Architecture/P05HeaderSelfContainmentMain.cpp)
    forge_configure_standard_target(ForgeConductor.Contracts.HeaderSelfContainment)
    target_link_libraries(ForgeConductor.Contracts.HeaderSelfContainment PRIVATE
        ForgeConductor.DomainContracts.HeaderObjects
        ForgeConductor::Contracts)

    add_test(
        NAME ForgeConductor.Contracts.HeaderSelfContainment
        COMMAND $<TARGET_FILE:ForgeConductor.Contracts.HeaderSelfContainment>
    )
    set_tests_properties(ForgeConductor.Contracts.HeaderSelfContainment PROPERTIES
        LABELS "T-UNIT;T-LMS;G05;G15"
        TIMEOUT 60
    )

    add_executable(ForgeConductor.Infrastructure.UnitTests
        tests/Application/LMStudioDeploymentServiceTests.cpp
        tests/Infrastructure/FoundationWindowsTests.cpp
        tests/Infrastructure/InfrastructureTestMain.cpp
        tests/Infrastructure/LMStudioConfigurationCodecTests.cpp
        tests/Infrastructure/StorageWindowsTests.cpp
        tests/Infrastructure/WindowsLMStudioEnvironmentTests.cpp
        tests/Infrastructure/WindowsLMStudioHostActivatorTests.cpp
        tests/Infrastructure/WindowsLMStudioServeVerifierTests.cpp
        tests/Infrastructure/WindowsUnicodeCanonicalizerTests.cpp
        tests/Infrastructure/WindowsDiagnosticSinkTests.cpp)
    forge_configure_native_target(ForgeConductor.Infrastructure.UnitTests)
    target_include_directories(ForgeConductor.Infrastructure.UnitTests PRIVATE
        "${PROJECT_SOURCE_DIR}/src"
        "${PROJECT_SOURCE_DIR}/tests")
    target_link_libraries(ForgeConductor.Infrastructure.UnitTests PRIVATE
        ForgeConductor::Infrastructure.Windows
        nlohmann_json::nlohmann_json)

    add_test(
        NAME ForgeConductor.Infrastructure.UnitTests
        COMMAND $<TARGET_FILE:ForgeConductor.Infrastructure.UnitTests>
    )
    set_tests_properties(ForgeConductor.Infrastructure.UnitTests PROPERTIES
        LABELS "T-UNIT;T-SEC;T-LMS;G06;G15"
        TIMEOUT 180
    )

    add_executable(
        ForgeConductor.Infrastructure.DiagnosticLogTailReaderTests
        tests/Infrastructure/WindowsDiagnosticLogTailReaderTests.cpp)
    forge_configure_native_target(
        ForgeConductor.Infrastructure.DiagnosticLogTailReaderTests)
    target_include_directories(
        ForgeConductor.Infrastructure.DiagnosticLogTailReaderTests PRIVATE
        "${PROJECT_SOURCE_DIR}/src"
        "${PROJECT_SOURCE_DIR}/tests")
    target_link_libraries(
        ForgeConductor.Infrastructure.DiagnosticLogTailReaderTests PRIVATE
        ForgeConductor::Infrastructure.Windows)

    add_test(
        NAME ForgeConductor.Infrastructure.DiagnosticLogTailReaderTests
        COMMAND
            $<TARGET_FILE:ForgeConductor.Infrastructure.DiagnosticLogTailReaderTests>
    )
    set_tests_properties(
        ForgeConductor.Infrastructure.DiagnosticLogTailReaderTests PROPERTIES
        LABELS "T-UNIT;T-SEC;G16"
        TIMEOUT 120
    )

    add_executable(ForgeConductor.Infrastructure.ShutdownTests
        tests/Infrastructure/ShutdownWindowsTests.cpp)
    forge_configure_native_target(ForgeConductor.Infrastructure.ShutdownTests)
    target_include_directories(ForgeConductor.Infrastructure.ShutdownTests PRIVATE
        "${PROJECT_SOURCE_DIR}/tests")
    target_link_libraries(ForgeConductor.Infrastructure.ShutdownTests PRIVATE
        ForgeConductor::Infrastructure.Windows)

    add_test(
        NAME ForgeConductor.Infrastructure.ShutdownTests
        COMMAND $<TARGET_FILE:ForgeConductor.Infrastructure.ShutdownTests>
    )
    set_tests_properties(ForgeConductor.Infrastructure.ShutdownTests PROPERTIES
        LABELS "T-UNIT;T-STRESS;G06"
    )

    add_executable(ForgeConductor.ProcessFixture
        tests/Infrastructure/Windows/ProcessFixture.cpp)
    forge_configure_native_target(ForgeConductor.ProcessFixture)

    add_executable(ForgeConductor.Infrastructure.ProcessTests
        tests/Infrastructure/ProcessIntegrationTestMain.cpp
        tests/Infrastructure/Windows/OverlappedPipeReaderTests.cpp
        tests/Infrastructure/Windows/WindowsProcessSupervisorTests.cpp)
    forge_configure_native_target(ForgeConductor.Infrastructure.ProcessTests)
    target_include_directories(ForgeConductor.Infrastructure.ProcessTests PRIVATE
        "${PROJECT_SOURCE_DIR}/src"
        "${PROJECT_SOURCE_DIR}/tests")
    target_link_libraries(ForgeConductor.Infrastructure.ProcessTests PRIVATE
        ForgeConductor::Infrastructure.Windows)
    add_dependencies(ForgeConductor.Infrastructure.ProcessTests
        ForgeConductor.ProcessFixture)

    add_test(
        NAME ForgeConductor.Infrastructure.ProcessTests
        COMMAND $<TARGET_FILE:ForgeConductor.Infrastructure.ProcessTests>
            "$<TARGET_FILE:ForgeConductor.ProcessFixture>"
    )
    set_tests_properties(ForgeConductor.Infrastructure.ProcessTests PROPERTIES
        LABELS "T-PROC;T-SEC;T-LMS;G06;G15"
        TIMEOUT 180
    )

    add_executable(ForgeConductor.LMStudio.RealHostTests
        tests/Infrastructure/WindowsLMStudioRealHostTests.cpp)
    forge_configure_native_target(ForgeConductor.LMStudio.RealHostTests)
    target_include_directories(
        ForgeConductor.LMStudio.RealHostTests PRIVATE
        "${PROJECT_SOURCE_DIR}/src")
    target_link_libraries(
        ForgeConductor.LMStudio.RealHostTests PRIVATE
        ForgeConductor::Infrastructure.Windows
        ForgeConductor::Mcp
        ForgeConductor::NativeTools.Windows
        nlohmann_json::nlohmann_json)
    add_dependencies(
        ForgeConductor.LMStudio.RealHostTests
        ForgeConductor.Cli)
    # This mutating machine-local qualification is intentionally not a CTest.
    # The G15 validator invokes it exactly once after deterministic tests pass.

    file(GLOB _forge_infrastructure_windows_headers CONFIGURE_DEPENDS
        "${PROJECT_SOURCE_DIR}/include/ForgeConductor/Infrastructure/Windows/*.h")
    if(NOT _forge_infrastructure_windows_headers)
        message(FATAL_ERROR "P06 public Windows infrastructure header inventory is empty.")
    endif()

    set(_forge_p06_header_isolation_sources)
    file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/generated/p06-header-isolation")
    foreach(_header IN LISTS _forge_infrastructure_windows_headers)
        file(RELATIVE_PATH _header_relative "${PROJECT_SOURCE_DIR}/include" "${_header}")
        string(REPLACE "\\" "/" _header_relative "${_header_relative}")
        string(MAKE_C_IDENTIFIER "${_header_relative}" _header_identifier)
        set(_isolation_source
            "${CMAKE_CURRENT_BINARY_DIR}/generated/p06-header-isolation/${_header_identifier}.cpp")
        file(GENERATE OUTPUT "${_isolation_source}" CONTENT
            "#include <${_header_relative}>\nint ${_header_identifier}_isolated() noexcept { return 0; }\n")
        list(APPEND _forge_p06_header_isolation_sources "${_isolation_source}")
    endforeach()

    add_library(ForgeConductor.InfrastructureWindows.HeaderObjects OBJECT
        ${_forge_p06_header_isolation_sources})
    forge_configure_native_target(ForgeConductor.InfrastructureWindows.HeaderObjects)
    target_link_libraries(ForgeConductor.InfrastructureWindows.HeaderObjects PRIVATE
        ForgeConductor::Infrastructure.Windows)

    add_executable(ForgeConductor.Infrastructure.HeaderSelfContainment
        tests/Architecture/P06HeaderSelfContainmentMain.cpp)
    forge_configure_native_target(ForgeConductor.Infrastructure.HeaderSelfContainment)
    target_link_libraries(ForgeConductor.Infrastructure.HeaderSelfContainment PRIVATE
        ForgeConductor.InfrastructureWindows.HeaderObjects
        ForgeConductor::Infrastructure.Windows)

    add_test(
        NAME ForgeConductor.Infrastructure.HeaderSelfContainment
        COMMAND $<TARGET_FILE:ForgeConductor.Infrastructure.HeaderSelfContainment>
    )
    set_tests_properties(ForgeConductor.Infrastructure.HeaderSelfContainment PROPERTIES
        LABELS "T-UNIT;T-LMS;G06;G15"
        TIMEOUT 120
    )

    add_executable(ForgeConductor.Persistence.ProcessFixture
        tests/Persistence/PersistenceProcessFixture.cpp)
    forge_configure_native_target(ForgeConductor.Persistence.ProcessFixture)
    target_include_directories(ForgeConductor.Persistence.ProcessFixture PRIVATE
        "${PROJECT_SOURCE_DIR}/src"
        "${PROJECT_SOURCE_DIR}/tests")
    target_link_libraries(ForgeConductor.Persistence.ProcessFixture PRIVATE
        ForgeConductor::Persistence.Windows)

    add_executable(ForgeConductor.Persistence.UnitTests
        tests/Persistence/BackupIntegrityTests.cpp
        tests/Persistence/CentralMigrationTests.cpp
        tests/Persistence/DatabaseAuthorityTests.cpp
        tests/Persistence/DatabaseConcurrencyTests.cpp
        tests/Persistence/DatabaseStoreSerializationTests.cpp
        tests/Persistence/IntegrityRecoveryTests.cpp
        tests/Persistence/PersistenceTestMain.cpp
        tests/Persistence/ProjectMigrationTests.cpp
        tests/Persistence/WinsqliteKernelTests.cpp)
    forge_configure_native_target(ForgeConductor.Persistence.UnitTests)
    target_include_directories(ForgeConductor.Persistence.UnitTests PRIVATE
        "${PROJECT_SOURCE_DIR}/src"
        "${PROJECT_SOURCE_DIR}/tests")
    target_link_libraries(ForgeConductor.Persistence.UnitTests PRIVATE
        ForgeConductor::Persistence.Windows
        nlohmann_json::nlohmann_json)
    add_dependencies(ForgeConductor.Persistence.UnitTests
        ForgeConductor.Persistence.ProcessFixture)

    add_test(
        NAME ForgeConductor.Persistence.UnitTests
        COMMAND $<TARGET_FILE:ForgeConductor.Persistence.UnitTests>
            "${PROJECT_SOURCE_DIR}/tests/Persistence/Fixtures"
            "$<TARGET_FILE:ForgeConductor.Persistence.ProcessFixture>"
    )
    set_tests_properties(ForgeConductor.Persistence.UnitTests PROPERTIES
        LABELS "T-UNIT;T-DB;T-SEC;G07"
        TIMEOUT 240
    )

    add_executable(ForgeConductor.Persistence.AuditRepositoryWindowsTests
        tests/Persistence/AuditRepositoryWindowsTests.cpp)
    forge_configure_native_target(
        ForgeConductor.Persistence.AuditRepositoryWindowsTests)
    target_include_directories(
        ForgeConductor.Persistence.AuditRepositoryWindowsTests PRIVATE
        "${PROJECT_SOURCE_DIR}/src"
        "${PROJECT_SOURCE_DIR}/tests")
    target_link_libraries(
        ForgeConductor.Persistence.AuditRepositoryWindowsTests PRIVATE
        ForgeConductor::Persistence.Windows)

    add_test(
        NAME ForgeConductor.Persistence.AuditRepositoryWindowsTests
        COMMAND $<TARGET_FILE:ForgeConductor.Persistence.AuditRepositoryWindowsTests>
    )
    set_tests_properties(
        ForgeConductor.Persistence.AuditRepositoryWindowsTests PROPERTIES
        LABELS "G14;T-MCP;T-DB;T-UNIT;T-INTEGRATION;T-SEC"
        TIMEOUT 120
    )

    add_executable(ForgeConductor.Persistence.ForgeStatusRepositoryWindowsTests
        tests/Persistence/ForgeStatusRepositoryWindowsTests.cpp)
    forge_configure_native_target(
        ForgeConductor.Persistence.ForgeStatusRepositoryWindowsTests)
    target_include_directories(
        ForgeConductor.Persistence.ForgeStatusRepositoryWindowsTests PRIVATE
        "${PROJECT_SOURCE_DIR}/src"
        "${PROJECT_SOURCE_DIR}/tests")
    target_link_libraries(
        ForgeConductor.Persistence.ForgeStatusRepositoryWindowsTests PRIVATE
        ForgeConductor::Persistence.Windows)

    add_test(
        NAME ForgeConductor.Persistence.ForgeStatusRepositoryWindowsTests
        COMMAND $<TARGET_FILE:ForgeConductor.Persistence.ForgeStatusRepositoryWindowsTests>
    )
    set_tests_properties(
        ForgeConductor.Persistence.ForgeStatusRepositoryWindowsTests PROPERTIES
        LABELS "G14;T-MCP;T-DB;T-UNIT;T-INTEGRATION;T-SEC"
        TIMEOUT 180
    )

    add_executable(
        ForgeConductor.Persistence.DashboardOperationalRepositoryWindowsTests
        tests/Persistence/DashboardOperationalRepositoryWindowsTests.cpp)
    forge_configure_native_target(
        ForgeConductor.Persistence.DashboardOperationalRepositoryWindowsTests)
    target_include_directories(
        ForgeConductor.Persistence.DashboardOperationalRepositoryWindowsTests PRIVATE
        "${PROJECT_SOURCE_DIR}/src"
        "${PROJECT_SOURCE_DIR}/tests")
    target_link_libraries(
        ForgeConductor.Persistence.DashboardOperationalRepositoryWindowsTests PRIVATE
        ForgeConductor::Persistence.Windows)

    add_test(
        NAME ForgeConductor.Persistence.DashboardOperationalRepositoryWindowsTests
        COMMAND
            $<TARGET_FILE:ForgeConductor.Persistence.DashboardOperationalRepositoryWindowsTests>
    )
    set_tests_properties(
        ForgeConductor.Persistence.DashboardOperationalRepositoryWindowsTests
        PROPERTIES
        LABELS "G16;T-DB;T-UNIT;T-INTEGRATION;T-SEC"
        TIMEOUT 180
    )

    add_executable(ForgeConductor.Persistence.ClientPresenceRepositoryWindowsTests
        tests/Persistence/ClientPresenceRepositoryWindowsTests.cpp)
    forge_configure_native_target(
        ForgeConductor.Persistence.ClientPresenceRepositoryWindowsTests)
    target_include_directories(
        ForgeConductor.Persistence.ClientPresenceRepositoryWindowsTests PRIVATE
        "${PROJECT_SOURCE_DIR}/src"
        "${PROJECT_SOURCE_DIR}/tests")
    target_link_libraries(
        ForgeConductor.Persistence.ClientPresenceRepositoryWindowsTests PRIVATE
        ForgeConductor::Persistence.Windows)

    add_test(
        NAME ForgeConductor.Persistence.ClientPresenceRepositoryWindowsTests
        COMMAND $<TARGET_FILE:ForgeConductor.Persistence.ClientPresenceRepositoryWindowsTests>
    )
    set_tests_properties(
        ForgeConductor.Persistence.ClientPresenceRepositoryWindowsTests PROPERTIES
        LABELS "G14;T-MCP;T-DB;T-UNIT;T-INTEGRATION"
        TIMEOUT 120
    )

    add_executable(ForgeConductor.Application.ClientPresenceLifecycleTests
        tests/Application/ClientPresenceLifecycleTests.cpp)
    forge_configure_standard_target(
        ForgeConductor.Application.ClientPresenceLifecycleTests)
    target_include_directories(
        ForgeConductor.Application.ClientPresenceLifecycleTests PRIVATE
        "${PROJECT_SOURCE_DIR}/tests")
    target_link_libraries(
        ForgeConductor.Application.ClientPresenceLifecycleTests PRIVATE
        ForgeConductor::Application)

    add_test(
        NAME ForgeConductor.Application.ClientPresenceLifecycleTests
        COMMAND $<TARGET_FILE:ForgeConductor.Application.ClientPresenceLifecycleTests>
    )
    set_tests_properties(
        ForgeConductor.Application.ClientPresenceLifecycleTests PROPERTIES
        LABELS "G14;T-MCP;T-UNIT;T-STRESS"
        TIMEOUT 60
    )

    add_test(
        NAME ForgeConductor.Continuity.ProjectMigrationRegressionTests
        COMMAND $<TARGET_FILE:ForgeConductor.Persistence.UnitTests>
            "${PROJECT_SOURCE_DIR}/tests/Persistence/Fixtures"
            "$<TARGET_FILE:ForgeConductor.Persistence.ProcessFixture>"
    )
    set_tests_properties(
        ForgeConductor.Continuity.ProjectMigrationRegressionTests PROPERTIES
        LABELS "T-UNIT;T-DB;T-CONT;G11"
        TIMEOUT 240
    )

    file(GLOB _forge_persistence_windows_headers CONFIGURE_DEPENDS
        "${PROJECT_SOURCE_DIR}/include/ForgeConductor/Persistence/Windows/*.h")
    if(NOT _forge_persistence_windows_headers)
        message(FATAL_ERROR "P07 public Windows persistence header inventory is empty.")
    endif()

    set(_forge_p07_header_isolation_sources)
    file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/generated/p07-header-isolation")
    foreach(_header IN LISTS _forge_persistence_windows_headers)
        file(RELATIVE_PATH _header_relative "${PROJECT_SOURCE_DIR}/include" "${_header}")
        string(REPLACE "\\" "/" _header_relative "${_header_relative}")
        string(MAKE_C_IDENTIFIER "${_header_relative}" _header_identifier)
        set(_isolation_source
            "${CMAKE_CURRENT_BINARY_DIR}/generated/p07-header-isolation/${_header_identifier}.cpp")
        file(GENERATE OUTPUT "${_isolation_source}" CONTENT
            "#include <${_header_relative}>\nint ${_header_identifier}_isolated() noexcept { return 0; }\n")
        list(APPEND _forge_p07_header_isolation_sources "${_isolation_source}")
    endforeach()

    add_library(ForgeConductor.PersistenceWindows.HeaderObjects OBJECT
        ${_forge_p07_header_isolation_sources})
    forge_configure_standard_target(ForgeConductor.PersistenceWindows.HeaderObjects)
    target_link_libraries(ForgeConductor.PersistenceWindows.HeaderObjects PRIVATE
        ForgeConductor::Persistence.Windows)

    add_executable(ForgeConductor.Persistence.HeaderSelfContainment
        tests/Architecture/P07HeaderSelfContainmentMain.cpp)
    forge_configure_standard_target(ForgeConductor.Persistence.HeaderSelfContainment)
    target_link_libraries(ForgeConductor.Persistence.HeaderSelfContainment PRIVATE
        ForgeConductor.PersistenceWindows.HeaderObjects
        ForgeConductor::Persistence.Windows)

    add_test(
        NAME ForgeConductor.Persistence.HeaderSelfContainment
        COMMAND $<TARGET_FILE:ForgeConductor.Persistence.HeaderSelfContainment>
    )
    set_tests_properties(ForgeConductor.Persistence.HeaderSelfContainment PROPERTIES
        LABELS "T-UNIT;G07"
        TIMEOUT 60
    )

    add_executable(ForgeConductor.ProjectMemory.CacheTests
        tests/ProjectMemory/ProjectMemoryCacheTests.cpp)
    forge_configure_standard_target(ForgeConductor.ProjectMemory.CacheTests)
    target_include_directories(ForgeConductor.ProjectMemory.CacheTests PRIVATE
        "${PROJECT_SOURCE_DIR}/tests")
    target_link_libraries(ForgeConductor.ProjectMemory.CacheTests PRIVATE
        ForgeConductor::Application)

    add_test(
        NAME ForgeConductor.ProjectMemory.CacheTests
        COMMAND $<TARGET_FILE:ForgeConductor.ProjectMemory.CacheTests>
    )
    set_tests_properties(ForgeConductor.ProjectMemory.CacheTests PROPERTIES
        LABELS "T-UNIT;T-MEM;T-STRESS;G08"
        TIMEOUT 60
    )

    add_test(
        NAME ForgeConductor.Continuity.RepositoryAggregateCacheTests
        COMMAND $<TARGET_FILE:ForgeConductor.ProjectMemory.CacheTests>
    )
    set_tests_properties(
        ForgeConductor.Continuity.RepositoryAggregateCacheTests PROPERTIES
        LABELS "T-UNIT;T-MEM;T-CONT;T-STRESS;G11"
        TIMEOUT 60
    )

    add_executable(ForgeConductor.Continuity.DocumentCodecTests
        tests/Infrastructure/ContinuityDocumentCodecTests.cpp)
    forge_configure_native_target(
        ForgeConductor.Continuity.DocumentCodecTests)
    target_include_directories(
        ForgeConductor.Continuity.DocumentCodecTests PRIVATE
        "${PROJECT_SOURCE_DIR}/src"
        "${PROJECT_SOURCE_DIR}/tests")
    target_link_libraries(
        ForgeConductor.Continuity.DocumentCodecTests PRIVATE
        ForgeConductor::Infrastructure.Windows)

    add_test(
        NAME ForgeConductor.Continuity.DocumentCodecTests
        COMMAND $<TARGET_FILE:ForgeConductor.Continuity.DocumentCodecTests>
    )
    set_tests_properties(
        ForgeConductor.Continuity.DocumentCodecTests PROPERTIES
        LABELS "T-UNIT;T-CONT;T-SEC;G11"
        TIMEOUT 60
    )

    add_executable(ForgeConductor.Continuity.LegacyApplicationTests
        tests/Application/LegacyContextContinuityServiceTests.cpp)
    forge_configure_standard_target(
        ForgeConductor.Continuity.LegacyApplicationTests)
    target_include_directories(
        ForgeConductor.Continuity.LegacyApplicationTests PRIVATE
        "${PROJECT_SOURCE_DIR}/tests")
    target_link_libraries(
        ForgeConductor.Continuity.LegacyApplicationTests PRIVATE
        ForgeConductor::Application)

    add_test(
        NAME ForgeConductor.Continuity.LegacyApplicationTests
        COMMAND $<TARGET_FILE:ForgeConductor.Continuity.LegacyApplicationTests>
    )
    set_tests_properties(
        ForgeConductor.Continuity.LegacyApplicationTests PROPERTIES
        LABELS "T-UNIT;T-CONT;T-STRESS;G11"
        TIMEOUT 120
    )

    add_executable(ForgeConductor.Continuity.CoordinatorTests
        tests/Continuity/ContinuityCoordinatorTests.cpp)
    forge_configure_standard_target(
        ForgeConductor.Continuity.CoordinatorTests)
    target_include_directories(
        ForgeConductor.Continuity.CoordinatorTests PRIVATE
        "${PROJECT_SOURCE_DIR}/tests")
    target_link_libraries(
        ForgeConductor.Continuity.CoordinatorTests PRIVATE
        ForgeConductor::Application)

    add_test(
        NAME ForgeConductor.Continuity.CoordinatorTests
        COMMAND $<TARGET_FILE:ForgeConductor.Continuity.CoordinatorTests>
    )
    set_tests_properties(
        ForgeConductor.Continuity.CoordinatorTests PROPERTIES
        LABELS "T-UNIT;T-CONT;T-STRESS;G11"
        TIMEOUT 120
    )

    add_executable(ForgeConductor.Continuity.AutomationTests
        tests/Continuity/ContinuityAutomationTests.cpp)
    forge_configure_standard_target(
        ForgeConductor.Continuity.AutomationTests)
    target_include_directories(
        ForgeConductor.Continuity.AutomationTests PRIVATE
        "${PROJECT_SOURCE_DIR}/tests")
    target_link_libraries(
        ForgeConductor.Continuity.AutomationTests PRIVATE
        ForgeConductor::Application)

    add_test(
        NAME ForgeConductor.Continuity.AutomationTests
        COMMAND $<TARGET_FILE:ForgeConductor.Continuity.AutomationTests>
    )
    set_tests_properties(
        ForgeConductor.Continuity.AutomationTests PROPERTIES
        LABELS "T-UNIT;T-CONT;T-AUTO;G12"
        TIMEOUT 120
    )

    add_executable(ForgeConductor.SessionHost.PluginSmokeTests
        tests/SessionHost/NativeSessionHostPluginSmokeTests.cpp)
    forge_configure_native_target(
        ForgeConductor.SessionHost.PluginSmokeTests)
    target_include_directories(
        ForgeConductor.SessionHost.PluginSmokeTests PRIVATE
        "${PROJECT_SOURCE_DIR}/tests")
    target_link_libraries(
        ForgeConductor.SessionHost.PluginSmokeTests PRIVATE
        ForgeConductor::SessionHost.Core
        ForgeConductor::Infrastructure.Windows)

    add_test(
        NAME ForgeConductor.SessionHost.PluginSmokeTests
        COMMAND $<TARGET_FILE:ForgeConductor.SessionHost.PluginSmokeTests>
            "$<TARGET_FILE:ForgeNativeSessionHostPlugin>"
    )
    set_tests_properties(
        ForgeConductor.SessionHost.PluginSmokeTests PROPERTIES
        LABELS "T-UNIT;T-CONT;T-ABI;G12"
        TIMEOUT 120
    )

    add_executable(ForgeConductor.SessionHost.AdapterTests
        tests/SessionHost/NativeSessionHostAdapterTests.cpp)
    forge_configure_native_target(
        ForgeConductor.SessionHost.AdapterTests)
    target_include_directories(
        ForgeConductor.SessionHost.AdapterTests PRIVATE
        "${PROJECT_SOURCE_DIR}/tests")
    target_link_libraries(
        ForgeConductor.SessionHost.AdapterTests PRIVATE
        ForgeConductor::SessionHost.Core
        ForgeConductor::Infrastructure.Windows)

    add_test(
        NAME ForgeConductor.SessionHost.AdapterTests
        COMMAND $<TARGET_FILE:ForgeConductor.SessionHost.AdapterTests>
    )
    set_tests_properties(
        ForgeConductor.SessionHost.AdapterTests PROPERTIES
        LABELS "T-UNIT;T-CONT;T-STRESS;G12"
        TIMEOUT 120
    )

    add_executable(ForgeConductor.SessionHost.WinHttpTransportTests
        tests/SessionHost/WinHttpLocalModelSessionTransportTests.cpp)
    forge_configure_native_target(
        ForgeConductor.SessionHost.WinHttpTransportTests)
    target_include_directories(
        ForgeConductor.SessionHost.WinHttpTransportTests PRIVATE
        "${PROJECT_SOURCE_DIR}/tests")
    target_link_libraries(
        ForgeConductor.SessionHost.WinHttpTransportTests PRIVATE
        ForgeConductor::Infrastructure.Windows
        ws2_32)

    add_test(
        NAME ForgeConductor.SessionHost.WinHttpTransportTests
        COMMAND $<TARGET_FILE:ForgeConductor.SessionHost.WinHttpTransportTests>
    )
    set_tests_properties(
        ForgeConductor.SessionHost.WinHttpTransportTests PROPERTIES
        LABELS "T-UNIT;T-CONT;T-NET;T-STRESS;G12"
        TIMEOUT 120
    )

    add_executable(ForgeConductor.SessionHost.LedgerTests
        tests/SessionHost/WindowsNativeSessionLedgerTests.cpp)
    forge_configure_native_target(
        ForgeConductor.SessionHost.LedgerTests)
    target_include_directories(
        ForgeConductor.SessionHost.LedgerTests PRIVATE
        "${PROJECT_SOURCE_DIR}/tests")
    target_link_libraries(
        ForgeConductor.SessionHost.LedgerTests PRIVATE
        ForgeConductor::Infrastructure.Windows)

    add_test(
        NAME ForgeConductor.SessionHost.LedgerTests
        COMMAND $<TARGET_FILE:ForgeConductor.SessionHost.LedgerTests>
    )
    set_tests_properties(
        ForgeConductor.SessionHost.LedgerTests PROPERTIES
        LABELS "T-UNIT;T-CONT;T-STORAGE;T-STRESS;G12"
        TIMEOUT 120
    )

    add_executable(ForgeConductor.SessionHost.ContinuityEndToEndTests
        tests/SessionHost/NativeSessionContinuityEndToEndTests.cpp)
    forge_configure_native_target(
        ForgeConductor.SessionHost.ContinuityEndToEndTests)
    target_include_directories(
        ForgeConductor.SessionHost.ContinuityEndToEndTests PRIVATE
        "${PROJECT_SOURCE_DIR}/tests")
    target_link_libraries(
        ForgeConductor.SessionHost.ContinuityEndToEndTests PRIVATE
        ForgeConductor::Infrastructure.Windows
        ForgeConductor::SessionHost.Core)

    add_test(
        NAME ForgeConductor.SessionHost.ContinuityEndToEndTests
        COMMAND
            $<TARGET_FILE:ForgeConductor.SessionHost.ContinuityEndToEndTests>
    )
    set_tests_properties(
        ForgeConductor.SessionHost.ContinuityEndToEndTests PROPERTIES
        LABELS "T-UNIT;T-CONT;T-STORAGE;G12"
        TIMEOUT 120
    )

    foreach(session_host_command IN ITEMS
            manifest health recover cancel-orphans self-test)
        add_test(
            NAME ForgeConductor.SessionHost.Command.${session_host_command}
            COMMAND
                $<TARGET_FILE:ForgeConductor.SessionHost>
                --${session_host_command}
        )
        set_tests_properties(
            ForgeConductor.SessionHost.Command.${session_host_command}
            PROPERTIES
            LABELS "T-INTEGRATION;T-CONT;G12"
            TIMEOUT 60
        )
    endforeach()

    add_executable(ForgeConductor.Continuity.ProcessFixtureTests
        tests/Continuity/ContinuityCoordinatorProcessFixture.cpp)
    forge_configure_native_target(
        ForgeConductor.Continuity.ProcessFixtureTests)
    target_include_directories(
        ForgeConductor.Continuity.ProcessFixtureTests PRIVATE
        "${PROJECT_SOURCE_DIR}/src"
        "${PROJECT_SOURCE_DIR}/tests")
    target_link_libraries(
        ForgeConductor.Continuity.ProcessFixtureTests PRIVATE
        ForgeConductor::Application
        ForgeConductor::Persistence.Windows)

    add_test(
        NAME ForgeConductor.Continuity.ProcessFixtureTests
        COMMAND $<TARGET_FILE:ForgeConductor.Continuity.ProcessFixtureTests>
    )
    set_tests_properties(
        ForgeConductor.Continuity.ProcessFixtureTests PROPERTIES
        LABELS "T-UNIT;T-DB;T-CONT;T-STRESS;G11"
        TIMEOUT 300
    )

    add_executable(ForgeConductor.ProjectMemory.ApplicationTests
        tests/ProjectMemory/ProjectMemoryApplicationTests.cpp)
    forge_configure_standard_target(ForgeConductor.ProjectMemory.ApplicationTests)
    target_include_directories(ForgeConductor.ProjectMemory.ApplicationTests PRIVATE
        "${PROJECT_SOURCE_DIR}/tests")
    target_link_libraries(ForgeConductor.ProjectMemory.ApplicationTests PRIVATE
        ForgeConductor::Application)

    add_test(
        NAME ForgeConductor.ProjectMemory.ApplicationTests
        COMMAND $<TARGET_FILE:ForgeConductor.ProjectMemory.ApplicationTests>
    )
    set_tests_properties(ForgeConductor.ProjectMemory.ApplicationTests PROPERTIES
        LABELS "T-UNIT;T-MEM;T-SEC;G08"
        TIMEOUT 60
    )

    add_executable(ForgeConductor.ProjectMemory.RepositoryWindowsTests
        tests/ProjectMemory/ProjectMemoryRepositoryWindowsTests.cpp)
    forge_configure_native_target(
        ForgeConductor.ProjectMemory.RepositoryWindowsTests)
    target_include_directories(
        ForgeConductor.ProjectMemory.RepositoryWindowsTests PRIVATE
        "${PROJECT_SOURCE_DIR}/src"
        "${PROJECT_SOURCE_DIR}/tests")
    target_link_libraries(
        ForgeConductor.ProjectMemory.RepositoryWindowsTests PRIVATE
        ForgeConductor::Persistence.Windows)

    add_test(
        NAME ForgeConductor.ProjectMemory.RepositoryWindowsTests
        COMMAND $<TARGET_FILE:ForgeConductor.ProjectMemory.RepositoryWindowsTests>
    )
    set_tests_properties(
        ForgeConductor.ProjectMemory.RepositoryWindowsTests PROPERTIES
        LABELS "T-UNIT;T-DB;T-MEM;T-SEC;G08"
        TIMEOUT 120
    )

    add_executable(ForgeConductor.Continuity.RepositoryWindowsTests
        tests/Continuity/ContinuityRepositoryWindowsTests.cpp)
    forge_configure_native_target(
        ForgeConductor.Continuity.RepositoryWindowsTests)
    target_include_directories(
        ForgeConductor.Continuity.RepositoryWindowsTests PRIVATE
        "${PROJECT_SOURCE_DIR}/src"
        "${PROJECT_SOURCE_DIR}/tests")
    target_link_libraries(
        ForgeConductor.Continuity.RepositoryWindowsTests PRIVATE
        ForgeConductor::Persistence.Windows)

    add_test(
        NAME ForgeConductor.Continuity.RepositoryWindowsTests
        COMMAND $<TARGET_FILE:ForgeConductor.Continuity.RepositoryWindowsTests>
    )
    set_tests_properties(
        ForgeConductor.Continuity.RepositoryWindowsTests PROPERTIES
        LABELS "T-UNIT;T-DB;T-CONT;T-SEC;G11"
        TIMEOUT 180
    )

    add_executable(ForgeConductor.LegacyContinuityPersistence.WindowsTests
        tests/Continuity/LegacyContinuityPersistenceWindowsTests.cpp)
    forge_configure_native_target(
        ForgeConductor.LegacyContinuityPersistence.WindowsTests)
    target_include_directories(
        ForgeConductor.LegacyContinuityPersistence.WindowsTests PRIVATE
        "${PROJECT_SOURCE_DIR}/src"
        "${PROJECT_SOURCE_DIR}/tests")
    target_link_libraries(
        ForgeConductor.LegacyContinuityPersistence.WindowsTests PRIVATE
        ForgeConductor::Persistence.Windows
        ForgeConductor::Infrastructure.Windows
        ForgeConductor::Domain
        winsqlite3)

    add_test(
        NAME ForgeConductor.LegacyContinuityPersistence.WindowsTests
        COMMAND $<TARGET_FILE:ForgeConductor.LegacyContinuityPersistence.WindowsTests>
    )
    set_tests_properties(
        ForgeConductor.LegacyContinuityPersistence.WindowsTests PROPERTIES
        LABELS "T-UNIT;T-DB;T-CONT;T-SEC;T-STRESS;G11;G13"
        TIMEOUT 180
    )

    add_executable(ForgeConductor.ProjectMemory.RegistryWindowsTests
        tests/ProjectMemory/ProjectRegistryWindowsTests.cpp)
    forge_configure_native_target(
        ForgeConductor.ProjectMemory.RegistryWindowsTests)
    target_include_directories(
        ForgeConductor.ProjectMemory.RegistryWindowsTests PRIVATE
        "${PROJECT_SOURCE_DIR}/src"
        "${PROJECT_SOURCE_DIR}/tests")
    target_link_libraries(
        ForgeConductor.ProjectMemory.RegistryWindowsTests PRIVATE
        ForgeConductor::Persistence.Windows)

    add_test(
        NAME ForgeConductor.ProjectMemory.RegistryWindowsTests
        COMMAND $<TARGET_FILE:ForgeConductor.ProjectMemory.RegistryWindowsTests>
    )
    set_tests_properties(
        ForgeConductor.ProjectMemory.RegistryWindowsTests PROPERTIES
        LABELS "T-UNIT;T-DB;T-MEM;T-SEC;G08"
        TIMEOUT 120
    )

    add_executable(ForgeConductor.ProjectMemory.ArtifactWindowsTests
        tests/ProjectMemory/ProjectMemoryArtifactWindowsTests.cpp)
    forge_configure_native_target(
        ForgeConductor.ProjectMemory.ArtifactWindowsTests)
    target_include_directories(
        ForgeConductor.ProjectMemory.ArtifactWindowsTests PRIVATE
        "${PROJECT_SOURCE_DIR}/src"
        "${PROJECT_SOURCE_DIR}/tests")
    target_link_libraries(
        ForgeConductor.ProjectMemory.ArtifactWindowsTests PRIVATE
        ForgeConductor::Persistence.Windows
        nlohmann_json::nlohmann_json)

    add_test(
        NAME ForgeConductor.ProjectMemory.ArtifactWindowsTests
        COMMAND $<TARGET_FILE:ForgeConductor.ProjectMemory.ArtifactWindowsTests>
            "${PROJECT_SOURCE_DIR}/tests/ProjectMemory/Fixtures"
    )
    set_tests_properties(
        ForgeConductor.ProjectMemory.ArtifactWindowsTests PROPERTIES
        LABELS "T-UNIT;T-DB;T-MEM;T-SEC;G08"
        TIMEOUT 180
    )

    add_executable(ForgeConductor.LegacyMemory.ApplicationTests
        tests/LegacyMemory/LegacyMemoryApplicationTests.cpp)
    forge_configure_standard_target(
        ForgeConductor.LegacyMemory.ApplicationTests)
    target_include_directories(
        ForgeConductor.LegacyMemory.ApplicationTests PRIVATE
        "${PROJECT_SOURCE_DIR}/tests")
    target_link_libraries(
        ForgeConductor.LegacyMemory.ApplicationTests PRIVATE
        ForgeConductor::Application)

    add_test(
        NAME ForgeConductor.LegacyMemory.ApplicationTests
        COMMAND $<TARGET_FILE:ForgeConductor.LegacyMemory.ApplicationTests>
    )
    set_tests_properties(
        ForgeConductor.LegacyMemory.ApplicationTests PROPERTIES
        LABELS "T-UNIT;T-MEM;T-SEC;G09"
        TIMEOUT 60
    )

    add_executable(ForgeConductor.LegacyMemory.RepositoryWindowsTests
        tests/LegacyMemory/LegacyMemoryRepositoryWindowsTests.cpp)
    forge_configure_native_target(
        ForgeConductor.LegacyMemory.RepositoryWindowsTests)
    target_include_directories(
        ForgeConductor.LegacyMemory.RepositoryWindowsTests PRIVATE
        "${PROJECT_SOURCE_DIR}/src"
        "${PROJECT_SOURCE_DIR}/tests")
    target_link_libraries(
        ForgeConductor.LegacyMemory.RepositoryWindowsTests PRIVATE
        ForgeConductor::Application
        ForgeConductor::Persistence.Windows
        nlohmann_json::nlohmann_json)

    add_test(
        NAME ForgeConductor.LegacyMemory.RepositoryWindowsTests
        COMMAND $<TARGET_FILE:ForgeConductor.LegacyMemory.RepositoryWindowsTests>
            "${PROJECT_SOURCE_DIR}/tests/Persistence/Fixtures"
    )
    set_tests_properties(
        ForgeConductor.LegacyMemory.RepositoryWindowsTests PROPERTIES
        LABELS "T-UNIT;T-DB;T-MEM;T-SEC;G09"
        TIMEOUT 180
    )

    add_executable(ForgeConductor.NativeTools.FileSystemSearchTests
        tests/NativeTools/WindowsFileSystemSearchTests.cpp)
    forge_configure_native_target(
        ForgeConductor.NativeTools.FileSystemSearchTests)
    target_include_directories(
        ForgeConductor.NativeTools.FileSystemSearchTests PRIVATE
        "${PROJECT_SOURCE_DIR}/src"
        "${PROJECT_SOURCE_DIR}/tests")
    target_link_libraries(
        ForgeConductor.NativeTools.FileSystemSearchTests PRIVATE
        ForgeConductor::NativeTools.Windows)
    add_test(
        NAME ForgeConductor.NativeTools.FileSystemSearchTests
        COMMAND $<TARGET_FILE:ForgeConductor.NativeTools.FileSystemSearchTests>
    )
    set_tests_properties(
        ForgeConductor.NativeTools.FileSystemSearchTests PROPERTIES
        LABELS "T-UNIT;T-INTEGRATION;T-SEC;G13"
        TIMEOUT 180
    )

    add_executable(ForgeConductor.NativeTools.GitShellTests
        tests/NativeTools/WindowsGitShellTests.cpp)
    forge_configure_native_target(ForgeConductor.NativeTools.GitShellTests)
    target_include_directories(
        ForgeConductor.NativeTools.GitShellTests PRIVATE
        "${PROJECT_SOURCE_DIR}/src"
        "${PROJECT_SOURCE_DIR}/tests")
    target_link_libraries(
        ForgeConductor.NativeTools.GitShellTests PRIVATE
        ForgeConductor::NativeTools.Windows)
    add_test(
        NAME ForgeConductor.NativeTools.GitShellTests
        COMMAND $<TARGET_FILE:ForgeConductor.NativeTools.GitShellTests>
    )
    set_tests_properties(
        ForgeConductor.NativeTools.GitShellTests PROPERTIES
        LABELS "T-UNIT;T-PROC;T-SEC;G13"
        TIMEOUT 180
    )

    set(FORGE_P13_GIT_EXECUTABLE
        "$ENV{ProgramFiles}/Git/bin/git.exe")
    if(NOT EXISTS "${FORGE_P13_GIT_EXECUTABLE}")
        find_program(FORGE_P13_GIT_EXECUTABLE_FALLBACK
            NAMES git.exe REQUIRED)
        set(FORGE_P13_GIT_EXECUTABLE
            "${FORGE_P13_GIT_EXECUTABLE_FALLBACK}")
    endif()
    find_program(FORGE_P13_POWERSHELL_EXECUTABLE NAMES pwsh.exe REQUIRED)
    add_executable(ForgeConductor.NativeTools.GitShellIntegrationTests
        tests/NativeTools/WindowsGitShellIntegrationTests.cpp)
    forge_configure_native_target(
        ForgeConductor.NativeTools.GitShellIntegrationTests)
    target_include_directories(
        ForgeConductor.NativeTools.GitShellIntegrationTests PRIVATE
        "${PROJECT_SOURCE_DIR}/src"
        "${PROJECT_SOURCE_DIR}/tests")
    target_link_libraries(
        ForgeConductor.NativeTools.GitShellIntegrationTests PRIVATE
        ForgeConductor::NativeTools.Windows)
    add_test(
        NAME ForgeConductor.NativeTools.GitShellIntegrationTests
        COMMAND $<TARGET_FILE:ForgeConductor.NativeTools.GitShellIntegrationTests>
            "${FORGE_P13_GIT_EXECUTABLE}"
            "${FORGE_P13_POWERSHELL_EXECUTABLE}"
    )
    set_tests_properties(
        ForgeConductor.NativeTools.GitShellIntegrationTests PROPERTIES
        LABELS "T-INTEGRATION;T-PROC;T-SEC;G13"
        TIMEOUT 60
    )

    add_executable(ForgeConductor.NativeTools.PdfTests
        tests/NativeTools/WindowsPdfServiceTests.cpp)
    forge_configure_native_target(ForgeConductor.NativeTools.PdfTests)
    target_include_directories(
        ForgeConductor.NativeTools.PdfTests PRIVATE
        "${PROJECT_SOURCE_DIR}/src"
        "${PROJECT_SOURCE_DIR}/tests")
    target_link_libraries(
        ForgeConductor.NativeTools.PdfTests PRIVATE
        ForgeConductor::NativeTools.Windows
        windowsapp)
    add_test(
        NAME ForgeConductor.NativeTools.PdfTests
        COMMAND $<TARGET_FILE:ForgeConductor.NativeTools.PdfTests>
    )
    set_tests_properties(
        ForgeConductor.NativeTools.PdfTests PROPERTIES
        LABELS "T-UNIT;T-INTEGRATION;T-DOC;T-SEC;G13"
        TIMEOUT 180
    )

    add_executable(ForgeConductor.NativeTools.WorkspaceAuthorityTests
        tests/NativeTools/WindowsWorkspaceAuthorityTests.cpp)
    forge_configure_native_target(
        ForgeConductor.NativeTools.WorkspaceAuthorityTests)
    target_include_directories(
        ForgeConductor.NativeTools.WorkspaceAuthorityTests PRIVATE
        "${PROJECT_SOURCE_DIR}/src"
        "${PROJECT_SOURCE_DIR}/tests")
    target_link_libraries(
        ForgeConductor.NativeTools.WorkspaceAuthorityTests PRIVATE
        ForgeConductor::Infrastructure.Windows)
    add_test(
        NAME ForgeConductor.NativeTools.WorkspaceAuthorityTests
        COMMAND $<TARGET_FILE:ForgeConductor.NativeTools.WorkspaceAuthorityTests>
    )
    set_tests_properties(
        ForgeConductor.NativeTools.WorkspaceAuthorityTests PROPERTIES
        LABELS "T-UNIT;T-SEC;G13"
        TIMEOUT 180
    )

    add_executable(
        ForgeConductor.Infrastructure.ProjectWorkspaceAuthorityTests
        tests/Infrastructure/WindowsProjectWorkspaceAuthorityTests.cpp)
    forge_configure_native_target(
        ForgeConductor.Infrastructure.ProjectWorkspaceAuthorityTests)
    target_include_directories(
        ForgeConductor.Infrastructure.ProjectWorkspaceAuthorityTests PRIVATE
        "${PROJECT_SOURCE_DIR}/src"
        "${PROJECT_SOURCE_DIR}/tests")
    target_link_libraries(
        ForgeConductor.Infrastructure.ProjectWorkspaceAuthorityTests PRIVATE
        ForgeConductor::Infrastructure.Windows)
    add_test(
        NAME ForgeConductor.Infrastructure.ProjectWorkspaceAuthorityTests
        COMMAND $<TARGET_FILE:ForgeConductor.Infrastructure.ProjectWorkspaceAuthorityTests>
    )
    set_tests_properties(
        ForgeConductor.Infrastructure.ProjectWorkspaceAuthorityTests PROPERTIES
        LABELS "G14;T-UNIT;T-INTEGRATION;T-SEC;T-MCP"
        TIMEOUT 180
    )

    add_test(
        NAME ForgeConductor.NativeTools.ContractTests
        COMMAND $<TARGET_FILE:ForgeConductor.Contracts.ContractTests>
    )
    set_tests_properties(
        ForgeConductor.NativeTools.ContractTests PROPERTIES
        LABELS "T-UNIT;G13"
        TIMEOUT 180
    )

    file(GLOB _forge_native_tools_windows_headers CONFIGURE_DEPENDS
        "${PROJECT_SOURCE_DIR}/include/ForgeConductor/NativeTools/Windows/*.h")
    if(NOT _forge_native_tools_windows_headers)
        message(FATAL_ERROR "P13 public native-tools header inventory is empty.")
    endif()
    set(_forge_p13_header_isolation_sources)
    file(MAKE_DIRECTORY
        "${CMAKE_CURRENT_BINARY_DIR}/generated/p13-header-isolation")
    foreach(_header IN LISTS _forge_native_tools_windows_headers)
        file(RELATIVE_PATH _header_relative "${PROJECT_SOURCE_DIR}/include" "${_header}")
        string(REPLACE "\\" "/" _header_relative "${_header_relative}")
        string(MAKE_C_IDENTIFIER "${_header_relative}" _header_identifier)
        set(_isolation_source
            "${CMAKE_CURRENT_BINARY_DIR}/generated/p13-header-isolation/${_header_identifier}.cpp")
        file(GENERATE OUTPUT "${_isolation_source}" CONTENT
            "#include <${_header_relative}>\nint ${_header_identifier}_isolated() noexcept { return 0; }\n")
        list(APPEND _forge_p13_header_isolation_sources "${_isolation_source}")
    endforeach()
    add_library(ForgeConductor.NativeToolsWindows.HeaderObjects OBJECT
        ${_forge_p13_header_isolation_sources})
    forge_configure_native_target(
        ForgeConductor.NativeToolsWindows.HeaderObjects)
    target_link_libraries(
        ForgeConductor.NativeToolsWindows.HeaderObjects PRIVATE
        ForgeConductor::NativeTools.Windows)
    add_executable(ForgeConductor.NativeTools.HeaderSelfContainment
        tests/Architecture/P06HeaderSelfContainmentMain.cpp)
    forge_configure_native_target(
        ForgeConductor.NativeTools.HeaderSelfContainment)
    target_link_libraries(
        ForgeConductor.NativeTools.HeaderSelfContainment PRIVATE
        ForgeConductor.NativeToolsWindows.HeaderObjects
        ForgeConductor::NativeTools.Windows)
    add_test(
        NAME ForgeConductor.NativeTools.HeaderSelfContainment
        COMMAND $<TARGET_FILE:ForgeConductor.NativeTools.HeaderSelfContainment>
    )
    set_tests_properties(
        ForgeConductor.NativeTools.HeaderSelfContainment PROPERTIES
        LABELS "T-UNIT;G13"
        TIMEOUT 60
    )

    add_test(
        NAME ForgeConductor.Cli.SelfTest
        COMMAND $<TARGET_FILE:ForgeConductor.Cli> --self-test
    )
    set_tests_properties(ForgeConductor.Cli.SelfTest PROPERTIES
        LABELS "T-UNIT;G04;G05"
    )

    add_test(
        NAME ForgeConductor.ForsettiHostSmoke
        COMMAND $<TARGET_FILE:ForgeConductor.ForsettiHostSmoke>
            "$<TARGET_FILE_DIR:ForgeConductor.ForsettiHostSmoke>/ForsettiManifests"
    )
    set_tests_properties(ForgeConductor.ForsettiHostSmoke PROPERTIES
        LABELS "T-UNIT;G04"
        WORKING_DIRECTORY "$<TARGET_FILE_DIR:ForgeConductor.ForsettiHostSmoke>"
    )
endif()
