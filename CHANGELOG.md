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
- Added two-second coalesced refresh, disconnected/stale rendering, resize redraw, persisted page selection, and window-close cancellation. Real isolated process probes confirmed repeated refresh and Manager survival after GUI close; a later native review verified rendered telemetry, while complete keyboard and scaled/high-contrast inspection remain open.

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

### R5 — Signed installer and data compatibility

- Established stable product version `0.9.1` and MSIX identity/version `ForgeConductor.Windows.Alpha` / `0.9.1.0` across the native hosts and package manifest.
- Bound the GUI, Manager, CLI, and SessionHost Release binaries to one committed staging receipt; added complete WinUI/runtime/resources payload validation, embedded provenance, per-file hashes, signature/certificate checks, unpack-and-rehash verification, and a minimal distribution bundle containing no private signing material.
- Added strict install/update validation for package, certificate, identity, publisher, and increasing version while preserving user data through normal MSIX update/uninstall semantics.
- Added a dedicated Manager exit code and actionable GUI explanation when the central store is newer than supported. The schema-9 disposable probe leaves the database unchanged and preserves `--alpha-root` as the explicit isolated alternative.
- Applied the owner execution correction in place: native compaction continues the current slice; draft, checks, readiness, merge, and acceptance are separate; and a single reused continuation is allowed when a primary phase PR merges early.

### R6 — Candidate acceptance and defect repair

- Fixed Alpha profile view-state isolation so project/page selection cannot leak between disposable roots, stale project IDs clear against the Manager snapshot, and an unrelated first project is never selected implicitly.
- Advanced the product and stable MSIX identity to version `0.9.2` / `0.9.2.0`, then built, signed, unpacked, and rehashed the complete committed Release package.
- Exercised the exact unpacked package: CLI version, GUI and Manager paths, live CPU/RAM and explicit GPU capability state, single-Manager detach/reattach, and independent two-profile project selection all passed.
- Completed the native Settings accessibility work with keyboard operation of the context slider and exact token value, actual Windows High Contrast, and actual 150% text scaling; every temporary system setting was restored.
- Retained installation and live-provider gates as blocked: Windows returned `0x800B0109` without machine-level publisher trust, and LM Studio was still unavailable at `127.0.0.1:1234`.

### R6 continuation — Live managed continuity

- Repaired the repository-backed managed-run start transaction so each new run creates the admissible open session and active binding before it enters the running state.
- Moved continuity observation after persisted native tool effects, retained bounded completed-work summaries in the canonical handoff, and instructed the fresh successor to continue without repeating those effects.
- Removed the remaining ordinary-run turn cap; completion, operator cancellation, a real failure, or context-triggered continuity now determine when managed work stops.
- Moved the native session ledger under the memory root and added legacy-root migration before SQLite opens, preventing the ledger and central database from sharing an atomic-replace directory.
- Updated LM Studio bootstrap tool selection to its supported required mode and proved a real context-triggered rollover with authoritative usage, saved handoff, fresh provider root, structured acknowledgment, predecessor fencing, and useful successor filesystem effects.
- Advanced runtime/package identity to `0.9.3` / `0.9.3.0`; built, signed, unpacked, and rehashed the source-bound candidate. MSIX SHA-256 is `9d9b899e7133cb9b46ac3f6221df5673e0bb7f55f1f92ef979d08ab77324607f`.

### R7 — Documented delivery

- Reconciled every active first-party document with the implemented capability, exact acceptance evidence, honest limitations, and current PR dependency chain.
- Retained and independently rehashed the unchanged tested 0.9.2.0 candidate: MSIX `4f43569b45438202d10cbfb67da4e456a04d65a80bb4b33177a3c94cfb74a695`, distribution ZIP `18e43f5508499b56ec802447cfb98dfe8bfc048ba1f649eb1da657f0ae28f8dd`, source commit `d8a2d68c80f2fd090aa36466a517725a0eb59445`.
- Kept installed lifecycle and authentic schema-9 compatibility visibly open. PRs #19–#21 merged in dependency order and local/origin/GitHub main synchronized at `a161443c974bce594ef7a655a88d02b20dec6040` before the R6 continuation.

### Remaining Alpha work

- Complete machine-trusted install/Start/update/uninstall acceptance and recover authentic C008/C009 history before accepting the owner schema-9 store. Live productive continuity is now verified.

<!-- alpha-phase-review:start -->
Phase review: R6 continuation — 2026-09-12. Implementation and verification status: [Product status](docs/STATUS.md).
Delivery/merge status is recorded by the linked phase pull request.
<!-- alpha-phase-review:end -->
