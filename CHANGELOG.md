# Changelog

## Unreleased — Windows Alpha recovery

### Implemented foundation

- Added the native C++20/WinUI 3 desktop host, per-user Manager attachment, typed provider settings, and isolated `--alpha-root` development profile support.
- Added real loopback LM Studio `/v1/models` and `/v1/responses` bootstrap transport, actual response-ID chaining, tool-call correlation, usage accounting, and Manager-owned continuity automation lifetime.
- Removed count- and time-triggered rollover behavior; context consumption is the only automatic continuity trigger.
- Verified disposable project registration, MCP deployment, filesystem/Git/shell/memory operations, project isolation, and preservation of foreign MCP entries through existing native services.
- Added signed x64 engineering MSIX/ZIP packaging with public certificate material and install guidance.

### R0 — Current baseline and accountable delivery

- Adopted replacement plan `windows-alpha-recovery-2026-09-12` without resetting merged foundation work or local Forge Qwen runtime evidence.
- Reconciled merged PR #2 and the real GitHub main, created R0–R7 milestones and phase issues, and migrated the execution ledger to R phase semantics.
- Reconciled all active first-party documentation and classified prior P0–P6 guidance and audit records as historical reference.

### R1 — Manager-owned managed runs and continuity controls

- Added a durable Manager-owned ordinary run service with typed start/status/pause/resume/cancel commands and native GUI controls.
- Completed `/v1/responses` function-call correlation through the authorized native MCP tool router with bounded turns, exact project/run/generation binding, and safe handling of uncertain recovered work.
- Separated lifetime token accounting from retained context, deduplicated provider observations, and returned productive successor response identity to the ordinary run loop after a canonical context-only handoff.
- Fixed isolated first-start Manager initialization by preparing the memory and handoff roots before workspace authority validation.
- Verified the affected Manager, continuity, transport, environment, and native app builds; live LM Studio continuity remains an R6 acceptance requirement.

### R2 — Native operational telemetry and dashboards

- Replaced the production unavailable telemetry adapter with native Windows CPU, RAM, Forge process, and DXGI capability collectors while preserving explicit warmup, stale, unavailable, and unsupported states.
- Added one typed Manager telemetry snapshot across the authenticated named pipe for resources, runtime diagnostics, provider, authoritative run/context/continuity state, projects, tools, events, and store health.
- Added WinUI status cards, CPU/RAM/GPU/context gauges, bounded resource and latency histories, accessible equivalent values, an activity timeline, and telemetry-backed Provider, Continuity, Runtimes, Projects, Tools, Feed, Events, Diagnostics, Manager, and Settings summaries.
- Added two-second coalesced refresh, disconnected/stale rendering, resize redraw, persisted page selection, and window-close cancellation. Real isolated process probes confirmed repeated refresh and Manager survival after GUI close; native keyboard/high-contrast and visual rendering inspection remain blocked by the available control surface.

### R3 — Native operating pages and local workflows

- Added typed Manager project-list, folder-registration, memory-search/read/write, and persistence-status operations over the authenticated named pipe.
- Replaced the Projects placeholder with native registration and stable selection, authorized-folder and storage-health views, full memory records, and persisted exact-ID binding into ordinary runs.
- Added project-scope fencing for Manager memory responses and focused two-project isolation coverage; a fresh isolated native workflow retained memory after restart without leaking it to the second project.
- Incorporated `continuous-delivery-repair-2026-09-12` into the adopted execution, Git, closeout, and handoff guidance so phase checkpoints and unavailable inspections do not stop independent R0–R7 work.
- Added native LM Studio MCP repair/activation, exact tool catalog/invocation, agent/session actions, audit feed, runtime, diagnostics, and Manager pages backed by typed Manager operations.

### R4 — Accessible settings and scoped maintenance

- Replaced the Settings placeholder with labeled native controls for dashboard, Manager lifecycle, LM Studio model discovery, logging, shell policy, session retention, and context rollover thresholds.
- Added paired context sliders and exact token values, pending-edit revert, provider testing, effective readback after save/restart, and direct links to focused Provider and Manager pages.
- Added typed Manager maintenance for exact-project memory, continuity, combined project data, and separately confirmed all-project data. Resets reuse transactional repositories, close old project generations, preserve source folders, and report affected counts.

### Remaining Alpha work

- Complete installer/data behavior, native visual walkthroughs, and real installed/live acceptance in R5–R7.

<!-- alpha-phase-review:start -->
Phase review: R4 — 2026-09-12. Implementation and verification status: [Product status](docs/STATUS.md).
Delivery/merge status is recorded by the linked phase pull request.
<!-- alpha-phase-review:end -->
