# Windows Alpha audit and macOS parity assessment
**Date:** September 10, 2026. Windows `3e67a03b6c64d9a105b677dfe41e413bddd7762c`; macOS `6c9c91da74d44a57812887dc71f5c1de7abeddf7`.
Evidence IDs resolve in [SOURCES.md](SOURCES.md).

## Verdict
The Windows repository is a substantial native backend port, **not a demonstrated working Alpha**.
The inspected tree has native CLI, manager, session-host, persistence, MCP and tool code worth preserving.
Its main deficiencies are product integration, a missing native GUI target, an unimplemented installer,
nonreproducible dependency setup, incorrect continuity trigger behavior, and disconnected provider execution.
Adding another general audit/gate framework would address none of these. [W03, W05, W09, W12, W17–W19]

## Findings that change the implementation order
| ID | Priority | Source-backed finding | Required action |
|---|---|---|---|
| A01 | Alpha blocker | CMake requires a Forsetti tree under `.forge-inputs`; it is absent from the tracked root and the external project has no download step. | Restore a verified dependency and check in a reproducible bootstrap/lock. Do not weaken the framework into a stub. [W05, W06] |
| A02 | Alpha blocker | The inspected host tree has CLI, Manager and SessionHost, and the CMake file has no WinUI target. Packaging names a GUI executable not established by that build. | Add the native WinUI C++/WinRT app, reuse existing services, and build it early. [W03, W10] |
| A03 | Alpha blocker | `scripts/package.ps1` always throws even after a certificate is supplied. The manifest has replacement identity/publisher values. | Implement a signed MSIX with actual executable/resources/dependencies and an installation distribution. [W09, W10] |
| A04 | Alpha blocker | `ContinuityAutomation::triggerDecision` can roll over on progress count or elapsed time while context action is Normal. | Remove these independent rollover policies and migrate their consumers. Use context threshold only. [W12] |
| A05 | Alpha blocker | Production SessionHost composition uses a local logical transport that makes synthetic native IDs and locally schedules/acknowledges handoffs. | Wire actual provider inference and a manager-owned continuation executor; a local enqueue is not autonomous model continuation. [W17, W18, W19] |
| A06 | Alpha blocker | The alternative WinHTTP transport expects `/v1/forge/sessions` and custom bootstrap/query endpoints. | Implement the documented LM Studio `/v1/responses` contract, matching the Mac behavioral seam instead of assuming these custom routes exist. [W15, W16, M05, E01, E02] |
| A07 | Scope conflict | Root agent instructions default shell off, require full hard-gate completion, and point to old governance/plan paths. | Replace active instructions with the owner's Alpha scope; preserve explicit opt-out, enable shell on clean profiles, retain basic data correctness. [W02, W14, M01] |
| A08 | Organization gap | README is 598 bytes of bootstrap instructions. Root contains generated CMakeCache/CMakeFiles and temporary check files, but no public docs/roadmap/.github directory in the inspected root tree. | Establish one public roadmap/status/docs set, remove generated files from Git tracking without deleting source or local dependencies, and seed GitHub milestones/issues. [W01, W03; root-tree inspection] |
| A09 | Delivery risk | CMake version is 0.9.0; package template is 0.1.0.0; CLI alias is attached to the GUI application entry. | Centralize versioning and verify installed CLI/serve routing independently from GUI startup. [W03, W10, W19] |
| A10 | Parity gap | The old Windows contract names seven UI surfaces, while Mac exposes projects, autonomy, continuity, runtimes, provider and evidence as well. | Implement all required native pages and settings actions; do not count labels or navigation placeholders as parity. [W02, M01, M03, M04] |

The root `src/Bootstrap/main.cpp` has a trivial self-test, but the actual CMake source also defines a separate
production CLI. This audit does **not** mistake that bootstrap for all existing Windows functionality. [W03, W11]

## What should be kept
Keep the OO C++20 interfaces and composition roots, native process and filesystem services, SQLite repositories,
MCP catalog/router/stdio transport, project memory services, agent assets, manager protocol, continuity coordinator,
and existing useful native tests. These are integration assets, not proof of an installed-app pass. [W03, W13, W19]
Do not force a new language, web-based GUI, external JavaScript runtime, or a second orchestration architecture.
Do not redesign sealed Forsetti internals merely to remove old workflow gates.

## Mac is a behavioral reference, not a completed-release certificate
The Mac source contains native provider setup and project actions, and its real adapter uses `v1/responses`.
Its README separately leaves current real-provider threshold rollover and release qualification open.
Windows should reuse those feature contracts and validate its own actual execution, not inherit a claim
that Mac has already qualified every scenario. Windows also must not inherit Mac-only notarization,
privileged-filesystem race qualification, or a lengthy signed-hardware matrix for this internal Alpha. [M01–M05]

The owner's no-quota requirement intentionally differs from older behavior in either edition.
Mac production `fs_move` and recursive protected deletion are documented as unavailable; do not describe
those as working Mac features that block Windows Alpha. Mark platform-specific or advanced work explicitly deferred. [M01]

## Documentation versus historical implementation evidence
The repository is not literally devoid of documentation: `.forge-codex/state/baseline` contains substantial
feature, tool, UI and persistence inventories, and old package instructions are tracked. The problem is that
those records are buried, historical, and not a concise current user/developer roadmap. Reuse their tool names
and data contracts instead of rebuilding the inventory from scratch, then reconcile only actual changes.
Do not expose old ledger percentages as product completion percentages.

## Audit coverage and limitations
This was remote source inspection of both pinned default branches, key build/config/packaging and composition files,
continuity implementations, Mac operator view models and native provider source. It was **not** a line-by-line
review of every file, a Windows build, an installer execution, or a live LM Studio run. Compilation, GUI usability,
all tool semantics and current hardware behavior remain unverified here. No trustworthy percentage-complete or
hour estimate follows from source-file volume or old phase ledgers.

GitHub issue search returned no open matches in the query performed. The connector did not support the milestones
endpoint; existing milestone/board state was not verified. No remote issues, labels, milestones, branches, settings,
or code were modified during this audit. The package's synchronization helper reads/reconciles that state first.

## Smallest credible route to Alpha
Restore reproducible builds and authority, get an actual native window plus engineering MSIX early, connect projects/MCP/tools,
fix provider execution and context-only continuity, finish native actions/settings, then perform one installed Alpha acceptance pass.
The supplied roadmap turns that sequence into seven milestones, with later parity/hardening work explicitly outside Alpha.

## Dependency recovery refinement
A public fixed upstream candidate was identified at `63b9db87c575b2c72bfb6b3c988fcd7abd7fabe5`. Its retrieved CMakeLists.txt
has exactly the required SHA-256 `d586bb4b7c9f6d00a987357c0df916718485aba34c13c86a3b5050c32cdbaa3d`.
The helper can download that fixed candidate and must verify the **entire** extracted tree against the original lock.
The complete archive/tree was not downloaded or verified in this environment, so equivalence is not asserted here.
This gives P0 a concrete recovery route rather than an unspecified dependency search. [D01, W05, W06]
