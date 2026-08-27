# P15-001: Evidence-Based LM Studio Deployment and Maintenance Authority

Status: Accepted

Date: 2026-08-27

## Context

P15 must preserve the macOS 0.9.0 LM Studio behavior on Windows without
guessing installation paths or weakening the existing authority boundary. The
Windows host on the owner's machine is a per-user LM Studio installation with
a generic `mcp.json`, two foreign plugin directories, a host synchronization
state file, and no observed command that reloads MCP registration. The existing
LM Studio contracts require project-bound read, write, and execute authority,
while LM Studio's configuration and plugin roots intentionally reside outside
ordinary user project roots.

The owner retained native product UI automation for alpha. Governing security
and MCP architecture separately prohibit automating another application's GUI
to simulate a supported session API. Those requirements address different
surfaces and do not conflict: Forge Conductor UI automation remains required in
P20, while P15 interacts with LM Studio only through files, processes, and
supported host activation mechanisms.

## Decision

### Discovery and retained evidence

- Discovery evaluates an explicit configuration path first, followed by
  per-user installed-application registrations and known user locations, then
  permitted running-process image paths.
- Every evaluated candidate is retained with its source, path, validation
  result, selection state, and a bounded diagnostic detail. The selected
  application executable is represented separately from its installation root.
- Candidate configuration is parsed as generic hostile JSON. A malformed root
  or `mcpServers` value is reported and is never silently replaced with a
  default document.
- Production discovery is behind an injected Windows candidate source and the
  existing filesystem/authority contracts so deterministic tests do not depend
  on this machine's registry, profile, or running processes.

### MCP role verification

- The native process supervisor accepts an optional bounded UTF-8 stdin payload
  and closes the child input pipe deterministically. The existing deadline,
  cancellation, output caps, Job Object, and shutdown ownership remain intact.
- The production serve verifier launches the exact selected Forge executable
  with `serve`, sets the primary or fallback role and optional shared deployment
  revision, and sends newline-delimited `initialize`, `initialized`, and
  `tools/list` JSON-RPC messages.
- A role is healthy only when the response uses newline JSON-RPC without
  `Content-Length`, negotiates a supported protocol, returns the exact role
  server name, and exposes exactly 53 unique deterministic tools.

### Transaction and preservation

- The current `mcp.json` is parsed by a dedicated generic codec. Foreign server
  entries and unknown root and per-server fields survive deploy, repair, and
  rollback.
- A deployment uses a fresh nonempty shared revision, pre-smokes both roles,
  snapshots the exact original configuration or absence state, stages and
  validates both Forge-owned plugin layouts, commits fallback before primary,
  atomically replaces configuration, validates committed state, and post-smokes
  both roles.
- Any failure rolls changed role paths back in reverse order and restores the
  exact original configuration bytes, or removes the new configuration when no
  prior file existed. `preserveForeignEntries=false` is rejected because
  preservation is a product invariant rather than an optional policy.
- Status treats missing roles, wrong binary, wrong role, empty or mismatched
  revisions, malformed configuration, and stale Forge-owned entries as typed
  drift. Healthy repair is idempotent; drift repair is a new transaction with a
  new revision.

### Host activation and synchronization truth

- When LM Studio is stopped, activation may use its discovered executable or
  registered protocol through `ShellExecuteExW`. P15 does not click, type into,
  inspect, or otherwise automate the LM Studio GUI.
- The activator waits for bounded host-owned evidence that both Forge entries
  synchronized the exact deployment revision. A matching configuration file
  alone is not reported as stronger proof that both lazy per-chat MCP role
  processes are connected.
- When LM Studio is already running and no supported hot-reload mechanism is
  available, P15 does not kill or restart it. It reports unsynchronized state
  unless host-owned evidence already matches. `restarted` remains false until a
  supported restart mechanism is established.

### Narrow maintenance authority

- LM Studio maintenance uses a dedicated application-owned maintenance project
  identity rather than widening any user workspace project. Its trusted roots
  are limited to the discovered LM Studio configuration/plugin root, the exact
  selected Forge executable parent, and the exact LM Studio executable parent
  needed by the requested operation.
- Read, write, and execute grants are issued separately. Deployment and
  activation verify the authorization's project, caller, correlation,
  authority identifier/generation, effect, and exact tool name against the
  current operation context.
- P15 implements and qualifies the backend boundary. P21 owns persistence and
  user-facing composition of the maintenance project identity for CLI commands;
  P20 owns the equivalent UI command wiring.

## Consequences

Discovery decisions are explainable and fixture-testable, deployment can fail
without corrupting an operator's LM Studio state, and the primary/fallback
registrations share one auditable revision while retaining independent role
identity. Host synchronization claims remain weaker than live role readiness
unless host evidence proves the latter.

The extra maintenance identity is product state with an explicit owner; it is
not a hidden service locator or a process-wide mutable singleton. Application
services continue to receive abstract capabilities and adapters through their
composition roots.

## Alternatives rejected

- Hard-coding a single `%USERPROFILE%` path would discard installed-app and
  running-process evidence and fail alternate per-user layouts.
- Reusing `WindowsConfigurationStore` would apply Forge's schema to an external
  generic JSON document and could discard fields owned by LM Studio or another
  plugin.
- Deploying only one role, or committing primary first, would weaken the
  required fallback continuity behavior.
- Treating a rewritten `mcp.json` as proof of host synchronization would confuse
  local mutation with host consumption.
- Fabricating a projectless capability or widening the active workspace
  authority would violate the established authority model.
- Automating LM Studio's GUI would violate the task contract and still would not
  provide a supported synchronization API.

## Scope and limitations

The owner-approved alpha qualification is limited to this Windows 11 x64
machine and one authoritative G15 rebuild/test invocation. Security-only
hardening, clean-environment qualification, a broad installer matrix, and
bespoke UI polish remain deferred under OWNER-002. Strict JSON handling,
bounded processes, scoped authority, cancellation, preservation, and exact
rollback remain functional correctness and are not deferred.

P15 does not close the later UI-006 or CLI-002 parity rows. It supplies their
backend. Real-host synchronization must still be captured before G15 can pass
because LM Studio is installed on this machine.

## Evidence basis

- `.forge-codex/state/decisions/OWNER-002-alpha-release-qualification-scope.md`
- `.forge-codex/state/commands/20260827T123222434Z-6a51667c.json`
- `.forge-codex/state/evidence/P15/windows-lm-studio-host-baseline.json`
- `.forge-codex/instructions/architecture/MCP_AND_LM_STUDIO.md`
- `.forge-codex/instructions/architecture/OOP_AND_OWNERSHIP.md`
- `.forge-codex/instructions/plans/phases.json`
- `.forge-codex/instructions/plans/gates.json`
- `.forge-codex/instructions/plans/test-matrix.json`
- `include/ForgeConductor/Contracts/ILMStudioEnvironment.h`
- `include/ForgeConductor/Contracts/ILMStudioDeploymentService.h`
- `include/ForgeConductor/Domain/EnvironmentModels.h`
- `.forge-inputs/Forge-Conductor-MacOS-main/Sources/ForgeConductorCore/Telemetry/LMStudioEnvironment.swift`
- `.forge-inputs/Forge-Conductor-MacOS-main/Sources/ForgeConductorCore/Install/LMStudioMCPPluginInstaller.swift`
- `.forge-inputs/Forge-Conductor-MacOS-main/Sources/ForgeConductorCore/Install/LMStudioDeployService.swift`
- `.forge-inputs/Forge-Conductor-MacOS-main/Sources/ForgeConductorCore/Install/MCPServeVerifier.swift`
