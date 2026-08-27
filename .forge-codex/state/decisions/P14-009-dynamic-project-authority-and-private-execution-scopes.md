# P14-009: Dynamic Project Authority and Private Execution Scopes

Status: Accepted

Date: 2026-08-27

## Context

The initial stdio composition bound one workspace authority and one native
execution authority to the project present at process startup. That was
sufficient for the first single-project MCP snapshot, but it could not safely
serve a project registered after launch or preserve multiple aliases for a
project. Git and PowerShell also retained the startup execution capability,
which made their lifetime and project scope differ from the authority carried
by each MCP request.

P14 must demonstrate both primary and fallback roles with the exact 53-tool
surface while a live process discovers, adopts, and operates on a second
canonical project. P16 will later replace alpha direct persistence ownership
with manager IPC, but P16 is not a dependency of the G14 acceptance criterion.

## Decision

- `WindowsProjectWorkspaceAuthority` owns the stdio process's project-to-
  authority-id bindings. It rereads the authoritative registry descriptor on
  every issuance, narrowing, and path authorization, so alias attachment,
  detachment, project removal, and projects registered after launch are
  visible without restarting the server.
- Authority identifiers remain stable for the process lifetime and are
  bounded to 1,024 projects. Concurrent first issuance uses double-checked
  publication so every caller observes one published identifier. Foreign
  identifiers, callers, stale generations, overlapping/case-duplicate roots,
  removed projects, reparse roots, cancellation, and expired deadlines fail
  through typed results.
- Relative MCP paths anchor to the request authority's first canonical alias.
  Absolute paths omit that base so any alias in the same registered project
  can be selected and validated by the authority implementation.
- Every Git operation now receives the caller's workspace authority. Git and
  PowerShell construct a private, invocation-local execute-only authority from
  that caller's canonical project roots plus the configured executable's
  exact parent directory. Git remains available when optional shell execution
  is disabled; PowerShell still requires the explicit shell opt-in. Neither
  service owns product state or a startup-project capability.
- Directory ancestor anchors continue to omit `FILE_SHARE_DELETE`, while
  non-final ancestors admit sibling write-capable handles. Sharing and lock
  violations receive one bounded 500 ms retry window; the exact target parent
  retains the caller's stricter concurrent-write policy.
- Project-memory calls without an explicit deadline inherit the earlier of
  the parent deadline and the service's 60-second maximum. Legacy continuity
  packets canonicalize packet and agent timestamps to the persisted
  millisecond wire precision before compare-exchange. `fs_write` first uses
  overwrite authority and retries a genuinely missing target with create
  authority, preserving the native atomic store's create/overwrite split.

## Consequences

A running stdio server can resolve every alias of every registered project and
can adopt a later project from continuity without broadening one startup
capability. Native process services remain constructor-injected and bounded,
but their effective filesystem and execution scope is derived for each call.

The real-process qualification deliberately registers project B through the
same atomic registry format used by the repository. This is a simulation of
future manager registration, not a claim that P16 manager IPC exists. Direct
stdio persistence remains the owner-approved alpha architecture recorded by
P14-007.

## Alternatives rejected

- Retaining a process-start snapshot would require restart to observe project
  B and would fail autonomous context recovery.
- Sharing one process-wide execution capability would obscure the request's
  project identity and keep stale roots alive.
- Granting create authority for every file write would make overwrite and
  create indistinguishable at the atomic-storage boundary.
- Weakening continuity CAS equality would hide persistence corruption; input
  timestamps are instead normalized to the established wire precision.
- Sequencing primary and fallback first startup would avoid the observed
  contention but would not qualify simultaneous convergence.

## Scope and limitations

This decision qualifies P14/G14 on the owner's current Windows 11 x64 machine
in Debug configuration. P16 still owns versioned manager IPC and final
cross-process persistence ownership. Security hardening, clean-environment
qualification, a broad installer matrix, and bespoke UI polish remain deferred
until after alpha under OWNER-002; planned UI automation remains in scope for
its later gate. This is not a whole-application completion claim.

## Evidence basis

- `.forge-codex/state/decisions/OWNER-002-alpha-release-qualification-scope.md`
- `.forge-codex/state/decisions/P14-007-alpha-stdio-persistence-ownership.md`
- `.forge-codex/state/decisions/P14-008-source-compatible-mcp-semantics-and-windows-extensions.md`
- `.forge-codex/state/commands/20260827T112918356Z-a8f5616a.json`
- `.forge-codex/state/evidence/P14/dynamic-multi-project-workspace-authority-checkpoint.json`
- `include/ForgeConductor/Infrastructure/Windows/WindowsProjectWorkspaceAuthority.h`
- `src/Infrastructure/Windows/WindowsProjectWorkspaceAuthority.cpp`
- `src/Hosts/Cli/McpServeCompositionRoot.cpp`
- `src/NativeTools/Windows/NativeToolValidation.h`
- `src/NativeTools/Windows/WindowsGitService.cpp`
- `src/NativeTools/Windows/WindowsShellService.cpp`
- `src/Mcp/McpToolPackAdapter.cpp`
- `tests/Infrastructure/WindowsProjectWorkspaceAuthorityTests.cpp`
- `tests/Mcp/McpServeProcessSnapshotTests.cpp`
- `tests/NativeTools/WindowsGitShellTests.cpp`
