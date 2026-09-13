# Alpha architecture — retain the backend, finish the product
## Ownership
`WinUI views -> view models -> typed manager client -> per-user manager -> application services -> domain/contracts`.
Windows infrastructure implements filesystem, process, SQLite, HTTP and IPC contracts. Constructors receive dependencies;
views never spawn processes, read databases or rewrite LM Studio configuration directly.

The manager owns project/run state, the live provider loop, continuity scheduling and long-running processes.
Closing the GUI detaches the client; it must not cancel a run unless the operator explicitly requests that action.
The CLI remains a separate native executable with `serve` as the external stdio MCP entry point.
The session-host adapter is an execution component, not proof that a real provider session exists.

## Build boundaries
Retain the current CMake targets for backend libraries, manager, CLI, session host and native tests.
Add a normal Microsoft WinUI C++/WinRT MSBuild project for XAML compilation and packaging.
Use explicit paths to the CMake output libraries through generated local build properties or a small staging manifest.
Do not hand-port XAML build machinery into thousands of new CMake rules. Match architecture, CRT and configuration.
The canonical build script orchestrates both toolchains and stages the actual runtime payload.

Use the existing Forsetti app-module composition where appropriate. A GUI entry point does not justify changing
sealed framework source or duplicating manager-owned services inside a singleton UI model.

## Product truth
Maintain a compact manager snapshot containing connection state, project identity, active run/session,
context usage/source/threshold, handoff status, tool availability and recent events.
It must distinguish disconnected, unavailable, pending, failed, paused, and completed. Do not display green
connected/autonomous status from the existence of a configuration file or a locally generated session ID.

Default local endpoint is LM Studio on 127.0.0.1:1234. The selected provider endpoint and model belong to
persistent configuration, not constants copied into separate pages. Manager IPC stays per-user/local.
Add support for an explicitly configured LAN model endpoint only through the same provider configuration/transport;
network placement does not change project identity, cancellation or data-safety behavior.

R2 implements that snapshot as `ManagerTelemetrySnapshot`. The Manager joins its existing telemetry service with
status, settings, selected-run context, continuity identity, runtime diagnostics, project/tool catalogs, audit events,
and store-read health before encoding one authenticated pipe response. The WinUI process renders the typed values and
never opens telemetry collectors or recomputes retained-context headroom. One window-owned timer requests fresh
snapshots; closing the window stops the timer and cancels in-flight UI work without stopping the Manager.

R3 and R4 extend the same pipe boundary with typed project, LM Studio, tool, operational, settings, and maintenance requests. Settings never opens configuration JSON or project databases. Reset dispatch validates an exact project/profile confirmation, invokes the existing transactional memory and continuity owners, and closes the affected repository generation so a stale owner cannot continue writing after a committed reset.

## Changes deliberately avoided
No new general plugin bus, migration to a web GUI, remote orchestration server, replacement database,
new governance engine, multi-agent validator pipeline, or generalized cross-platform source rewrite.
The Mac Swift source is behavioral evidence only. No Swift binaries belong in the Windows build or installer.

## Product and package identity

`ForgeConductor::Domain::ProductIdentity` is the single native product-version source consumed by the Manager, CLI/MCP host, LM Studio transports, and diagnostics. CMake and packaging validate the same `0.9.4` value; the stable MSIX identity is `ForgeConductor.Windows.Alpha` with numeric version `0.9.4.0`. Release staging records the commit, tree, configuration, architecture, and hashes of all four product executables before packaging. The Manager uses a dedicated process exit code for an unsupported newer central store so the GUI can explain the non-destructive failure and the explicit disposable `--alpha-root` alternative. Production view state retains the stable legacy registry names; `--alpha-root` derives deterministic per-profile names and validates a saved project ID against the authoritative Manager snapshot before use.

<!-- alpha-phase-review:start -->
Phase review: R6 acceptance closeout — 2026-09-13. Implementation and verification status: [Product status](STATUS.md).
Delivery/merge status is recorded by the linked phase pull request.
<!-- alpha-phase-review:end -->
