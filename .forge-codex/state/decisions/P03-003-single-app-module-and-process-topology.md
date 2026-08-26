# P03-003: Single App Module and Process Topology

Status: Accepted

Date: 2026-08-25

## Context

The product requires native GUI, CLI, stdio MCP, manager, session-host, and installer surfaces. Forsetti permits a single active UI/app surface and the task contract selects deployment_pattern single_app_module.

## Decision

ForgeConductorAppModule is the only registered Forge app/UI module. It is identified by com.forsetti.app.forge-conductor-windows and owns all seven product UI surfaces through one WinUI 3 shell.

The executable topology is:

1. ForgeConductor.App.exe owns WinUI windows, presentation state, and Forsetti host composition.
2. forge-conductor.exe owns CLI execution and one stdio MCP connection when invoked in serve mode.
3. ForgeConductor.Manager.exe is the single per-user manager and owns dashboard listening, telemetry sampling, durable coordination, deployment reconciliation, and startup lifecycle.
4. ForgeConductor.SessionHost.exe and ForgeNativeSessionHostPlugin.dll own supported provider adapters and logical-session execution.
5. ForgeConductor.Setup.exe owns install, repair, update, uninstall, and purge transactions.

CLI, manager, MCP, session-host, setup, and plugin surfaces are process/deployment components, not Forsetti modules. They reuse Domain, Contracts, Application, and adapter libraries through their own explicit composition roots. They do not register competing app modules or simulate GUI host behavior. Each exclusive resource has one process owner; cross-process collaboration uses authenticated protocols.

## Consequences

UI activation is deterministic, dashboard/listener duplication is prevented, stdio purity is preserved, and process failures remain isolated. Process boundaries do not become module boundaries.

## Framework boundary

Changes to ForsettiCore, ForsettiPlatform, or ForsettiHostTemplate internals are prohibited. The GUI composes the sealed host through public APIs; the other processes do not patch or subclass it.

## Evidence

- .forge-codex/instructions/architecture/TARGET_ARCHITECTURE.md - SHA-256 6271806ac05ec06b638b04d9cf1a9a0c6123f7a32bc4ecbc74227cc3c603a88d
- .forge-codex/instructions/architecture/PROCESS_MODEL_AND_IPC.md - SHA-256 3da5cb2b39bdd3e55494e172177a052e8999deea9d4e92b4a95e36a7fef31e3d
- .forge-codex/instructions/governance/PORT_TASK_CONTRACT.json - SHA-256 cd2714169189e274eb51c3b0bb666b2b59a68042f4c01b0a60f7506ab6ad0547
- .forge-codex/state/baseline/p02-mcp-semantic-inventory.json - SHA-256 a16739eeb36da11c805a62522a769cddf843ffc28df3f87a16d6e90aa7602ef3
