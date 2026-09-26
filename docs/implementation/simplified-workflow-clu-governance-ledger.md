# Simplified workflow and CLU governance implementation ledger

Date: 2026-09-26

## Phase 0 baseline

- Repository: `https://github.com/flynn33/forge-conductor-windows-edition`
- Branch: `feature/simplified-workflow-clu-governance`
- Base and upstream: `main` / `origin/main`
- Base commit: `69dbefca1c9b955a834d5ef26e3ad08df54b6740`
- Product version: `1.2.1`
- Open pull requests at baseline: none
- Central persistence: released schema 9 with immutable C001-C009 migration history
- Project persistence: immutable P001-P003 migration history
- Preserved unrelated work: `.forge-qwen/state/event-ledger.jsonl`, `.forge-qwen/state/microtasks.json`, `.forge-qwen/state/evidence/P01-V/`, `.forge-qwen/state/scan_microtasks.py`, and `.superdesign/`

The reviewed baseline and the fetched remote were identical. The existing product still exposed guided setup, manual policy review, a single bounded text instruction manifest, and continuity operations under CLU names.

## Baseline verification

The following Release x64 targets built successfully:

- `ForgeConductor.Manager.RequestDispatcherTests`
- `ForgeConductor.Manager.ProtocolTests`
- `ForgeConductor.App.ProjectPolicyServiceTests`
- `ForgeConductor.Mcp.ToolPackAdapterTests`
- `ForgeConductor.Continuity.AutomationTests`

CTest ran the same five focused tests with five passes and no failures.

## Source anchors

- Workspace/navigation: `src/Hosts/App/MainWindow.xaml`, `MainWindow.xaml.cpp`, and `MainWindow.xaml.h`
- Manager protocol/package intake: `include/ForgeConductor/Manager/ManagerProtocolCodec.h`, `src/Manager/ManagerProtocolCodec.cpp`, and `src/Manager/ManagerRequestDispatcher.cpp`
- Policy workflow: `IProjectPolicyService`, `IProjectPolicyGate`, `ProjectPolicyService`, and `WindowsPolicySourceReader`
- Active CLU continuity catalog: `McpToolCatalog`, `McpToolPackAdapter`, `McpServer`, LM Studio configuration/deployment/verifier code, and C008/C009 persistence
- Continuity: continuity automation/coordinator, session host, managed run, and provider binding services
- Project identity: project registry and project-memory models/repositories
- Operational surfaces: dashboard/telemetry, diagnostics, Doctor, audit, and maintenance services

## Phase order

1. Domain boundaries and persistence contracts
2. Universal instruction-package ingestion
3. Persistent ordered package queue
4. CLU governance enforcement
5. Independent automatic continuity
6. Workspace and four-destination navigation
7. Activity, Rig, Settings, Doctor, and export consolidation
8. Migration and removal of retired behavior
9. Full qualification, package creation, documentation, and delivery evidence

Phase adjustments and verification commands are recorded below as implementation proceeds.

## Implemented release

- Version advanced to 1.3.0.
- Normal navigation reduced to Workspace, Rig, Activity, and Settings.
- Universal instruction-package inventory and persistent ordered queue implemented through the typed Manager boundary.
- CLU replaced with nonblocking development-governance tools and policy coverage/findings/correction/export state.
- Automatic continuity carried independently on managed-run start requests and skipped when disabled.
- Legacy active instruction-package state migrates into the queue; existing policy snapshots migrate to the current governance schema.
- In-app help, README, user guide, architecture, status, roadmap, changelog, and release notes updated.

## Qualification

- Complete Release x64 backend build: passed.
- Release x64 WinUI application and sibling-service staging: passed.
- Full CTest matrix: 153/153 passed.
- Native-stack, no-Python, and no-attribution static gates: passed.

Packaging is performed only after the release inputs are committed and rebuilt so the staging manifest can bind the exact commit and tree.
