# Forge Conductor — Build and Test Evidence

## PASS: `uname -a`

- Working directory: `/mnt/data/forge_conductor_audit_repo/Forge-Conductor-MacOS-main`
- Exit code: `0`
- Timed out: `False`

```text
Linux a976e5707a86 6.18.35 #1 SMP Mon Jul 27 18:07:50 UTC 2026 x86_64 GNU/Linux
```

## PASS: `swift --version`

- Working directory: `/mnt/data/forge_conductor_audit_repo/Forge-Conductor-MacOS-main`
- Exit code: `0`
- Timed out: `False`

```text
Swift version 6.2.1 (swift-6.2.1-RELEASE)
Target: x86_64-unknown-linux-gnu
```

## FAIL (127): `xcodebuild -version`

- Working directory: `/mnt/data/forge_conductor_audit_repo/Forge-Conductor-MacOS-main`
- Exit code: `127`
- Timed out: `False`

```text
[Errno 2] No such file or directory: 'xcodebuild'
```

## PASS: `git --version`

- Working directory: `/mnt/data/forge_conductor_audit_repo/Forge-Conductor-MacOS-main`
- Exit code: `0`
- Timed out: `False`

```text
git version 2.47.3
```

## PASS: `swift package describe --type json`

- Working directory: `/mnt/data/forge_conductor_audit_repo/Forge-Conductor-MacOS-main`
- Exit code: `0`
- Timed out: `False`

```text
{
  "dependencies" : [

  ],
  "manifest_display_name" : "ForgeConductor",
  "name" : "ForgeConductor",
  "path" : "/mnt/data/forge_conductor_audit_repo/Forge-Conductor-MacOS-main",
  "platforms" : [
    {
      "name" : "macos",
      "version" : "26.0"
    }
  ],
  "products" : [
    {
      "name" : "ForgeConductorCore",
      "targets" : [
        "ForgeConductorCore"
      ],
      "type" : {
        "library" : [
          "automatic"
        ]
      }
    },
    {
      "name" : "forge-conductor",
      "targets" : [
        "ForgeConductorCLI"
      ],
      "type" : {
        "executable" : null
      }
    },
    {
      "name" : "forge-conductor-app",
      "targets" : [
        "ForgeConductorApp"
      ],
      "type" : {
        "executable" : null
      }
    }
  ],
  "swift_languages_versions" : [
    "5"
  ],
  "targets" : [
    {
      "c99name" : "ForgeConductorTests",
      "module_type" : "SwiftTarget",
      "name" : "ForgeConductorTests",
      "path" : "Tests/ForgeConductorTests",
      "resources" : [
        {
          "path" : "/mnt/data/forge_conductor_audit_repo/Forge-Conductor-MacOS-main/Tests/ForgeConductorTests/Fixtures/telemetry_contract_keys.json",
          "rule" : {
            "process" : {

            }
          }
        }
      ],
      "sources" : [
        "AppConfigAndDoctorTests.swift",
        "ContinuityTests.swift",
        "CoreTests.swift",
        "DashboardSecurityTests.swift",
        "DashboardTests.swift",
        "ForgeProcessEntryTests.swift",
        "G1G10AcceptanceTests.swift",
        "HTTPTestHelpers.swift",
        "LMStudioConnectorReliabilityTests.swift",
        "LiveCollectorEvidenceTests.swift",
        "MCPProtocolAndDiagnosticsTests.swift",
        "MachHostMetricsTests.swift",
        "ManagerTests.swift",
        "MemoryToolTests.swift",
        "NativeAppSmokeTests.swift",
        "NativeTelemetryTests.swift",
        "ProcessRunnerTests.swift",
        "ProductPathReliabilityTests.swift",
        "RealtimeStreamTests.swift",
        "RigParityTests.swift",
        "TelemetryContractTests.swift"
      ],
      "target_dependencies" : [
        "ForgeConductorCore",
        "ForgeConductorCLI"
      ],
      "type" : "test"
    },
    {
      "c99name" : "ForgeConductorCore",
      "module_type" : "SwiftTarget",
      "name" : "ForgeConductorCore",
      "path" : "Sources/ForgeConductorCore",
      "product_memberships" : [
        "ForgeConductorCore",
        "forge-conductor",
        "forge-conductor-app"
      ],
      "resources" : [
        {
          "path" : "/mnt/data/forge_conductor_audit_repo/Forge-Conductor-MacOS-main/Sources/ForgeConductorCore/Resources/Agents/debug.md",
          "rule" : {
            "process" : {

            }
          }
        },
        {
          "path" : "/mnt/data/forge_conductor_audit_repo/Forge-Conductor-MacOS-main/Sources/ForgeConductorCore/Resources/Agents/docs.md",
          "rule" : {
            "process" : {

            }
          }
        },
        {
          "path" : "/mnt/data/forge_conductor_audit_repo/Forge-Conductor-MacOS-main/Sources/ForgeConductorCore/Resources/Agents/explore.md",
          "rule" : {
            "process" : {

            }
          }
        },
        {
          "path" : "/mnt/data/forge_conductor_audit_repo/Forge-Conductor-MacOS-main/Sources/ForgeConductorCore/Resources/Agents/implement.md",
          "rule" : {
            "process" : {

            }
          }
        },
        {
          "path" : "/mnt/data/forge_conductor_audit_repo/Forge-Conductor-MacOS-main/Sources/ForgeConductorCore/Resources/Agents/plan.md",
          "rule" : {
            "process" : {

            }
          }
        },
        {
          "path" : "/mnt/data/forge_conductor_audit_repo/Forge-Conductor-MacOS-main/Sources/ForgeConductorCore/Resources/Agents/precommit-audit.md",
          "rule" : {
            "process" : {

            }
          }
        },
        {
          "path" : "/mnt/data/forge_conductor_audit_repo/Forge-Conductor-MacOS-main/Sources/ForgeConductorCore/Resources/Agents/research.md",
          "rule" : {
            "process" : {

            }
          }
        },
        {
          "path" : "/mnt/data/forge_conductor_audit_repo/Forge-Conductor-MacOS-main/Sources/ForgeConductorCore/Resources/Agents/review.md",
          "rule" : {
            "process" : {

            }
          }
        },
        {
          "path" : "/mnt/data/forge_conductor_audit_repo/Forge-Conductor-MacOS-main/Sources/ForgeConductorCore/Resources/Agents/security.md",
          "rule" : {
            "process" : {

            }
          }
        },
        {
          "path" : "/mnt/data/forge_conductor_audit_repo/Forge-Conductor-MacOS-main/Sources/ForgeConductorCore/Resources/Agents/test.md",
          "rule" : {
            "process" : {

            }
          }
        },
        {
          "path" : "/mnt/data/forge_conductor_audit_repo/Forge-Conductor-MacOS-main/Sources/ForgeConductorCore/Resources/TelemetryStatic/app.js",
          "rule" : {
            "process" : {

            }
          }
        },
        {
          "path" : "/mnt/data/forge_conductor_audit_repo/Forge-Conductor-MacOS-main/Sources/ForgeConductorCore/Resources/TelemetryStatic/index.html",
          "rule" : {
            "process" : {

            }
          }
        },
        {
          "path" : "/mnt/data/forge_conductor_audit_repo/Forge-Conductor-MacOS-main/Sources/ForgeConductorCore/Resources/TelemetryStatic/style.css",
          "rule" : {
            "process" : {

            }
          }
        },
        {
          "path" : "/mnt/data/forge_conductor_audit_repo/Forge-Conductor-MacOS-main/Sources/ForgeConductorCore/Resources/TelemetryStatic/tools-catalog.js",
          "rule" : {
            "process" : {

            }
          }
        }
      ],
      "sources" : [
        "Application/AgentCatalog.swift",
        "Application/AgentSessionService.swift",
        "Application/ContextContinuityService.swift",
        "Application/ContinuityAutomation.swift",
        "Application/ForgeApp.swift",
        "Application/ForgeProcessEntry.swift",
        "Application/ToolAuthorizationService.swift",
        "Application/ToolRouter.swift",
        "Application/Tools/AgentToolPack.swift",
        "Application/Tools/ContinuityToolPack.swift",
        "Application/Tools/DocsToolPack.swift",
        "Application/Tools/FilesystemToolPack.swift",
        "Application/Tools/GitToolPack.swift",
        "Application/Tools/MemoryToolPack.swift",
        "Application/Tools/SearchToolPack.swift",
        "Application/Tools/ShellToolPack.swift",
        "Application/Tools/ToolArgHelpers.swift",
        "Dashboard/DashboardHTML.swift",
        "Dashboard/DashboardHTTPRequest.swift",
        "Dashboard/DashboardServer.swift",
        "Dashboard/HTTPResponder.swift",
        "Dashboard/ManagerRoutes.swift",
        "Dashboard/OperationalRoutes.swift",
        "Dashboard/TelemetryRoutes.swift",
        "Domain/AppConfig.swift",
        "Domain/DoctorModels.swift",
        "Domain/ForgeSnapshot.swift",
        "Domain/HandoffPacket.swift",
        "Domain/JSONSupport.swift",
        "Domain/LMStudioConnector.swift",
        "Domain/ManagerModels.swift",
        "Domain/Models.swift",
        "Domain/Protocols.swift",
        "Infrastructure/AppPaths.swift",
        "Infrastructure/AuditService.swift",
        "Infrastructure/ConfigStore.swift",
        "Infrastructure/DashboardPortGuard.swift",
        "Infrastructure/DiagnosticLog.swift",
        "Infrastructure/PDFWriter.swift",
        "Infrastructure/ProcessRunner.swift",
        "Infrastructure/ResourceBundle.swift",
        "Infrastructure/SQLiteStore.swift",
        "MCP/MCPServeVerifier.swift",
        "MCP/MCPServer.swift",
        "Manager/ManagerDashboardClient.swift",
        "Manager/ManagerInstaller.swift",
        "Manager/ManagerNode.swift",
        "Manager/ManagerPIDFile.swift",
        "Manager/ManagerRuntime.swift",
        "Manager/ManagerSettingsNormalizer.swift",
        "Telemetry/Collectors/CPUCollector.swift",
        "Telemetry/Collectors/CPUFrequencyEstimator.swift",
        "Telemetry/Collectors/DiskIOCollector.swift",
        "Telemetry/Collectors/DiskVolumeCollector.swift",
        "Telemetry/Collectors/GPUCollector.swift",
        "Telemetry/Collectors/IOKitPropertyWalk.swift",
        "Telemetry/Collectors/PowerSourcesCollector.swift",
        "Telemetry/Collectors/ProcessMetricsCollector.swift",
        "Telemetry/Collectors/RAMCollector.swift",
        "Telemetry/ForgeCollector.swift",
        "Telemetry/LMStudioDeployService.swift",
        "Telemetry/LMStudioEnvironment.swift",
        "Telemetry/LMStudioMCPPluginInstaller.swift",
        "Telemetry/Models/ForgeUIModels.swift",
        "Telemetry/Models/TelemetryModels.swift",
        "Telemetry/ProcessDiscovery.swift",
        "Telemetry/RealtimeMetricsEngine.swift",
        "Telemetry/SystemCollector.swift",
        "Telemetry/TelemetryService.swift"
      ],
      "type" : "library"
    },
    {
      "c99name" : "ForgeConductorCLI",
      "module_type" : "SwiftTarget",
      "name" : "ForgeConductorCLI",
      "path" : "Sources/ForgeConductorCLI",
      "product_memberships" : [
        "forge-conductor"
      ],
      "sources" : [
        "ForgeConductorMain.swift"
      ],
      "target_dependencies" : [
        "ForgeConductorCore"
      ],
      "type" : "executable"
    },
    {
      "c99name" : "ForgeConductorApp",
      "module_type" : "SwiftTarget",
      "name" : "ForgeConductorApp",
      "path" : "Sources/ForgeConductorApp",
      "product_memberships" : [
        "forge-conductor-app"
      ],
      "sources" : [
        "AppDeployController.swift",
        "AppModel.swift",
        "AppTelemetryBinding.swift",
        "ForgeConductorApp.swift",
        "Metal/LoadTraceRenderer.swift",
        "Metal/MetalGaugeKit.swift",
        "Metal/MetalLoadChart.swift",
        "Metal/MetalMeterView.swift",
        "Metal/MultiSeriesLoadRenderer.swift",
        "Views/AgentsView.swift",
        "Views/AppSidebarView.swift",
        "Views/ContentView.swift",
        "Views/DiagnosticsView.swift",
        "Views/LiveFeedView.swift",
        "Views/MCPServersView.swift",
        "Views/ManagerSettingsView.swift",
        "Views/Rig/RigDashboardView.swift",
        "Views/TelemetryDashboardView.swift",
        "Views/ToolsView.swift"
      ],
      "target_dependencies" : [
        "ForgeConductorCore"
      ],
      "type" : "executable"
    }
  ],
  "tools_version" : "6.2"
}
```

## FAIL (1): `swift build`

- Working directory: `/mnt/data/forge_conductor_audit_repo/Forge-Conductor-MacOS-main`
- Exit code: `1`
- Timed out: `False`

```text
[0/1] Planning build
Building for debugging...
[0/8] Write swift-version--1BA0962812E73E12.txt
error: emit-module command failed with exit code 1 (use -v to see invocation)
[2/65] Emitting module ForgeConductorCore
/mnt/data/forge_conductor_audit_repo/Forge-Conductor-MacOS-main/Sources/ForgeConductorCore/Application/ContextContinuityService.swift:8:8: error: no such module 'Darwin'
  6 | 
  7 | import Foundation
  8 | import Darwin
    |        `- error: no such module 'Darwin'
  9 | 
 10 | /// Context + agent continuity control plane (stdio MCP / same serve binary).
[3/79] Compiling ForgeConductorCore SearchToolPack.swift
/mnt/data/forge_conductor_audit_repo/Forge-Conductor-MacOS-main/Sources/ForgeConductorCore/Application/ContextContinuityService.swift:8:8: error: no such module 'Darwin'
  6 | 
  7 | import Foundation
  8 | import Darwin
    |        `- error: no such module 'Darwin'
  9 | 
 10 | /// Context + agent continuity control plane (stdio MCP / same serve binary).
[4/79] Compiling ForgeConductorCore ShellToolPack.swift
/mnt/data/forge_conductor_audit_repo/Forge-Conductor-MacOS-main/Sources/ForgeConductorCore/Application/ContextContinuityService.swift:8:8: error: no such module 'Darwin'
  6 | 
  7 | import Foundation
  8 | import Darwin
    |        `- error: no such module 'Darwin'
  9 | 
 10 | /// Context + agent continuity control plane (stdio MCP / same serve binary).
[5/79] Compiling ForgeConductorCore ToolArgHelpers.swift
/mnt/data/forge_conductor_audit_repo/Forge-Conductor-MacOS-main/Sources/ForgeConductorCore/Application/ContextContinuityService.swift:8:8: error: no such module 'Darwin'
  6 | 
  7 | import Foundation
  8 | import Darwin
    |        `- error: no such module 'Darwin'
  9 | 
 10 | /// Context + agent continuity control plane (stdio MCP / same serve binary).
[6/79] Compiling ForgeConductorCore DashboardHTML.swift
/mnt/data/forge_conductor_audit_repo/Forge-Conductor-MacOS-main/Sources/ForgeConductorCore/Application/ContextContinuityService.swift:8:8: error: no such module 'Darwin'
  6 | 
  7 | import Foundation
  8 | import Darwin
    |        `- error: no such module 'Darwin'
  9 | 
 10 | /// Context + agent continuity control plane (stdio MCP / same serve binary).
[7/79] Compiling ForgeConductorCore DashboardHTTPRequest.swift
/mnt/data/forge_conductor_audit_repo/Forge-Conductor-MacOS-main/Sources/ForgeConductorCore/Application/ContextContinuityService.swift:8:8: error: no such module 'Darwin'
  6 | 
  7 | import Foundation
  8 | import Darwin
    |        `- error: no such module 'Darwin'
  9 | 
 10 | /// Context + agent continuity control plane (stdio MCP / same serve binary).
[8/79] Compiling ForgeConductorCore DashboardServer.swift
/mnt/data/forge_conductor_audit_repo/Forge-Conductor-MacOS-main/Sources/ForgeConductorCore/Application/ContextContinuityService.swift:8:8: error: no such module 'Darwin'
  6 | 
  7 | import Foundation
  8 | import Darwin
    |        `- error: no such module 'Darwin'
  9 | 
 10 | /// Context + agent continuity control plane (stdio MCP / same serve binary).
[9/79] Compiling ForgeConductorCore HTTPResponder.swift
/mnt/data/forge_conductor_audit_repo/Forge-Conductor-MacOS-main/Sources/ForgeConductorCore/Application/ContextContinuityService.swift:8:8: error: no such module 'Darwin'
  6 | 
  7 | import Foundation
  8 | import Darwin
    |        `- error: no such module 'Darwin'
  9 | 
 10 | /// Context + agent continuity control plane (stdio MCP / same serve binary).
[10/79] Compiling ForgeConductorCore ManagerRoutes.swift
/mnt/data/forge_conductor_audit_repo/Forge-Conductor-MacOS-main/Sources/ForgeConductorCore/Application/ContextContinuityService.swift:8:8: error: no such module 'Darwin'
  6 | 
  7 | import Foundation
  8 | import Darwin
    |        `- error: no such module 'Darwin'
  9 | 
 10 | /// Context + agent continuity control plane (stdio MCP / same serve binary).
[11/79] Compiling ForgeConductorCore OperationalRoutes.swift
/mnt/data/forge_conductor_audit_repo/Forge-Conductor-MacOS-main/Sources/ForgeConductorCore/Application/ContextContinuityService.swift:8:8: error: no such module 'Darwin'
  6 | 
  7 | import Foundation
  8 | import Darwin
    |        `- error: no such module 'Darwin'
  9 | 
 10 | /// Context + agent continuity control plane (stdio MCP / same serve binary).
[12/79] Compiling ForgeConductorCore TelemetryRoutes.swift
/mnt/data/forge_conductor_audit_repo/Forge-Conductor-MacOS-main/Sources/ForgeConductorCore/Application/ContextContinuityService.swift:8:8: error: no such module 'Darwin'
  6 | 
  7 | import Foundation
  8 | import Darwin
    |        `- error: no such module 'Darwin'
  9 | 
 10 | /// Context + agent continuity control plane (stdio MCP / same serve binary).
[13/79] Compiling ForgeConductorCore AppConfig.swift
/mnt/data/forge_conductor_audit_repo/Forge-Conductor-MacOS-main/Sources/ForgeConductorCore/Application/ContextContinuityService.swift:8:8: error: no such module 'Darwin'
  6 | 
  7 | import Foundation
  8 | import Darwin
    |        `- error: no such module 'Darwin'
  9 | 
 10 | /// Context + agent continuity control plane (stdio MCP / same serve binary).
[14/79] Compiling ForgeConductorCore DoctorModels.swift
/mnt/data/forge_conductor_audit_repo/Forge-Conductor-MacOS-main/Sources/ForgeConductorCore/Application/ContextContinuityService.swift:8:8: error: no such module 'Darwin'
  6 | 
  7 | import Foundation
  8 | import Darwin
    |        `- error: no such module 'Darwin'
  9 | 
 10 | /// Context + agent continuity control plane (stdio MCP / same serve binary).
[15/79] Compiling ForgeConductorCore ForgeSnapshot.swift
/mnt/data/forge_conductor_audit_repo/Forge-Conductor-MacOS-main/Sources/ForgeConductorCore/Application/ContextContinuityService.swift:8:8: error: no such module 'Darwin'
  6 | 
  7 | import Foundation
  8 | import Darwin
    |        `- error: no such module 'Darwin'
  9 | 
 10 | /// Context + agent continuity control plane (stdio MCP / same serve binary).
[16/79] Compiling ForgeConductorCore HandoffPacket.swift
/mnt/data/forge_conductor_audit_repo/Forge-Conductor-MacOS-main/Sources/ForgeConductorCore/Application/ContextContinuityService.swift:8:8: error: no such module 'Darwin'
  6 | 
  7 | import Foundation
  8 | import Darwin
    |        `- error: no such module 'Darwin'
  9 | 
 10 | /// Context + agent continuity control plane (stdio MCP / same serve binary).
[17/79] Compiling ForgeConductorCore AgentCatalog.swift
/mnt/data/forge_conductor_audit_repo/Forge-Conductor-MacOS-main/Sources/ForgeConductorCore/Application/ContextContinuityService.swift:8:8: error: no such module 'Darwin'
  6 | 
  7 | import Foundation
  8 | import Darwin
    |        `- error: no such module 'Darwin'
  9 | 
 10 | /// Context + agent continuity control plane (stdio MCP / same serve binary).
[18/79] Compiling ForgeConductorCore AgentSessionService.swift
/mnt/data/forge_conductor_audit_repo/Forge-Conductor-MacOS-main/Sources/ForgeConductorCore/Application/ContextContinuityService.swift:8:8: error: no such module 'Darwin'
  6 | 
  7 | import Foundation
  8 | import Darwin
    |        `- error: no such module 'Darwin'
  9 | 
 10 | /// Context + agent continuity control plane (stdio MCP / same serve binary).
[19/79] Compiling ForgeConductorCore ContextContinuityService.swift
/mnt/data/forge_conductor_audit_repo/Forge-Conductor-MacOS-main/Sources/ForgeConductorCore/Application/ContextContinuityService.swift:8:8: error: no such module 'Darwin'
  6 | 
  7 | import Foundation
  8 | import Darwin
    |        `- error: no such module 'Darwin'
  9 | 
 10 | /// Context + agent continuity control plane (stdio MCP / same serve binary).
[20/79] Compiling ForgeConductorCore ContinuityAutomation.swift
/mnt/data/forge_conductor_audit_repo/Forge-Conductor-MacOS-main/Sources/ForgeConductorCore/Application/ContextContinuityService.swift:8:8: error: no such module 'Darwin'
  6 | 
  7 | import Foundation
  8 | import Darwin
    |        `- error: no such module 'Darwin'
  9 | 
 10 | /// Context + agent continuity control plane (stdio MCP / same serve binary).
[21/79] Compiling ForgeConductorCore ForgeApp.swift
/mnt/data/forge_conductor_audit_repo/Forge-Conductor-MacOS-main/Sources/ForgeConductorCore/Application/ContextContinuityService.swift:8:8: error: no such module 'Darwin'
  6 | 
  7 | import Foundation
  8 | import Darwin
    |        `- error: no such module 'Darwin'
  9 | 
 10 | /// Context + agent continuity control plane (stdio MCP / same serve binary).
[22/79] Compiling ForgeConductorCore ForgeProcessEntry.swift
/mnt/data/forge_conductor_audit_repo/Forge-Conductor-MacOS-main/Sources/ForgeConductorCore/Application/ContextContinuityService.swift:8:8: error: no such module 'Darwin'
  6 | 
  7 | import Foundation
  8 | import Darwin
    |        `- error: no such module 'Darwin'
  9 | 
 10 | /// Context + agent continuity control plane (stdio MCP / same serve binary).
[23/79] Compiling ForgeConductorCore ToolAuthorizationService.swift
/mnt/data/forge_conductor_audit_repo/Forge-Conductor-MacOS-main/Sources/ForgeConductorCore/Application/ContextContinuityService.swift:8:8: error: no such module 'Darwin'
  6 | 
  7 | import Foundation
  8 | import Darwin
    |        `- error: no such module 'Darwin'
  9 | 
 10 | /// Context + agent continuity control plane (stdio MCP / same serve binary).
[24/79] Compiling ForgeConductorCore ToolRouter.swift
/mnt/data/forge_conductor_audit_repo/Forge-Conductor-MacOS-main/Sources/ForgeConductorCore/Application/ContextContinuityService.swift:8:8: error: no such module 'Darwin'
  6 | 
  7 | import Foundation
  8 | import Darwin
    |        `- error: no such module 'Darwin'
  9 | 
 10 | /// Context + agent continuity control plane (stdio MCP / same serve binary).
[25/79] Compiling ForgeConductorCore AgentToolPack.swift
/mnt/data/forge_conductor_audit_repo/Forge-Conductor-MacOS-main/Sources/ForgeConductorCore/Application/ContextContinuityService.swift:8:8: error: no such module 'Darwin'
  6 | 
  7 | import Foundation
  8 | import Darwin
    |        `- error: no such module 'Darwin'
  9 | 
 10 | /// Context + agent continuity control plane (stdio MCP / same serve binary).
[26/79] Compiling ForgeConductorCore ContinuityToolPack.swift
/mnt/data/forge_conductor_audit_repo/Forge-Conductor-MacOS-main/Sources/ForgeConductorCore/Application/ContextContinuityService.swift:8:8: error: no such module 'Darwin'
  6 | 
  7 | import Foundation
  8 | import Darwin
    |        `- error: no such module 'Darwin'
  9 | 
 10 | /// Context + agent continuity control plane (stdio MCP / same serve binary).
[27/79] Compiling ForgeConductorCore DocsToolPack.swift
/mnt/data/forge_conductor_audit_repo/Forge-Conductor-MacOS-main/Sources/ForgeConductorCore/Application/ContextContinuityService.swift:8:8: error: no such module 'Darwin'
  6 | 
  7 | import Foundation
  8 | import Darwin
    |        `- error: no such module 'Darwin'
  9 | 
 10 | /// Context + agent continuity control plane (stdio MCP / same serve binary).
[28/79] Compiling ForgeConductorCore FilesystemToolPack.swift
/mnt/data/forge_conductor_audit_repo/Forge-Conductor-MacOS-main/Sources/ForgeConductorCore/Application/ContextContinuityService.swift:8:8: error: no such module 'Darwin'
  6 | 
  7 | import Foundation
  8 | import Darwin
    |        `- error: no such module 'Darwin'
  9 | 
 10 | /// Context + agent continuity control plane (stdio MCP / same serve binary).
[29/79] Compiling ForgeConductorCore GitToolPack.swift
/mnt/data/forge_conductor_audit_repo/Forge-Conductor-MacOS-main/Sources/ForgeConductorCore/Application/ContextContinuityService.swift:8:8: error: no such module 'Darwin'
  6 | 
  7 | import Foundation
  8 | import Darwin
    |        `- error: no such module 'Darwin'
  9 | 
 10 | /// Context + agent continuity control plane (stdio MCP / same serve binary).
[30/79] Compiling ForgeConductorCore MemoryToolPack.swift
/mnt/data/forge_conductor_audit_repo/Forge-Conductor-MacOS-main/Sources/ForgeConductorCore/Application/ContextContinuityService.swift:8:8: error: no such module 'Darwin'
  6 | 
  7 | import Foundation
  8 | import Darwin
    |        `- error: no such module 'Darwin'
  9 | 
 10 | /// Context + agent continuity control plane (stdio MCP / same serve binary).
[31/79] Compiling ForgeConductorCore PowerSourcesCollector.swift
/mnt/data/forge_conductor_audit_repo/Forge-Conductor-MacOS-main/Sources/ForgeConductorCore/Application/ContextContinuityService.swift:8:8: error: no such module 'Darwin'
  6 | 
  7 | import Foundation
  8 | import Darwin
    |        `- error: no such module 'Darwin'
  9 | 
 10 | /// Context + agent continuity control plane (stdio MCP / same serve binary).
[32/79] Compiling ForgeConductorCore ProcessMetricsCollector.swift
/mnt/data/forge_conductor_audit_repo/Forge-Conductor-MacOS-main/Sources/ForgeConductorCore/Application/ContextContinuityService.swift:8:8: error: no such module 'Darwin'
  6 | 
  7 | import Foundation
  8 | import Darwin
    |        `- error: no such module 'Darwin'
  9 | 
 10 | /// Context + agent continuity control plane (stdio MCP / same serve binary).
[33/79] Compiling ForgeConductorCore RAMCollector.swift
/mnt/data/forge_conductor_audit_repo/Forge-Conductor-MacOS-main/Sources/ForgeConductorCore/Application/ContextContinuityService.swift:8:8: error: no such module 'Darwin'
  6 | 
  7 | import Foundation
  8 | import Darwin
    |        `- error: no such module 'Darwin'
  9 | 
 10 | /// Context + agent continuity control plane (stdio MCP / same serve binary).
[34/79] Compiling ForgeConductorCore ForgeCollector.swift
/mnt/data/forge_conductor_audit_repo/Forge-Conductor-MacOS-main/Sources/ForgeConductorCore/Application/ContextContinuityService.swift:8:8: error: no such module 'Darwin'
  6 | 
  7 | import Foundation
  8 | import Darwin
    |        `- error: no such module 'Darwin'
  9 | 
 10 | /// Context + agent continuity control plane (stdio MCP / same serve binary).
[35/79] Compiling ForgeConductorCore LMStudioDeployService.swift
/mnt/data/forge_conductor_audit_repo/Forge-Conductor-MacOS-main/Sources/ForgeConductorCore/Application/ContextContinuityService.swift:8:8: error: no such module 'Darwin'
  6 | 
  7 | import Foundation
  8 | import Darwin
    |        `- error: no such module 'Darwin'
  9 | 
 10 | /// Context + agent continuity control plane (stdio MCP / same serve binary).
[36/79] Compiling ForgeConductorCore LMStudioEnvironment.swift
/mnt/data/forge_conductor_audit_repo/Forge-Conductor-MacOS-main/Sources/ForgeConductorCore/Application/ContextContinuityService.swift:8:8: error: no such module 'Darwin'
  6 | 
  7 | import Foundation
  8 | import Darwin
    |        `- error: no such module 'Darwin'
  9 | 
 10 | /// Context + agent continuity control plane (stdio MCP / same serve binary).
[37/79] Compiling ForgeConductorCore LMStudioMCPPluginInstaller.swift
/mnt/data/forge_conductor_audit_repo/Forge-Conductor-MacOS-main/Sources/ForgeConductorCore/Application/ContextContinuityService.swift:8:8: error: no such module 'Darwin'
  6 | 
  7 | import Foundation
  8 | import Darwin
    |        `- error: no such module 'Darwin'
  9 | 
 10 | /// Context + agent continuity control plane (stdio MCP / same serve binary).
[38/79] Compiling ForgeConductorCore ForgeUIModels.swift
/mnt/data/forge_conductor_audit_repo/Forge-Conductor-MacOS-main/Sources/ForgeConductorCore/Application/ContextContinuityService.swift:8:8: error: no such module 'Darwin'
  6 | 
  7 | import Foundation
  8 | import Darwin
    |        `- error: no such module 'Darwin'
  9 | 
 10 | /// Context + agent continuity control plane (stdio MCP / same serve binary).
[39/79] Compiling ForgeConductorCore TelemetryModels.swift
/mnt/data/forge_conductor_audit_repo/Forge-Conductor-MacOS-main/Sources/ForgeConductorCore/Application/ContextContinuityService.swift:8:8: error: no such module 'Darwin'
  6 | 
  7 | import Foundation
  8 | import Darwin
    |        `- error: no such module 'Darwin'
  9 | 
 10 | /// Context + agent continuity control plane (stdio MCP / same serve binary).
[40/79] Compiling ForgeConductorCore ProcessDiscovery.swift
/mnt/data/forge_conductor_audit_repo/Forge-Conductor-MacOS-main/Sources/ForgeConductorCore/Application/ContextContinuityService.swift:8:8: error: no such module 'Darwin'
  6 | 
  7 | import Foundation
  8 | import Darwin
    |        `- error: no such module 'Darwin'
  9 | 
 10 | /// Context + agent continuity control plane (stdio MCP / same serve binary).
[41/79] Compiling ForgeConductorCore RealtimeMetricsEngine.swift
/mnt/data/forge_conductor_audit_repo/Forge-Conductor-MacOS-main/Sources/ForgeConductorCore/Application/ContextContinuityService.swift:8:8: error: no such module 'Darwin'
  6 | 
  7 | import Foundation
  8 | import Darwin
    |        `- error: no such module 'Darwin'
  9 | 
 10 | /// Context + agent continuity control plane (stdio MCP / same serve binary).
[42/79] Compiling ForgeConductorCore SystemCollector.swift
/mnt/data/forge_conductor_audit_repo/Forge-Conductor-MacOS-main/Sources/ForgeConductorCore/Application/ContextContinuityService.swift:8:8: error: no such module 'Darwin'
  6 | 
  7 | import Foundation
  8 | import Darwin
    |        `- error: no such module 'Darwin'
  9 | 
 10 | /// Context + agent continuity control plane (stdio MCP / same serve binary).
[43/79] Compiling ForgeConductorCore TelemetryService.swift
/mnt/data/forge_conductor_audit_repo/Forge-Conductor-MacOS-main/Sources/ForgeConductorCore/Application/ContextContinuityService.swift:8:8: error: no such module 'Darwin'
  6 | 
  7 | import Foundation
  8 | import Darwin
    |        `- error: no such module 'Darwin'
  9 | 
 10 | /// Context + agent continuity control plane (stdio MCP / same serve binary).
[44/79] Compiling ForgeConductorCore resource_bundle_accessor.swift
/mnt/data/forge_conductor_audit_repo/Forge-Conductor-MacOS-main/Sources/ForgeConductorCore/Application/ContextContinuityService.swift:8:8: error: no such module 'Darwin'
  6 | 
  7 | import Foundation
  8 | import Darwin
    |        `- error: no such module 'Darwin'
  9 | 
 10 | /// Context + agent continuity control plane (stdio MCP / same serve binary).
[45/79] Compiling ForgeConductorCore JSONSupport.swift
/mnt/data/forge_conductor_audit_repo/Forge-Conductor-MacOS-main/Sources/ForgeConductorCore/Application/ContextContinuityService.swift:8:8: error: no such module 'Darwin'
  6 | 
  7 | import Foundation
  8 | import Darwin
    |        `- error: no such module 'Darwin'
  9 | 
 10 | /// Context + agent continuity control plane (stdio MCP / same serve binary).
[46/79] Compiling ForgeConductorCore LMStudioConnector.swift
/mnt/data/forge_conductor_audit_repo/Forge-Conductor-MacOS-main/Sources/ForgeConductorCore/Application/ContextContinuityService.swift:8:8: error: no such module 'Darwin'
  6 | 
  7 | import Foundation
  8 | import Darwin
    |        `- error: no such module 'Darwin'
  9 | 
 10 | /// Context + agent continuity control plane (stdio MCP / same serve binary).
[47/79] Compiling ForgeConductorCore ManagerModels.swift
/mnt/data/forge_conductor_audit_repo/Forge-Conductor-MacOS-main/Sources/ForgeConductorCore/Application/ContextContinuityService.swift:8:8: error: no such module 'Darwin'
  6 | 
  7 | import Foundation
  8 | import Darwin
    |        `- error: no such module 'Darwin'
  9 | 
 10 | /// Context + agent continuity control plane (stdio MCP / same serve binary).
[48/79] Compiling ForgeConductorCore Models.swift
/mnt/data/forge_conductor_audit_repo/Forge-Conductor-MacOS-main/Sources/ForgeConductorCore/Application/ContextContinuityService.swift:8:8: error: no such module 'Darwin'
  6 | 
  7 | import Foundation
  8 | import Darwin
    |        `- error: no such module 'Darwin'
  9 | 
 10 | /// Context + agent continuity control plane (stdio MCP / same serve binary).
[49/79] Compiling ForgeConductorCore Protocols.swift
/mnt/data/forge_conductor_audit_repo/Forge-Conductor-MacOS-main/Sources/ForgeConductorCore/Application/ContextContinuityService.swift:8:8: error: no such module 'Darwin'
  6 | 
  7 | import Foundation
  8 | import Darwin
    |        `- error: no such module 'Darwin'
  9 | 
 10 | /// Context + agent continuity control plane (stdio MCP / same serve binary).
[50/79] Compiling ForgeConductorCore AppPaths.swift
/mnt/data/forge_conductor_audit_repo/Forge-Conductor-MacOS-main/Sources/ForgeConductorCore/Application/ContextContinuityService.swift:8:8: error: no such module 'Darwin'
  6 | 
  7 | import Foundation
  8 | import Darwin
    |        `- error: no such module 'Darwin'
  9 | 
 10 | /// Context + agent continuity control plane (stdio MCP / same serve binary).
[51/79] Compiling ForgeConductorCore AuditService.swift
/mnt/data/forge_conductor_audit_repo/Forge-Conductor-MacOS-main/Sources/ForgeConductorCore/Application/ContextContinuityService.swift:8:8: error: no such module 'Darwin'
  6 | 
  7 | import Foundation
  8 | import Darwin
    |        `- error: no such module 'Darwin'
  9 | 
 10 | /// Context + agent continuity control plane (stdio MCP / same serve binary).
[52/79] Compiling ForgeConductorCore ConfigStore.swift
/mnt/data/forge_conductor_audit_repo/Forge-Conductor-MacOS-main/Sources/ForgeConductorCore/Application/ContextContinuityService.swift:8:8: error: no such module 'Darwin'
  6 | 
  7 | import Foundation
  8 | import Darwin
    |        `- error: no such module 'Darwin'
  9 | 
 10 | /// Context + agent continuity control plane (stdio MCP / same serve binary).
[53/79] Compiling ForgeConductorCore DashboardPortGuard.swift
/mnt/data/forge_conductor_audit_repo/Forge-Conductor-MacOS-main/Sources/ForgeConductorCore/Application/ContextContinuityService.swift:8:8: error: no such module 'Darwin'
  6 | 
  7 | import Foundation
  8 | import Darwin
    |        `- error: no such module 'Darwin'
  9 | 
 10 | /// Context + agent continuity control plane (stdio MCP / same serve binary).
[54/79] Compiling ForgeConductorCore DiagnosticLog.swift
/mnt/data/forge_conductor_audit_repo/Forge-Conductor-MacOS-main/Sources/ForgeConductorCore/Application/ContextContinuityService.swift:8:8: error: no such module 'Darwin'
  6 | 
  7 | import Foundation
  8 | import Darwin
    |        `- error: no such module 'Darwin'
  9 | 
 10 | /// Context + agent continuity control plane (stdio MCP / same serve binary).
[55/79] Compiling ForgeConductorCore PDFWriter.swift
/mnt/data/forge_conductor_audit_repo/Forge-Conductor-MacOS-main/Sources/ForgeConductorCore/Application/ContextContinuityService.swift:8:8: error: no such module 'Darwin'
  6 | 
  7 | import Foundation
  8 | import Darwin
    |        `- error: no such module 'Darwin'
  9 | 
 10 | /// Context + agent continuity control plane (stdio MCP / same serve binary).
[56/79] Compiling ForgeConductorCore ProcessRunner.swift
/mnt/data/forge_conductor_audit_repo/Forge-Conductor-MacOS-main/Sources/ForgeConductorCore/Application/ContextContinuityService.swift:8:8: error: no such module 'Darwin'
  6 | 
  7 | import Foundation
  8 | import Darwin
    |        `- error: no such module 'Darwin'
  9 | 
 10 | /// Context + agent continuity control plane (stdio MCP / same serve binary).
[57/79] Compiling ForgeConductorCore ResourceBundle.swift
/mnt/data/forge_conductor_audit_repo/Forge-Conductor-MacOS-main/Sources/ForgeConductorCore/Application/ContextContinuityService.swift:8:8: error: no such module 'Darwin'
  6 | 
  7 | import Foundation
  8 | import Darwin
    |        `- error: no such module 'Darwin'
  9 | 
 10 | /// Context + agent continuity control plane (stdio MCP / same serve binary).
[58/79] Compiling ForgeConductorCore SQLiteStore.swift
/mnt/data/forge_conductor_audit_repo/Forge-Conductor-MacOS-main/Sources/ForgeConductorCore/Application/ContextContinuityService.swift:8:8: error: no such module 'Darwin'
  6 | 
  7 | import Foundation
  8 | import Darwin
    |        `- error: no such module 'Darwin'
  9 | 
 10 | /// Context + agent continuity control plane (stdio MCP / same serve binary).
[59/79] Compiling ForgeConductorCore MCPServeVerifier.swift
/mnt/data/forge_conductor_audit_repo/Forge-Conductor-MacOS-main/Sources/ForgeConductorCore/Application/ContextContinuityService.swift:8:8: error: no such module 'Darwin'
  6 | 
  7 | import Foundation
  8 | import Darwin
    |        `- error: no such module 'Darwin'
  9 | 
 10 | /// Context + agent continuity control plane (stdio MCP / same serve binary).
[60/79] Compiling ForgeConductorCore MCPServer.swift
/mnt/data/forge_conductor_audit_repo/Forge-Conductor-MacOS-main/Sources/ForgeConductorCore/Application/ContextContinuityService.swift:8:8: error: no such module 'Darwin'
  6 | 
  7 | import Foundation
  8 | import Darwin
    |        `- error: no such module 'Darwin'
  9 | 
 10 | /// Context + agent continuity control plane (stdio MCP / same serve binary).
[61/79] Compiling ForgeConductorCore ManagerDashboardClient.swift
/mnt/data/forge_conductor_audit_repo/Forge-Conductor-MacOS-main/Sources/ForgeConductorCore/Application/ContextContinuityService.swift:8:8: error: no such module 'Darwin'
  6 | 
  7 | import Foundation
  8 | import Darwin
    |        `- error: no such module 'Darwin'
  9 | 
 10 | /// Context + agent continuity control plane (stdio MCP / same serve binary).
[62/79] Compiling ForgeConductorCore ManagerInstaller.swift
/mnt/data/forge_conductor_audit_repo/Forge-Conductor-MacOS-main/Sources/ForgeConductorCore/Application/ContextContinuityService.swift:8:8: error: no such module 'Darwin'
  6 | 
  7 | import Foundation
  8 | import Darwin
    |        `- error: no such module 'Darwin'
  9 | 
 10 | /// Context + agent continuity control plane (stdio MCP / same serve binary).
[63/79] Compiling ForgeConductorCore ManagerNode.swift
/mnt/data/forge_conductor_audit_repo/Forge-Conductor-MacOS-main/Sources/ForgeConductorCore/Application/ContextContinuityService.swift:8:8: error: no such module 'Darwin'
  6 | 
  7 | import Foundation
  8 | import Darwin
    |        `- error: no such module 'Darwin'
  9 | 
 10 | /// Context + agent continuity control plane (stdio MCP / same serve binary).
[64/79] Compiling ForgeConductorCore ManagerPIDFile.swift
/mnt/data/forge_conductor_audit_repo/Forge-Conductor-MacOS-main/Sources/ForgeConductorCore/Application/ContextContinuityService.swift:8:8: error: no such module 'Darwin'
  6 | 
  7 | import Foundation
  8 | import Darwin
    |        `- error: no such module 'Darwin'
  9 | 
 10 | /// Context + agent continuity control plane (stdio MCP / same serve binary).
[65/79] Compiling ForgeConductorCore ManagerRuntime.swift
/mnt/data/forge_conductor_audit_repo/Forge-Conductor-MacOS-main/Sources/ForgeConductorCore/Application/ContextContinuityService.swift:8:8: error: no such module 'Darwin'
  6 | 
  7 | import Foundation
  8 | import Darwin
    |        `- error: no such module 'Darwin'
  9 | 
 10 | /// Context + agent continuity control plane (stdio MCP / same serve binary).
[66/79] Compiling ForgeConductorCore ManagerSettingsNormalizer.swift
/mnt/data/forge_conductor_audit_repo/Forge-Conductor-MacOS-main/Sources/ForgeConductorCore/Application/ContextContinuityService.swift:8:8: error: no such module 'Darwin'
  6 | 
  7 | import Foundation
  8 | import Darwin
    |        `- error: no such module 'Darwin'
  9 | 
 10 | /// Context + agent continuity control plane (stdio MCP / same serve binary).
[67/79] Compiling ForgeConductorCore CPUCollector.swift
/mnt/data/forge_conductor_audit_repo/Forge-Conductor-MacOS-main/Sources/ForgeConductorCore/Application/ContextContinuityService.swift:8:8: error: no such module 'Darwin'
  6 | 
  7 | import Foundation
  8 | import Darwin
    |        `- error: no such module 'Darwin'
  9 | 
 10 | /// Context + agent continuity control plane (stdio MCP / same serve binary).
[68/79] Compiling ForgeConductorCore CPUFrequencyEstimator.swift
/mnt/data/forge_conductor_audit_repo/Forge-Conductor-MacOS-main/Sources/ForgeConductorCore/Application/ContextContinuityService.swift:8:8: error: no such module 'Darwin'
  6 | 
  7 | import Foundation
  8 | import Darwin
    |        `- error: no such module 'Darwin'
  9 | 
 10 | /// Context + agent continuity control plane (stdio MCP / same serve binary).
[69/79] Compiling ForgeConductorCore DiskIOCollector.swift
/mnt/data/forge_conductor_audit_repo/Forge-Conductor-MacOS-main/Sources/ForgeConductorCore/Application/ContextContinuityService.swift:8:8: error: no such module 'Darwin'
  6 | 
  7 | import Foundation
  8 | import Darwin
    |        `- error: no such module 'Darwin'
  9 | 
 10 | /// Context + agent continuity control plane (stdio MCP / same serve binary).
[70/79] Compiling ForgeConductorCore DiskVolumeCollector.swift
/mnt/data/forge_conductor_audit_repo/Forge-Conductor-MacOS-main/Sources/ForgeConductorCore/Application/ContextContinuityService.swift:8:8: error: no such module 'Darwin'
  6 | 
  7 | import Foundation
  8 | import Darwin
    |        `- error: no such module 'Darwin'
  9 | 
 10 | /// Context + agent continuity control plane (stdio MCP / same serve binary).
[71/79] Compiling ForgeConductorCore GPUCollector.swift
/mnt/data/forge_conductor_audit_repo/Forge-Conductor-MacOS-main/Sources/ForgeConductorCore/Application/ContextContinuityService.swift:8:8: error: no such module 'Darwin'
  6 | 
  7 | import Foundation
  8 | import Darwin
    |        `- error: no such module 'Darwin'
  9 | 
 10 | /// Context + agent continuity control plane (stdio MCP / same serve binary).
[72/79] Compiling ForgeConductorCore IOKitPropertyWalk.swift
/mnt/data/forge_conductor_audit_repo/Forge-Conductor-MacOS-main/Sources/ForgeConductorCore/Application/ContextContinuityService.swift:8:8: error: no such module 'Darwin'
  6 | 
  7 | import Foundation
  8 | import Darwin
    |        `- error: no such module 'Darwin'
  9 | 
 10 | /// Context + agent continuity control plane (stdio MCP / same serve binary).
```

## FAIL (1): `swift test --parallel`

- Working directory: `/mnt/data/forge_conductor_audit_repo/Forge-Conductor-MacOS-main`
- Exit code: `1`
- Timed out: `False`

```text
[0/1] Planning build
Building for debugging...
[0/17] Write swift-version--1BA0962812E73E12.txt
error: emit-module command failed with exit code 1 (use -v to see invocation)
[2/74] Emitting module ForgeConductorCore
/mnt/data/forge_conductor_audit_repo/Forge-Conductor-MacOS-main/Sources/ForgeConductorCore/Application/ContextContinuityService.swift:8:8: error: no such module 'Darwin'
  6 | 
  7 | import Foundation
  8 | import Darwin
    |        `- error: no such module 'Darwin'
  9 | 
 10 | /// Context + agent continuity control plane (stdio MCP / same serve binary).
[3/88] Compiling ForgeConductorCore JSONSupport.swift
/mnt/data/forge_conductor_audit_repo/Forge-Conductor-MacOS-main/Sources/ForgeConductorCore/Application/ContextContinuityService.swift:8:8: error: no such module 'Darwin'
  6 | 
  7 | import Foundation
  8 | import Darwin
    |        `- error: no such module 'Darwin'
  9 | 
 10 | /// Context + agent continuity control plane (stdio MCP / same serve binary).
[4/88] Compiling ForgeConductorCore LMStudioConnector.swift
/mnt/data/forge_conductor_audit_repo/Forge-Conductor-MacOS-main/Sources/ForgeConductorCore/Application/ContextContinuityService.swift:8:8: error: no such module 'Darwin'
  6 | 
  7 | import Foundation
  8 | import Darwin
    |        `- error: no such module 'Darwin'
  9 | 
 10 | /// Context + agent continuity control plane (stdio MCP / same serve binary).
[5/88] Compiling ForgeConductorCore ManagerModels.swift
/mnt/data/forge_conductor_audit_repo/Forge-Conductor-MacOS-main/Sources/ForgeConductorCore/Application/ContextContinuityService.swift:8:8: error: no such module 'Darwin'
  6 | 
  7 | import Foundation
  8 | import Darwin
    |        `- error: no such module 'Darwin'
  9 | 
 10 | /// Context + agent continuity control plane (stdio MCP / same serve binary).
[6/88] Compiling ForgeConductorCore Models.swift
/mnt/data/forge_conductor_audit_repo/Forge-Conductor-MacOS-main/Sources/ForgeConductorCore/Application/ContextContinuityService.swift:8:8: error: no such module 'Darwin'
  6 | 
  7 | import Foundation
  8 | import Darwin
    |        `- error: no such module 'Darwin'
  9 | 
 10 | /// Context + agent continuity control plane (stdio MCP / same serve binary).
[7/88] Compiling ForgeConductorCore Protocols.swift
/mnt/data/forge_conductor_audit_repo/Forge-Conductor-MacOS-main/Sources/ForgeConductorCore/Application/ContextContinuityService.swift:8:8: error: no such module 'Darwin'
  6 | 
  7 | import Foundation
  8 | import Darwin
    |        `- error: no such module 'Darwin'
  9 | 
 10 | /// Context + agent continuity control plane (stdio MCP / same serve binary).
[8/88] Compiling ForgeConductorCore AppPaths.swift
/mnt/data/forge_conductor_audit_repo/Forge-Conductor-MacOS-main/Sources/ForgeConductorCore/Application/ContextContinuityService.swift:8:8: error: no such module 'Darwin'
  6 | 
  7 | import Foundation
  8 | import Darwin
    |        `- error: no such module 'Darwin'
  9 | 
 10 | /// Context + agent continuity control plane (stdio MCP / same serve binary).
[9/88] Compiling ForgeConductorCore AuditService.swift
/mnt/data/forge_conductor_audit_repo/Forge-Conductor-MacOS-main/Sources/ForgeConductorCore/Application/ContextContinuityService.swift:8:8: error: no such module 'Darwin'
  6 | 
  7 | import Foundation
  8 | import Darwin
    |        `- error: no such module 'Darwin'
  9 | 
 10 | /// Context + agent continuity control plane (stdio MCP / same serve binary).
[10/88] Compiling ForgeConductorCore ConfigStore.swift
/mnt/data/forge_conductor_audit_repo/Forge-Conductor-MacOS-main/Sources/ForgeConductorCore/Application/ContextContinuityService.swift:8:8: error: no such module 'Darwin'
  6 | 
  7 | import Foundation
  8 | import Darwin
    |        `- error: no such module 'Darwin'
  9 | 
 10 | /// Context + agent continuity control plane (stdio MCP / same serve binary).
[11/88] Compiling ForgeConductorCore DashboardPortGuard.swift
/mnt/data/forge_conductor_audit_repo/Forge-Conductor-MacOS-main/Sources/ForgeConductorCore/Application/ContextContinuityService.swift:8:8: error: no such module 'Darwin'
  6 | 
  7 | import Foundation
  8 | import Darwin
    |        `- error: no such module 'Darwin'
  9 | 
 10 | /// Context + agent continuity control plane (stdio MCP / same serve binary).
[12/88] Compiling ForgeConductorCore DiagnosticLog.swift
/mnt/data/forge_conductor_audit_repo/Forge-Conductor-MacOS-main/Sources/ForgeConductorCore/Application/ContextContinuityService.swift:8:8: error: no such module 'Darwin'
  6 | 
  7 | import Foundation
  8 | import Darwin
    |        `- error: no such module 'Darwin'
  9 | 
 10 | /// Context + agent continuity control plane (stdio MCP / same serve binary).
[13/88] Compiling ForgeConductorCore PDFWriter.swift
/mnt/data/forge_conductor_audit_repo/Forge-Conductor-MacOS-main/Sources/ForgeConductorCore/Application/ContextContinuityService.swift:8:8: error: no such module 'Darwin'
  6 | 
  7 | import Foundation
  8 | import Darwin
    |        `- error: no such module 'Darwin'
  9 | 
 10 | /// Context + agent continuity control plane (stdio MCP / same serve binary).
[14/88] Compiling ForgeConductorCore ProcessRunner.swift
/mnt/data/forge_conductor_audit_repo/Forge-Conductor-MacOS-main/Sources/ForgeConductorCore/Application/ContextContinuityService.swift:8:8: error: no such module 'Darwin'
  6 | 
  7 | import Foundation
  8 | import Darwin
    |        `- error: no such module 'Darwin'
  9 | 
 10 | /// Context + agent continuity control plane (stdio MCP / same serve binary).
[15/88] Compiling ForgeConductorCore ResourceBundle.swift
/mnt/data/forge_conductor_audit_repo/Forge-Conductor-MacOS-main/Sources/ForgeConductorCore/Application/ContextContinuityService.swift:8:8: error: no such module 'Darwin'
  6 | 
  7 | import Foundation
  8 | import Darwin
    |        `- error: no such module 'Darwin'
  9 | 
 10 | /// Context + agent continuity control plane (stdio MCP / same serve binary).
[16/88] Compiling ForgeConductorCore SQLiteStore.swift
/mnt/data/forge_conductor_audit_repo/Forge-Conductor-MacOS-main/Sources/ForgeConductorCore/Application/ContextContinuityService.swift:8:8: error: no such module 'Darwin'
  6 | 
  7 | import Foundation
  8 | import Darwin
    |        `- error: no such module 'Darwin'
  9 | 
 10 | /// Context + agent continuity control plane (stdio MCP / same serve binary).
[17/88] Compiling ForgeConductorCore AgentCatalog.swift
/mnt/data/forge_conductor_audit_repo/Forge-Conductor-MacOS-main/Sources/ForgeConductorCore/Application/ContextContinuityService.swift:8:8: error: no such module 'Darwin'
  6 | 
  7 | import Foundation
  8 | import Darwin
    |        `- error: no such module 'Darwin'
  9 | 
 10 | /// Context + agent continuity control plane (stdio MCP / same serve binary).
[18/88] Compiling ForgeConductorCore AgentSessionService.swift
/mnt/data/forge_conductor_audit_repo/Forge-Conductor-MacOS-main/Sources/ForgeConductorCore/Application/ContextContinuityService.swift:8:8: error: no such module 'Darwin'
  6 | 
  7 | import Foundation
  8 | import Darwin
    |        `- error: no such module 'Darwin'
  9 | 
 10 | /// Context + agent continuity control plane (stdio MCP / same serve binary).
[19/88] Compiling ForgeConductorCore ContextContinuityService.swift
/mnt/data/forge_conductor_audit_repo/Forge-Conductor-MacOS-main/Sources/ForgeConductorCore/Application/ContextContinuityService.swift:8:8: error: no such module 'Darwin'
  6 | 
  7 | import Foundation
  8 | import Darwin
    |        `- error: no such module 'Darwin'
  9 | 
 10 | /// Context + agent continuity control plane (stdio MCP / same serve binary).
[20/88] Compiling ForgeConductorCore ContinuityAutomation.swift
/mnt/data/forge_conductor_audit_repo/Forge-Conductor-MacOS-main/Sources/ForgeConductorCore/Application/ContextContinuityService.swift:8:8: error: no such module 'Darwin'
  6 | 
  7 | import Foundation
  8 | import Darwin
    |        `- error: no such module 'Darwin'
  9 | 
 10 | /// Context + agent continuity control plane (stdio MCP / same serve binary).
[21/88] Compiling ForgeConductorCore ForgeApp.swift
/mnt/data/forge_conductor_audit_repo/Forge-Conductor-MacOS-main/Sources/ForgeConductorCore/Application/ContextContinuityService.swift:8:8: error: no such module 'Darwin'
  6 | 
  7 | import Foundation
  8 | import Darwin
    |        `- error: no such module 'Darwin'
  9 | 
 10 | /// Context + agent continuity control plane (stdio MCP / same serve binary).
[22/88] Compiling ForgeConductorCore ForgeProcessEntry.swift
/mnt/data/forge_conductor_audit_repo/Forge-Conductor-MacOS-main/Sources/ForgeConductorCore/Application/ContextContinuityService.swift:8:8: error: no such module 'Darwin'
  6 | 
  7 | import Foundation
  8 | import Darwin
    |        `- error: no such module 'Darwin'
  9 | 
 10 | /// Context + agent continuity control plane (stdio MCP / same serve binary).
[23/88] Compiling ForgeConductorCore ToolAuthorizationService.swift
/mnt/data/forge_conductor_audit_repo/Forge-Conductor-MacOS-main/Sources/ForgeConductorCore/Application/ContextContinuityService.swift:8:8: error: no such module 'Darwin'
  6 | 
  7 | import Foundation
  8 | import Darwin
    |        `- error: no such module 'Darwin'
  9 | 
 10 | /// Context + agent continuity control plane (stdio MCP / same serve binary).
[24/88] Compiling ForgeConductorCore ToolRouter.swift
/mnt/data/forge_conductor_audit_repo/Forge-Conductor-MacOS-main/Sources/ForgeConductorCore/Application/ContextContinuityService.swift:8:8: error: no such module 'Darwin'
  6 | 
  7 | import Foundation
  8 | import Darwin
    |        `- error: no such module 'Darwin'
  9 | 
 10 | /// Context + agent continuity control plane (stdio MCP / same serve binary).
[25/88] Compiling ForgeConductorCore AgentToolPack.swift
/mnt/data/forge_conductor_audit_repo/Forge-Conductor-MacOS-main/Sources/ForgeConductorCore/Application/ContextContinuityService.swift:8:8: error: no such module 'Darwin'
  6 | 
  7 | import Foundation
  8 | import Darwin
    |        `- error: no such module 'Darwin'
  9 | 
 10 | /// Context + agent continuity control plane (stdio MCP / same serve binary).
[26/88] Compiling ForgeConductorCore ContinuityToolPack.swift
/mnt/data/forge_conductor_audit_repo/Forge-Conductor-MacOS-main/Sources/ForgeConductorCore/Application/ContextContinuityService.swift:8:8: error: no such module 'Darwin'
  6 | 
  7 | import Foundation
  8 | import Darwin
    |        `- error: no such module 'Darwin'
  9 | 
 10 | /// Context + agent continuity control plane (stdio MCP / same serve binary).
[27/88] Compiling ForgeConductorCore DocsToolPack.swift
/mnt/data/forge_conductor_audit_repo/Forge-Conductor-MacOS-main/Sources/ForgeConductorCore/Application/ContextContinuityService.swift:8:8: error: no such module 'Darwin'
  6 | 
  7 | import Foundation
  8 | import Darwin
    |        `- error: no such module 'Darwin'
  9 | 
 10 | /// Context + agent continuity control plane (stdio MCP / same serve binary).
[28/88] Compiling ForgeConductorCore FilesystemToolPack.swift
/mnt/data/forge_conductor_audit_repo/Forge-Conductor-MacOS-main/Sources/ForgeConductorCore/Application/ContextContinuityService.swift:8:8: error: no such module 'Darwin'
  6 | 
  7 | import Foundation
  8 | import Darwin
    |        `- error: no such module 'Darwin'
  9 | 
 10 | /// Context + agent continuity control plane (stdio MCP / same serve binary).
[29/88] Compiling ForgeConductorCore GitToolPack.swift
/mnt/data/forge_conductor_audit_repo/Forge-Conductor-MacOS-main/Sources/ForgeConductorCore/Application/ContextContinuityService.swift:8:8: error: no such module 'Darwin'
  6 | 
  7 | import Foundation
  8 | import Darwin
    |        `- error: no such module 'Darwin'
  9 | 
 10 | /// Context + agent continuity control plane (stdio MCP / same serve binary).
[30/88] Compiling ForgeConductorCore MemoryToolPack.swift
/mnt/data/forge_conductor_audit_repo/Forge-Conductor-MacOS-main/Sources/ForgeConductorCore/Application/ContextContinuityService.swift:8:8: error: no such module 'Darwin'
  6 | 
  7 | import Foundation
  8 | import Darwin
    |        `- error: no such module 'Darwin'
  9 | 
 10 | /// Context + agent continuity control plane (stdio MCP / same serve binary).
[31/88] Compiling ForgeConductorCore PowerSourcesCollector.swift
/mnt/data/forge_conductor_audit_repo/Forge-Conductor-MacOS-main/Sources/ForgeConductorCore/Application/ContextContinuityService.swift:8:8: error: no such module 'Darwin'
  6 | 
  7 | import Foundation
  8 | import Darwin
    |        `- error: no such module 'Darwin'
  9 | 
 10 | /// Context + agent continuity control plane (stdio MCP / same serve binary).
[32/88] Compiling ForgeConductorCore ProcessMetricsCollector.swift
/mnt/data/forge_conductor_audit_repo/Forge-Conductor-MacOS-main/Sources/ForgeConductorCore/Application/ContextContinuityService.swift:8:8: error: no such module 'Darwin'
  6 | 
  7 | import Foundation
  8 | import Darwin
    |        `- error: no such module 'Darwin'
  9 | 
 10 | /// Context + agent continuity control plane (stdio MCP / same serve binary).
[33/88] Compiling ForgeConductorCore RAMCollector.swift
/mnt/data/forge_conductor_audit_repo/Forge-Conductor-MacOS-main/Sources/ForgeConductorCore/Application/ContextContinuityService.swift:8:8: error: no such module 'Darwin'
  6 | 
  7 | import Foundation
  8 | import Darwin
    |        `- error: no such module 'Darwin'
  9 | 
 10 | /// Context + agent continuity control plane (stdio MCP / same serve binary).
[34/88] Compiling ForgeConductorCore ForgeCollector.swift
/mnt/data/forge_conductor_audit_repo/Forge-Conductor-MacOS-main/Sources/ForgeConductorCore/Application/ContextContinuityService.swift:8:8: error: no such module 'Darwin'
  6 | 
  7 | import Foundation
  8 | import Darwin
    |        `- error: no such module 'Darwin'
  9 | 
 10 | /// Context + agent continuity control plane (stdio MCP / same serve binary).
[35/88] Compiling ForgeConductorCore LMStudioDeployService.swift
/mnt/data/forge_conductor_audit_repo/Forge-Conductor-MacOS-main/Sources/ForgeConductorCore/Application/ContextContinuityService.swift:8:8: error: no such module 'Darwin'
  6 | 
  7 | import Foundation
  8 | import Darwin
    |        `- error: no such module 'Darwin'
  9 | 
 10 | /// Context + agent continuity control plane (stdio MCP / same serve binary).
[36/88] Compiling ForgeConductorCore LMStudioEnvironment.swift
/mnt/data/forge_conductor_audit_repo/Forge-Conductor-MacOS-main/Sources/ForgeConductorCore/Application/ContextContinuityService.swift:8:8: error: no such module 'Darwin'
  6 | 
  7 | import Foundation
  8 | import Darwin
    |        `- error: no such module 'Darwin'
  9 | 
 10 | /// Context + agent continuity control plane (stdio MCP / same serve binary).
[37/88] Compiling ForgeConductorCore LMStudioMCPPluginInstaller.swift
/mnt/data/forge_conductor_audit_repo/Forge-Conductor-MacOS-main/Sources/ForgeConductorCore/Application/ContextContinuityService.swift:8:8: error: no such module 'Darwin'
  6 | 
  7 | import Foundation
  8 | import Darwin
    |        `- error: no such module 'Darwin'
  9 | 
 10 | /// Context + agent continuity control plane (stdio MCP / same serve binary).
[38/88] Compiling ForgeConductorCore ForgeUIModels.swift
/mnt/data/forge_conductor_audit_repo/Forge-Conductor-MacOS-main/Sources/ForgeConductorCore/Application/ContextContinuityService.swift:8:8: error: no such module 'Darwin'
  6 | 
  7 | import Foundation
  8 | import Darwin
    |        `- error: no such module 'Darwin'
  9 | 
 10 | /// Context + agent continuity control plane (stdio MCP / same serve binary).
[39/88] Compiling ForgeConductorCore TelemetryModels.swift
/mnt/data/forge_conductor_audit_repo/Forge-Conductor-MacOS-main/Sources/ForgeConductorCore/Application/ContextContinuityService.swift:8:8: error: no such module 'Darwin'
  6 | 
  7 | import Foundation
  8 | import Darwin
    |        `- error: no such module 'Darwin'
  9 | 
 10 | /// Context + agent continuity control plane (stdio MCP / same serve binary).
[40/88] Compiling ForgeConductorCore ProcessDiscovery.swift
/mnt/data/forge_conductor_audit_repo/Forge-Conductor-MacOS-main/Sources/ForgeConductorCore/Application/ContextContinuityService.swift:8:8: error: no such module 'Darwin'
  6 | 
  7 | import Foundation
  8 | import Darwin
    |        `- error: no such module 'Darwin'
  9 | 
 10 | /// Context + agent continuity control plane (stdio MCP / same serve binary).
[41/88] Compiling ForgeConductorCore RealtimeMetricsEngine.swift
/mnt/data/forge_conductor_audit_repo/Forge-Conductor-MacOS-main/Sources/ForgeConductorCore/Application/ContextContinuityService.swift:8:8: error: no such module 'Darwin'
  6 | 
  7 | import Foundation
  8 | import Darwin
    |        `- error: no such module 'Darwin'
  9 | 
 10 | /// Context + agent continuity control plane (stdio MCP / same serve binary).
[42/88] Compiling ForgeConductorCore SystemCollector.swift
/mnt/data/forge_conductor_audit_repo/Forge-Conductor-MacOS-main/Sources/ForgeConductorCore/Application/ContextContinuityService.swift:8:8: error: no such module 'Darwin'
  6 | 
  7 | import Foundation
  8 | import Darwin
    |        `- error: no such module 'Darwin'
  9 | 
 10 | /// Context + agent continuity control plane (stdio MCP / same serve binary).
[43/88] Compiling ForgeConductorCore TelemetryService.swift
/mnt/data/forge_conductor_audit_repo/Forge-Conductor-MacOS-main/Sources/ForgeConductorCore/Application/ContextContinuityService.swift:8:8: error: no such module 'Darwin'
  6 | 
  7 | import Foundation
  8 | import Darwin
    |        `- error: no such module 'Darwin'
  9 | 
 10 | /// Context + agent continuity control plane (stdio MCP / same serve binary).
[44/88] Compiling ForgeConductorCore resource_bundle_accessor.swift
/mnt/data/forge_conductor_audit_repo/Forge-Conductor-MacOS-main/Sources/ForgeConductorCore/Application/ContextContinuityService.swift:8:8: error: no such module 'Darwin'
  6 | 
  7 | import Foundation
  8 | import Darwin
    |        `- error: no such module 'Darwin'
  9 | 
 10 | /// Context + agent continuity control plane (stdio MCP / same serve binary).
[45/88] Compiling ForgeConductorCore SearchToolPack.swift
/mnt/data/forge_conductor_audit_repo/Forge-Conductor-MacOS-main/Sources/ForgeConductorCore/Application/ContextContinuityService.swift:8:8: error: no such module 'Darwin'
  6 | 
  7 | import Foundation
  8 | import Darwin
    |        `- error: no such module 'Darwin'
  9 | 
 10 | /// Context + agent continuity control plane (stdio MCP / same serve binary).
[46/88] Compiling ForgeConductorCore ShellToolPack.swift
/mnt/data/forge_conductor_audit_repo/Forge-Conductor-MacOS-main/Sources/ForgeConductorCore/Application/ContextContinuityService.swift:8:8: error: no such module 'Darwin'
  6 | 
  7 | import Foundation
  8 | import Darwin
    |        `- error: no such module 'Darwin'
  9 | 
 10 | /// Context + agent continuity control plane (stdio MCP / same serve binary).
[47/88] Compiling ForgeConductorCore ToolArgHelpers.swift
/mnt/data/forge_conductor_audit_repo/Forge-Conductor-MacOS-main/Sources/ForgeConductorCore/Application/ContextContinuityService.swift:8:8: error: no such module 'Darwin'
  6 | 
  7 | import Foundation
  8 | import Darwin
    |        `- error: no such module 'Darwin'
  9 | 
 10 | /// Context + agent continuity control plane (stdio MCP / same serve binary).
[48/88] Compiling ForgeConductorCore DashboardHTML.swift
/mnt/data/forge_conductor_audit_repo/Forge-Conductor-MacOS-main/Sources/ForgeConductorCore/Application/ContextContinuityService.swift:8:8: error: no such module 'Darwin'
  6 | 
  7 | import Foundation
  8 | import Darwin
    |        `- error: no such module 'Darwin'
  9 | 
 10 | /// Context + agent continuity control plane (stdio MCP / same serve binary).
[49/88] Compiling ForgeConductorCore DashboardHTTPRequest.swift
/mnt/data/forge_conductor_audit_repo/Forge-Conductor-MacOS-main/Sources/ForgeConductorCore/Application/ContextContinuityService.swift:8:8: error: no such module 'Darwin'
  6 | 
  7 | import Foundation
  8 | import Darwin
    |        `- error: no such module 'Darwin'
  9 | 
 10 | /// Context + agent continuity control plane (stdio MCP / same serve binary).
[50/88] Compiling ForgeConductorCore DashboardServer.swift
/mnt/data/forge_conductor_audit_repo/Forge-Conductor-MacOS-main/Sources/ForgeConductorCore/Application/ContextContinuityService.swift:8:8: error: no such module 'Darwin'
  6 | 
  7 | import Foundation
  8 | import Darwin
    |        `- error: no such module 'Darwin'
  9 | 
 10 | /// Context + agent continuity control plane (stdio MCP / same serve binary).
[51/88] Compiling ForgeConductorCore HTTPResponder.swift
/mnt/data/forge_conductor_audit_repo/Forge-Conductor-MacOS-main/Sources/ForgeConductorCore/Application/ContextContinuityService.swift:8:8: error: no such module 'Darwin'
  6 | 
  7 | import Foundation
  8 | import Darwin
    |        `- error: no such module 'Darwin'
  9 | 
 10 | /// Context + agent continuity control plane (stdio MCP / same serve binary).
[52/88] Compiling ForgeConductorCore ManagerRoutes.swift
/mnt/data/forge_conductor_audit_repo/Forge-Conductor-MacOS-main/Sources/ForgeConductorCore/Application/ContextContinuityService.swift:8:8: error: no such module 'Darwin'
  6 | 
  7 | import Foundation
  8 | import Darwin
    |        `- error: no such module 'Darwin'
  9 | 
 10 | /// Context + agent continuity control plane (stdio MCP / same serve binary).
[53/88] Compiling ForgeConductorCore OperationalRoutes.swift
/mnt/data/forge_conductor_audit_repo/Forge-Conductor-MacOS-main/Sources/ForgeConductorCore/Application/ContextContinuityService.swift:8:8: error: no such module 'Darwin'
  6 | 
  7 | import Foundation
  8 | import Darwin
    |        `- error: no such module 'Darwin'
  9 | 
 10 | /// Context + agent continuity control plane (stdio MCP / same serve binary).
[54/88] Compiling ForgeConductorCore TelemetryRoutes.swift
/mnt/data/forge_conductor_audit_repo/Forge-Conductor-MacOS-main/Sources/ForgeConductorCore/Application/ContextContinuityService.swift:8:8: error: no such module 'Darwin'
  6 | 
  7 | import Foundation
  8 | import Darwin
    |        `- error: no such module 'Darwin'
  9 | 
 10 | /// Context + agent continuity control plane (stdio MCP / same serve binary).
[55/88] Compiling ForgeConductorCore AppConfig.swift
/mnt/data/forge_conductor_audit_repo/Forge-Conductor-MacOS-main/Sources/ForgeConductorCore/Application/ContextContinuityService.swift:8:8: error: no such module 'Darwin'
  6 | 
  7 | import Foundation
  8 | import Darwin
    |        `- error: no such module 'Darwin'
  9 | 
 10 | /// Context + agent continuity control plane (stdio MCP / same serve binary).
[56/88] Compiling ForgeConductorCore DoctorModels.swift
/mnt/data/forge_conductor_audit_repo/Forge-Conductor-MacOS-main/Sources/ForgeConductorCore/Application/ContextContinuityService.swift:8:8: error: no such module 'Darwin'
  6 | 
  7 | import Foundation
  8 | import Darwin
    |        `- error: no such module 'Darwin'
  9 | 
 10 | /// Context + agent continuity control plane (stdio MCP / same serve binary).
[57/88] Compiling ForgeConductorCore ForgeSnapshot.swift
/mnt/data/forge_conductor_audit_repo/Forge-Conductor-MacOS-main/Sources/ForgeConductorCore/Application/ContextContinuityService.swift:8:8: error: no such module 'Darwin'
  6 | 
  7 | import Foundation
  8 | import Darwin
    |        `- error: no such module 'Darwin'
  9 | 
 10 | /// Context + agent continuity control plane (stdio MCP / same serve binary).
[58/88] Compiling ForgeConductorCore HandoffPacket.swift
/mnt/data/forge_conductor_audit_repo/Forge-Conductor-MacOS-main/Sources/ForgeConductorCore/Application/ContextContinuityService.swift:8:8: error: no such module 'Darwin'
  6 | 
  7 | import Foundation
  8 | import Darwin
    |        `- error: no such module 'Darwin'
  9 | 
 10 | /// Context + agent continuity control plane (stdio MCP / same serve binary).
[59/88] Compiling ForgeConductorCore MCPServeVerifier.swift
/mnt/data/forge_conductor_audit_repo/Forge-Conductor-MacOS-main/Sources/ForgeConductorCore/Application/ContextContinuityService.swift:8:8: error: no such module 'Darwin'
  6 | 
  7 | import Foundation
  8 | import Darwin
    |        `- error: no such module 'Darwin'
  9 | 
 10 | /// Context + agent continuity control plane (stdio MCP / same serve binary).
[60/88] Compiling ForgeConductorCore MCPServer.swift
/mnt/data/forge_conductor_audit_repo/Forge-Conductor-MacOS-main/Sources/ForgeConductorCore/Application/ContextContinuityService.swift:8:8: error: no such module 'Darwin'
  6 | 
  7 | import Foundation
  8 | import Darwin
    |        `- error: no such module 'Darwin'
  9 | 
 10 | /// Context + agent continuity control plane (stdio MCP / same serve binary).
[61/88] Compiling ForgeConductorCore ManagerDashboardClient.swift
/mnt/data/forge_conductor_audit_repo/Forge-Conductor-MacOS-main/Sources/ForgeConductorCore/Application/ContextContinuityService.swift:8:8: error: no such module 'Darwin'
  6 | 
  7 | import Foundation
  8 | import Darwin
    |        `- error: no such module 'Darwin'
  9 | 
 10 | /// Context + agent continuity control plane (stdio MCP / same serve binary).
[62/88] Compiling ForgeConductorCore ManagerInstaller.swift
/mnt/data/forge_conductor_audit_repo/Forge-Conductor-MacOS-main/Sources/ForgeConductorCore/Application/ContextContinuityService.swift:8:8: error: no such module 'Darwin'
  6 | 
  7 | import Foundation
  8 | import Darwin
    |        `- error: no such module 'Darwin'
  9 | 
 10 | /// Context + agent continuity control plane (stdio MCP / same serve binary).
[63/88] Compiling ForgeConductorCore ManagerNode.swift
/mnt/data/forge_conductor_audit_repo/Forge-Conductor-MacOS-main/Sources/ForgeConductorCore/Application/ContextContinuityService.swift:8:8: error: no such module 'Darwin'
  6 | 
  7 | import Foundation
  8 | import Darwin
    |        `- error: no such module 'Darwin'
  9 | 
 10 | /// Context + agent continuity control plane (stdio MCP / same serve binary).
[64/88] Compiling ForgeConductorCore ManagerPIDFile.swift
/mnt/data/forge_conductor_audit_repo/Forge-Conductor-MacOS-main/Sources/ForgeConductorCore/Application/ContextContinuityService.swift:8:8: error: no such module 'Darwin'
  6 | 
  7 | import Foundation
  8 | import Darwin
    |        `- error: no such module 'Darwin'
  9 | 
 10 | /// Context + agent continuity control plane (stdio MCP / same serve binary).
[65/88] Compiling ForgeConductorCore ManagerRuntime.swift
/mnt/data/forge_conductor_audit_repo/Forge-Conductor-MacOS-main/Sources/ForgeConductorCore/Application/ContextContinuityService.swift:8:8: error: no such module 'Darwin'
  6 | 
  7 | import Foundation
  8 | import Darwin
    |        `- error: no such module 'Darwin'
  9 | 
 10 | /// Context + agent continuity control plane (stdio MCP / same serve binary).
[66/88] Compiling ForgeConductorCore ManagerSettingsNormalizer.swift
/mnt/data/forge_conductor_audit_repo/Forge-Conductor-MacOS-main/Sources/ForgeConductorCore/Application/ContextContinuityService.swift:8:8: error: no such module 'Darwin'
  6 | 
  7 | import Foundation
  8 | import Darwin
    |        `- error: no such module 'Darwin'
  9 | 
 10 | /// Context + agent continuity control plane (stdio MCP / same serve binary).
[67/88] Compiling ForgeConductorCore CPUCollector.swift
/mnt/data/forge_conductor_audit_repo/Forge-Conductor-MacOS-main/Sources/ForgeConductorCore/Application/ContextContinuityService.swift:8:8: error: no such module 'Darwin'
  6 | 
  7 | import Foundation
  8 | import Darwin
    |        `- error: no such module 'Darwin'
  9 | 
 10 | /// Context + agent continuity control plane (stdio MCP / same serve binary).
[68/88] Compiling ForgeConductorCore CPUFrequencyEstimator.swift
/mnt/data/forge_conductor_audit_repo/Forge-Conductor-MacOS-main/Sources/ForgeConductorCore/Application/ContextContinuityService.swift:8:8: error: no such module 'Darwin'
  6 | 
  7 | import Foundation
  8 | import Darwin
    |        `- error: no such module 'Darwin'
  9 | 
 10 | /// Context + agent continuity control plane (stdio MCP / same serve binary).
[69/88] Compiling ForgeConductorCore DiskIOCollector.swift
/mnt/data/forge_conductor_audit_repo/Forge-Conductor-MacOS-main/Sources/ForgeConductorCore/Application/ContextContinuityService.swift:8:8: error: no such module 'Darwin'
  6 | 
  7 | import Foundation
  8 | import Darwin
    |        `- error: no such module 'Darwin'
  9 | 
 10 | /// Context + agent continuity control plane (stdio MCP / same serve binary).
[70/88] Compiling ForgeConductorCore DiskVolumeCollector.swift
/mnt/data/forge_conductor_audit_repo/Forge-Conductor-MacOS-main/Sources/ForgeConductorCore/Application/ContextContinuityService.swift:8:8: error: no such module 'Darwin'
  6 | 
  7 | import Foundation
  8 | import Darwin
    |        `- error: no such module 'Darwin'
  9 | 
 10 | /// Context + agent continuity control plane (stdio MCP / same serve binary).
[71/88] Compiling ForgeConductorCore GPUCollector.swift
/mnt/data/forge_conductor_audit_repo/Forge-Conductor-MacOS-main/Sources/ForgeConductorCore/Application/ContextContinuityService.swift:8:8: error: no such module 'Darwin'
  6 | 
  7 | import Foundation
  8 | import Darwin
    |        `- error: no such module 'Darwin'
  9 | 
 10 | /// Context + agent continuity control plane (stdio MCP / same serve binary).
[72/88] Compiling ForgeConductorCore IOKitPropertyWalk.swift
/mnt/data/forge_conductor_audit_repo/Forge-Conductor-MacOS-main/Sources/ForgeConductorCore/Application/ContextContinuityService.swift:8:8: error: no such module 'Darwin'
  6 | 
  7 | import Foundation
  8 | import Darwin
    |        `- error: no such module 'Darwin'
  9 | 
 10 | /// Context + agent continuity control plane (stdio MCP / same serve binary).
error: fatalError
```

## FAIL (127): `xcodebuild -project ForgeConductor.xcodeproj -list`

- Working directory: `/mnt/data/forge_conductor_audit_repo/Forge-Conductor-MacOS-main`
- Exit code: `127`
- Timed out: `False`

```text
[Errno 2] No such file or directory: 'xcodebuild'
```

## FAIL (127): `xcodebuild -workspace ForgeConductor.xcworkspace -list`

- Working directory: `/mnt/data/forge_conductor_audit_repo/Forge-Conductor-MacOS-main`
- Exit code: `127`
- Timed out: `False`

```text
[Errno 2] No such file or directory: 'xcodebuild'
```

## PASS: `swiftc -frontend -parse <each Swift file>`

- Working directory: `/mnt/data/forge_conductor_audit_repo/Forge-Conductor-MacOS-main`
- Exit code: `0`
- Timed out: `False`

```text
Parsed 112 Swift files; failures: 0
```

## Test source reachability

- Test files: 22
- Production type declarations: 234
- Production types referenced by test source: 95
- Production types not referenced by test source: 139

Lexical name reachability is not line or branch coverage; it identifies ownership/telemetry surfaces needing targeted tests.

### Test files

- `Tests/ForgeConductorTests/AppConfigAndDoctorTests.swift`
- `Tests/ForgeConductorTests/ContinuityTests.swift`
- `Tests/ForgeConductorTests/CoreTests.swift`
- `Tests/ForgeConductorTests/DashboardSecurityTests.swift`
- `Tests/ForgeConductorTests/DashboardTests.swift`
- `Tests/ForgeConductorTests/ForgeProcessEntryTests.swift`
- `Tests/ForgeConductorTests/G1G10AcceptanceTests.swift`
- `Tests/ForgeConductorTests/HTTPTestHelpers.swift`
- `Tests/ForgeConductorTests/LMStudioConnectorReliabilityTests.swift`
- `Tests/ForgeConductorTests/LiveCollectorEvidenceTests.swift`
- `Tests/ForgeConductorTests/MCPProtocolAndDiagnosticsTests.swift`
- `Tests/ForgeConductorTests/MachHostMetricsTests.swift`
- `Tests/ForgeConductorTests/ManagerTests.swift`
- `Tests/ForgeConductorTests/MemoryToolTests.swift`
- `Tests/ForgeConductorTests/NativeAppSmokeTests.swift`
- `Tests/ForgeConductorTests/NativeTelemetryTests.swift`
- `Tests/ForgeConductorTests/ProcessRunnerTests.swift`
- `Tests/ForgeConductorTests/ProductPathReliabilityTests.swift`
- `Tests/ForgeConductorTests/RealtimeStreamTests.swift`
- `Tests/ForgeConductorTests/RigParityTests.swift`
- `Tests/ForgeConductorTests/TelemetryContractTests.swift`
- `Tests/ForgeConductorUITests/ForgeConductorUITests.swift`

### Production types not referenced in tests

- `AppDeployController` (class) — `Sources/ForgeConductorApp/AppDeployController.swift:12`
- `AppModel` (class) — `Sources/ForgeConductorApp/AppModel.swift:19`
- `AppTelemetryBinding` (class) — `Sources/ForgeConductorApp/AppTelemetryBinding.swift:14`
- `ForgeConductorMain` (enum) — `Sources/ForgeConductorApp/ForgeConductorApp.swift:15`
- `ForgeConductorGUIApp` (struct) — `Sources/ForgeConductorApp/ForgeConductorApp.swift:25`
- `ForgeApplicationDelegate` (class) — `Sources/ForgeConductorApp/ForgeConductorApp.swift:60`
- `ForgeMainWindowController` (class) — `Sources/ForgeConductorApp/ForgeConductorApp.swift:104`
- `LoadTraceRenderer` (class) — `Sources/ForgeConductorApp/Metal/LoadTraceRenderer.swift:13`
- `MetalGaugePalette` (enum) — `Sources/ForgeConductorApp/Metal/MetalGaugeKit.swift:26`
- `GaugeVertex` (struct) — `Sources/ForgeConductorApp/Metal/MetalGaugeKit.swift:51`
- `MetalGaugePipeline` (enum) — `Sources/ForgeConductorApp/Metal/MetalGaugeKit.swift:57`
- `MetalBarRenderer` (class) — `Sources/ForgeConductorApp/Metal/MetalGaugeKit.swift:87`
- `MetalBarGauge` (struct) — `Sources/ForgeConductorApp/Metal/MetalGaugeKit.swift:146`
- `MetalRingRenderer` (class) — `Sources/ForgeConductorApp/Metal/MetalGaugeKit.swift:185`
- `MetalRingGauge` (struct) — `Sources/ForgeConductorApp/Metal/MetalGaugeKit.swift:276`
- `MetalRingGaugeLabeled` (struct) — `Sources/ForgeConductorApp/Metal/MetalGaugeKit.swift:294`
- `MetalCoreBarsRenderer` (class) — `Sources/ForgeConductorApp/Metal/MetalGaugeKit.swift:312`
- `MetalCoreBarsView` (struct) — `Sources/ForgeConductorApp/Metal/MetalGaugeKit.swift:396`
- `MetalToolLoadTile` (struct) — `Sources/ForgeConductorApp/Metal/MetalGaugeKit.swift:412`
- `MetalStatusPill` (struct) — `Sources/ForgeConductorApp/Metal/MetalGaugeKit.swift:446`
- `MetalLoadChart` (struct) — `Sources/ForgeConductorApp/Metal/MetalLoadChart.swift:11`
- `MultiSeriesLoadRenderer` (class) — `Sources/ForgeConductorApp/Metal/MultiSeriesLoadRenderer.swift:14`
- `MultiSeriesLoadChart` (struct) — `Sources/ForgeConductorApp/Metal/MultiSeriesLoadRenderer.swift:204`
- `AgentsView` (struct) — `Sources/ForgeConductorApp/Views/AgentsView.swift:14`
- `AppSidebarView` (struct) — `Sources/ForgeConductorApp/Views/AppSidebarView.swift:12`
- `ContentView` (struct) — `Sources/ForgeConductorApp/Views/ContentView.swift:13`
- `DiagnosticsView` (struct) — `Sources/ForgeConductorApp/Views/DiagnosticsView.swift:11`
- `LiveFeedView` (struct) — `Sources/ForgeConductorApp/Views/LiveFeedView.swift:13`
- `MCPServersView` (struct) — `Sources/ForgeConductorApp/Views/MCPServersView.swift:14`
- `ManagerSettingsView` (struct) — `Sources/ForgeConductorApp/Views/ManagerSettingsView.swift:11`
- `RigDashboardView` (struct) — `Sources/ForgeConductorApp/Views/Rig/RigDashboardView.swift:12`
- `TelemetryDashboardView` (struct) — `Sources/ForgeConductorApp/Views/TelemetryDashboardView.swift:13`
- `ToolsView` (struct) — `Sources/ForgeConductorApp/Views/ToolsView.swift:13`
- `ForgeConductorMain` (enum) — `Sources/ForgeConductorCLI/ForgeConductorMain.swift:15`
- `AgentCatalog` (class) — `Sources/ForgeConductorCore/Application/AgentCatalog.swift:10`
- `SimpleYAML` (enum) — `Sources/ForgeConductorCore/Application/AgentCatalog.swift:358`
- `AgentSessionService` (class) — `Sources/ForgeConductorCore/Application/AgentSessionService.swift:10`
- `ContextContinuityService` (class) — `Sources/ForgeConductorCore/Application/ContextContinuityService.swift:11`
- `WorkspaceRootProviding` (protocol) — `Sources/ForgeConductorCore/Application/ContinuityAutomation.swift:10`
- `ContinuityObservation` (struct) — `Sources/ForgeConductorCore/Application/ContinuityAutomation.swift:15`
- `ToolAuthorizationDecision` (enum) — `Sources/ForgeConductorCore/Application/ToolAuthorizationService.swift:13`
- `ToolAuthorizing` (protocol) — `Sources/ForgeConductorCore/Application/ToolAuthorizationService.swift:19`
- `ToolAuthorizationService` (class) — `Sources/ForgeConductorCore/Application/ToolAuthorizationService.swift:32`
- `AgentToolPack` (struct) — `Sources/ForgeConductorCore/Application/Tools/AgentToolPack.swift:10`
- `ContinuityToolPack` (struct) — `Sources/ForgeConductorCore/Application/Tools/ContinuityToolPack.swift:9`
- `DocsToolPack` (struct) — `Sources/ForgeConductorCore/Application/Tools/DocsToolPack.swift:10`
- `FilesystemToolPack` (struct) — `Sources/ForgeConductorCore/Application/Tools/FilesystemToolPack.swift:10`
- `GitToolPack` (struct) — `Sources/ForgeConductorCore/Application/Tools/GitToolPack.swift:10`
- `MemoryToolPack` (struct) — `Sources/ForgeConductorCore/Application/Tools/MemoryToolPack.swift:10`
- `SearchToolPack` (struct) — `Sources/ForgeConductorCore/Application/Tools/SearchToolPack.swift:10`
- `ShellToolPack` (struct) — `Sources/ForgeConductorCore/Application/Tools/ShellToolPack.swift:10`
- `ToolArgHelpers` (enum) — `Sources/ForgeConductorCore/Application/Tools/ToolArgHelpers.swift:10`
- `DashboardHTTPRequestParseResult` (enum) — `Sources/ForgeConductorCore/Dashboard/DashboardHTTPRequest.swift:33`
- `DashboardBindResult` (class) — `Sources/ForgeConductorCore/Dashboard/DashboardServer.swift:13`
- `DashboardError` (enum) — `Sources/ForgeConductorCore/Dashboard/DashboardServer.swift:286`
- `HTTPResponder` (class) — `Sources/ForgeConductorCore/Dashboard/HTTPResponder.swift:12`
- `SSEStreamSession` (class) — `Sources/ForgeConductorCore/Dashboard/HTTPResponder.swift:148`
- `ManagerRoutes` (class) — `Sources/ForgeConductorCore/Dashboard/ManagerRoutes.swift:11`
- `OperationalRoutes` (class) — `Sources/ForgeConductorCore/Dashboard/OperationalRoutes.swift:12`
- `DoctorCheck` (struct) — `Sources/ForgeConductorCore/Domain/DoctorModels.swift:10`
- `DoctorReport` (struct) — `Sources/ForgeConductorCore/Domain/DoctorModels.swift:29`
- `AppStatusSnapshot` (struct) — `Sources/ForgeConductorCore/Domain/DoctorModels.swift:72`
- `ToolPackSummary` (struct) — `Sources/ForgeConductorCore/Domain/ForgeSnapshot.swift:116`
- `AgentSessionSummary` (struct) — `Sources/ForgeConductorCore/Domain/ForgeSnapshot.swift:126`
- `AgentsSummary` (struct) — `Sources/ForgeConductorCore/Domain/ForgeSnapshot.swift:158`
- `LiveFeedEvent` (struct) — `Sources/ForgeConductorCore/Domain/ForgeSnapshot.swift:174`
- `FeedStats` (struct) — `Sources/ForgeConductorCore/Domain/ForgeSnapshot.swift:194`
- `OrchestrationStatus` (struct) — `Sources/ForgeConductorCore/Domain/ForgeSnapshot.swift:212`
- `UsageWindow` (struct) — `Sources/ForgeConductorCore/Domain/ForgeSnapshot.swift:323`
- `MCPLoadWindows` (struct) — `Sources/ForgeConductorCore/Domain/ForgeSnapshot.swift:404`
- `ForgeFilesPresence` (struct) — `Sources/ForgeConductorCore/Domain/ForgeSnapshot.swift:420`
- `AuditEventSummary` (struct) — `Sources/ForgeConductorCore/Domain/ForgeSnapshot.swift:440`
- `AgentContinuitySnapshot` (struct) — `Sources/ForgeConductorCore/Domain/HandoffPacket.swift:18`
- `ManagerStatus` (struct) — `Sources/ForgeConductorCore/Domain/ManagerModels.swift:10`
- `ManagerSettings` (struct) — `Sources/ForgeConductorCore/Domain/ManagerModels.swift:138`
- `ManagerModelError` (enum) — `Sources/ForgeConductorCore/Domain/ManagerModels.swift:216`
- `ManagerJSONValue` (enum) — `Sources/ForgeConductorCore/Domain/ManagerModels.swift:228`
- `TelemetryHealthReport` (struct) — `Sources/ForgeConductorCore/Domain/ManagerModels.swift:301`
- `MemoryNote` (struct) — `Sources/ForgeConductorCore/Domain/Models.swift:54`
- `SessionStatus` (enum) — `Sources/ForgeConductorCore/Domain/Models.swift:94`
- `AgentSession` (struct) — `Sources/ForgeConductorCore/Domain/Models.swift:105`
- `AgentSpec` (struct) — `Sources/ForgeConductorCore/Domain/Models.swift:135`
- `ActiveBinding` (struct) — `Sources/ForgeConductorCore/Domain/Models.swift:205`
- `DiagnosticSeverity` (enum) — `Sources/ForgeConductorCore/Domain/Models.swift:353`
- `DiagnosticRecord` (struct) — `Sources/ForgeConductorCore/Domain/Models.swift:357`
- `Clock` (protocol) — `Sources/ForgeConductorCore/Domain/Models.swift:415`
- `SystemClock` (struct) — `Sources/ForgeConductorCore/Domain/Models.swift:419`
- `ConfigurationProviding` (protocol) — `Sources/ForgeConductorCore/Domain/Protocols.swift:19`
- `PresenceStore` (protocol) — `Sources/ForgeConductorCore/Domain/Protocols.swift:31`
- `RAMMetricsCollecting` (protocol) — `Sources/ForgeConductorCore/Domain/Protocols.swift:41`
- `DiskVolumeCollecting` (protocol) — `Sources/ForgeConductorCore/Domain/Protocols.swift:46`
- `SessionStore` (protocol) — `Sources/ForgeConductorCore/Domain/Protocols.swift:51`
- `AuditReading` (protocol) — `Sources/ForgeConductorCore/Domain/Protocols.swift:56`
- `AgentCatalogProviding` (protocol) — `Sources/ForgeConductorCore/Domain/Protocols.swift:63`
- `ToolExecuting` (protocol) — `Sources/ForgeConductorCore/Domain/Protocols.swift:70`
- `CPUMetricsCollecting` (protocol) — `Sources/ForgeConductorCore/Domain/Protocols.swift:78`
- `GPUMetricsCollecting` (protocol) — `Sources/ForgeConductorCore/Domain/Protocols.swift:83`
- `DiskIOMetricsCollecting` (protocol) — `Sources/ForgeConductorCore/Domain/Protocols.swift:88`
- `ProcessMetricsCollecting` (protocol) — `Sources/ForgeConductorCore/Domain/Protocols.swift:93`
- `PowerMetricsCollecting` (protocol) — `Sources/ForgeConductorCore/Domain/Protocols.swift:98`
- `SystemMetricsCollecting` (protocol) — `Sources/ForgeConductorCore/Domain/Protocols.swift:103`
- `ForgeMetricsCollecting` (protocol) — `Sources/ForgeConductorCore/Domain/Protocols.swift:108`
- `TelemetryProviding` (protocol) — `Sources/ForgeConductorCore/Domain/Protocols.swift:113`
- `ManagerControlling` (protocol) — `Sources/ForgeConductorCore/Domain/Protocols.swift:196`
- `SessionManaging` (protocol) — `Sources/ForgeConductorCore/Domain/Protocols.swift:209`
- `AuditService` (class) — `Sources/ForgeConductorCore/Infrastructure/AuditService.swift:10`
- `DiagnosticCategory` (enum) — `Sources/ForgeConductorCore/Infrastructure/DiagnosticLog.swift:273`
- `DiagnosticExportError` (enum) — `Sources/ForgeConductorCore/Infrastructure/DiagnosticLog.swift:286`
- `DiagnosticEnvelope` (struct) — `Sources/ForgeConductorCore/Infrastructure/DiagnosticLog.swift:296`
- `PDFWriter` (enum) — `Sources/ForgeConductorCore/Infrastructure/PDFWriter.swift:10`
- `ProcessRunnerError` (enum) — `Sources/ForgeConductorCore/Infrastructure/ProcessRunner.swift:24`
- `ResourceBundle` (enum) — `Sources/ForgeConductorCore/Infrastructure/ResourceBundle.swift:10`
- `BundleToken` (class) — `Sources/ForgeConductorCore/Infrastructure/ResourceBundle.swift:21`
- `NativeMCPServeVerifier` (struct) — `Sources/ForgeConductorCore/MCP/MCPServeVerifier.swift:271`
- `ManagerArtifactSignatureState` (enum) — `Sources/ForgeConductorCore/Manager/ManagerInstaller.swift:35`
- `FileManagerArtifactCopier` (struct) — `Sources/ForgeConductorCore/Manager/ManagerInstaller.swift:149`
- `FileManagerArtifactReplacer` (struct) — `Sources/ForgeConductorCore/Manager/ManagerInstaller.swift:155`
- `SelfExecutable` (enum) — `Sources/ForgeConductorCore/Manager/ManagerInstaller.swift:1394`
- `ManagerServiceState` (enum) — `Sources/ForgeConductorCore/Manager/ManagerNode.swift:11`
- `ManagerSettingsNormalizer` (enum) — `Sources/ForgeConductorCore/Manager/ManagerSettingsNormalizer.swift:9`
- `DiskVolumeCollector` (class) — `Sources/ForgeConductorCore/Telemetry/Collectors/DiskVolumeCollector.swift:10`
- `IOKitPropertyWalk` (enum) — `Sources/ForgeConductorCore/Telemetry/Collectors/IOKitPropertyWalk.swift:16`
- `OrchestrationDecision` (struct) — `Sources/ForgeConductorCore/Telemetry/ForgeCollector.swift:72`
- `NativeLMStudioHostActivator` (class) — `Sources/ForgeConductorCore/Telemetry/LMStudioDeployService.swift:285`
- `MCPConfigurationError` (enum) — `Sources/ForgeConductorCore/Telemetry/LMStudioEnvironment.swift:384`
- `InstallError` (enum) — `Sources/ForgeConductorCore/Telemetry/LMStudioMCPPluginInstaller.swift:472`
- `NativeLMStudioPluginInstaller` (struct) — `Sources/ForgeConductorCore/Telemetry/LMStudioMCPPluginInstaller.swift:488`
- `MCPServerCard` (struct) — `Sources/ForgeConductorCore/Telemetry/Models/ForgeUIModels.swift:15`
- `AgentCard` (struct) — `Sources/ForgeConductorCore/Telemetry/Models/ForgeUIModels.swift:190`
- `OrchRoleCard` (struct) — `Sources/ForgeConductorCore/Telemetry/Models/ForgeUIModels.swift:256`
- `PowerMetrics` (struct) — `Sources/ForgeConductorCore/Telemetry/Models/TelemetryModels.swift:130`
- `CPUMetrics` (struct) — `Sources/ForgeConductorCore/Telemetry/Models/TelemetryModels.swift:190`
- `RAMMetrics` (struct) — `Sources/ForgeConductorCore/Telemetry/Models/TelemetryModels.swift:246`
- `DiskVolume` (struct) — `Sources/ForgeConductorCore/Telemetry/Models/TelemetryModels.swift:293`
- `DiskIOMetrics` (struct) — `Sources/ForgeConductorCore/Telemetry/Models/TelemetryModels.swift:333`
- `GPUMetrics` (struct) — `Sources/ForgeConductorCore/Telemetry/Models/TelemetryModels.swift:373`
- `ProcessMetrics` (struct) — `Sources/ForgeConductorCore/Telemetry/Models/TelemetryModels.swift:432`
- `TelemetrySnapshot` (struct) — `Sources/ForgeConductorCore/Telemetry/Models/TelemetryModels.swift:521`
- `TelemetryError` (enum) — `Sources/ForgeConductorCore/Telemetry/TelemetryService.swift:289`
