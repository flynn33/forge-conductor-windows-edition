# Alpha scope and definition of done
## Required product
One supported Alpha platform: Windows 11 x64. A native C++20 WinUI 3 GUI must install, start,
connect to its manager, remain responsive, and perform useful work without a developer shell.
The installer distribution includes a signed MSIX, dependencies when needed, public certificate/trust instructions,
installation helper, quick-start and release notes. An unsigned ZIP of executables is not the installer deliverable.

Required behavior: select/create projects and retain their identities; authorize project folders;
deploy primary/fallback Forge MCP registrations without losing unrelated entries; run filesystem/search/Git/shell
and project-memory operations; inspect agents/tools/feed; save provider settings and discover/test a real LM Studio model;
start/pause/resume/stop a managed run; trigger automatic continuity at the effective model context threshold;
resume real model work after exact handoff acknowledgment and predecessor fencing; keep work alive after GUI close;
provide project-scoped memory/continuity reset and an explicitly confirmed all-stores maintenance action.

Required native surfaces: Rig, LM Studio MCP, Agents, Tools, Feed, Projects, Autonomy, Continuity,
Runtimes, Provider, Events & Evidence, Diagnostics, Manager, and Settings. Related pages may share a layout
or tab, but all named capabilities must be reachable, useful and connected to real data/actions.
No decorative dashboard or disabled placeholder button may stand in for required functionality.

No per-run tool-call allowance or count/time-triggered session rollover is part of the product.
Output token reserves and the model context threshold remain, because handoff must happen before overflow.

## Practical acceptance
Each item must have one short recorded result, exact tested commit/configuration and any limitation:
1. Build the real x64 Release products and create the installer distribution.
2. Install on a Windows environment without this source checkout or Visual Studio. Launch through Start.
3. Register a disposable project, deploy MCP, execute actual tools, and prove memory persists across restart.
4. Complete a real managed-provider run through a context-forced successor transition and automatic further work.
5. Exercise every native page once and the essential settings/reset actions on disposable data.
6. Close/reopen GUI with manager-owned work; check installed CLI `serve` remains protocol-only on stdout.
7. Reinstall/upgrade once and uninstall once without silently erasing user project data or foreign MCP configuration.

These are product acceptance checks, not a request for dozens of new test frameworks.
A failed required capability remains a blocker; lack of live hardware/provider access is recorded, never called a pass.
An early installable engineering checkpoint is encouraged, but it is not labeled the final working Alpha.

## Not required before this Alpha
New security-hardening projects, Mac filesystem E2 qualification, penetration testing, exhaustive crash matrices,
benchmark/stress/leak campaigns, Store publication, public release signing infrastructure, auto-update service,
ARM64, custom GPU art, full browser dashboard parity, and a new governance/completion-policy engine.
Preserve useful existing implementations without expanding them into Alpha blockers.
Full parity beyond the explicit Alpha subset is a later milestone; do not label deferred rows complete.

<!-- alpha-phase-review:start -->
Phase review: R6 continuation — 2026-09-12. Implementation and verification status: [Product status](STATUS.md).
Delivery/merge status is recorded by the linked phase pull request.
<!-- alpha-phase-review:end -->
