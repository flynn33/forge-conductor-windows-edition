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

## Changes deliberately avoided
No new general plugin bus, migration to a web GUI, remote orchestration server, replacement database,
new governance engine, multi-agent validator pipeline, or generalized cross-platform source rewrite.
The Mac Swift source is behavioral evidence only. No Swift binaries belong in the Windows build or installer.

<!-- alpha-phase-review:start -->
Phase review: R1 — 2026-09-12. Implementation and verification status: [Product status](STATUS.md).
Delivery/merge status is recorded by the linked phase pull request.
<!-- alpha-phase-review:end -->
