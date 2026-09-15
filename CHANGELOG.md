# Changelog

## 1.1.5 — Direction A live-view finish

- Strengthen the native Rig hierarchy with a distinct machine identity, full-height meters, a readable 60-second chart, and structured recent outcomes.
- Put run and log navigation in the command center while retaining service lifecycle controls on Manager.
- Refine the remaining operational views and validate them against installed production-data screenshots.

## 1.1.4 — Structured native view refinement

- Replace generic operational text dumps with selectable native records, context-specific status headers, detail readback, and an advanced canonical projection.
- Add tool search, pack filtering, capability selection and details while retaining the Manager-owned invocation path.
- Give LM Studio MCP explicit Primary, Fallback, and CLU role status cards; distinguish Autonomy from retained-context Continuity.
- Group persistent Settings into readable configuration, rollover, maintenance, and verification surfaces.

## 1.1.3 — All-view visual finish

- Finished the tool workbench and operational destinations with the same layered command-center cards, structured actions, bounded live-data panes, and monospace technical readouts used by the Rig dashboard.

## 1.1.2 — Command center fit and finish

- Tightened the Rig viewport so the complete operational action cluster remains above the fold at the reference desktop size.

## 1.1.1 — Direction A command center

- Rebuilt the native Windows shell around the selected Obsidian Command Center direction, including dark window chrome, a compact production command strip, high-density telemetry cards, live sparklines, an operational chart grid, status rows, and a consistent premium surface system across every view.
- Preserved the authenticated Manager data path, existing navigation destinations, native command handlers, and release provenance while advancing the Windows package to `1.1.1.0` for a safe in-place upgrade.

## 1.1.0 — Windows product recovery

- Removed the global WinUI busy gate. Live telemetry now coalesces independently while user commands retain a bounded FIFO disposition, and closing the window cancels both lanes.
- Made ordinary GUI startup attach to an existing matching Manager or launch the packaged sibling Manager automatically, then verify the authenticated pipe handshake before reporting success.
- Preserved pending Provider and Settings edits across delayed Manager readback, and derived run/project authority internally instead of asking users for client IDs, generations, or other plumbing identifiers.
- Added the dedicated `clu` LM Studio MCP role with the exact four continuity-control tools, strict closed schemas, fail-closed shared-MCP behavior, three-role transactional deployment, and three-role health reporting.
- Raised the stable runtime and package version to 1.1.0 / `1.1.0.0`; Release packaging remains bound to a clean committed source tree and exact four-executable payload receipt.

## 1.0.0 — Windows production release

- Promoted runtime and package identity to stable 1.0.0 / `ForgeConductor.Windows`.
- Routed ordinary GUI launches to the production `%LOCALAPPDATA%\Forge Conductor` profile.
- Added strict compatibility with the released central schema 9, including immutable C008/C009 ledger checksums, continuation operation tables, and reset-generation metadata.
- Added an independently reconstructed schema-9 fixture that contains no user content and proves byte-stable compatible open behavior.
- Added Windows Release CI and secret-backed production-signing automation.
- Corrected App Installer metadata to reference the generated MSIX and added optional update-manifest generation.
- Updated shipped profile, package, CLI self-test, installer, and documentation language for the finished product.
- Retained `--alpha-root` only as a backward-compatible isolated-profile option.

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

### R2 parity follow-up — Complete native Rig instrumentation

- Added persistent PDH collection for per-logical CPU utilization/frequency, GPU engine utilization, and physical-disk bytes/operations per second, plus native volume capacity and relevant Forge/LM Studio process measurements.
- Extended the Manager snapshot and strict pipe codec with metric identity, timestamps, availability, cadence, source, memory scope, GPU engines, disks, volumes, and process samples; the GUI remains a typed consumer rather than a second collector.
- Expanded the WinUI Rig with logical-CPU bars, GPU adapter/engine and scoped-memory details, disk/volume and process panels, GPU/disk histories, workflow inventory, measured cadence, sample ages, and honest disconnected gaps.
- Tiered collection at a 250 ms base cadence with heavier one-second and five-second probes, bounded histories, and refresh cancellation during lifecycle actions. Focused collector/protocol/presentation and version-sensitive MCP/infrastructure tests pass.
- Advanced runtime/package identity to `0.9.5` / `0.9.5.0`. The source-bound signed candidate has MSIX SHA-256 `78683d3cef440a190932b8f8cb0fe53b39a6a1ac2940a80c7e4da9e4309a8f22` and ZIP SHA-256 `6bb46f8992599a4d7da5729570011c76cf6cd0657857505ffbb19ca4737b8e91`.
- Inspected the exact candidate under a disposable `--alpha-root`: real RTX 4090 engines, 32 logical CPUs, three volumes, live disk rates, relevant processes, workflow counts, disconnect state, Manager start/reconnect, and native keyboard focus were exposed. High Contrast passed. A 150% text pass found status-banner clipping; the layout was repaired, rebuilt, repackaged, and then passed a top-to-bottom 150% walkthrough with host settings restored.

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

### R6 acceptance closeout — Managed continuity guard

- Corrected the earlier rollover record: the terminal context-budget block was emitted by Forge's legacy desktop-chat invocation guard after repeated native calls, while `PACKAGE_OK` belonged to a separate exact-package run.
- Exempted Manager-owned `managed-run-v1` traffic from that legacy repetition/handoff guard while retaining normal routing, authorization, and audit behavior; ordinary MCP desktop chats retain their existing protection.
- Added a 12-identical-call guard regression and reconciled the managed-run service regression at 80 tool turns. The five focused suites and full x64 Debug/Release product builds passed.
- Advanced runtime/package identity to `0.9.4` / `0.9.4.0`. The source-bound signed candidate has MSIX SHA-256 `937e503c3198d829907aff2a349067ad8f21c7cec54df071d668a531eb65c586`.
- Ran the exact 0.9.4 GUI and Manager against real LM Studio. Run `34c078f8-6256-4b03-80e1-936e2e50f2f4` completed canonical rollover to successor `4ddd93de-6cc6-413f-b5fd-90da72e074d8`, performed the remaining effect, and returned terminal text `DONE` without the legacy block.
- Preserved the four retired remote branch heads in one verified Git bundle before guarded deletion. Added an exact administrator/test-account handoff for the still-open installed lifecycle; no elevated trust workaround was attempted.

### R6 internal Alpha completion continuation — Installed evidence integrity

- Added `-PreflightOnly` to the engineering installation helper. It validates package, certificate, signer, machine trust, prior version, and update eligibility without registering the package.
- Require the exact development publisher in Local Machine Trusted People before deployment and report the authorized administrator action when it is absent.
- Write a unique timestamped JSON receipt after every successful install or update so lifecycle evidence cannot overwrite a prior result.
- Rebuilt the source-bound 0.9.5.0 candidate from commit `c77c45386b25d7b76270c3685b79c172f41526c8` and tree `2bb16d60c7616f3d6f31ec94c85192dfe30db349`. Its MSIX SHA-256 is `9c772fcc9646f1e876f83c59c9e59a189f6f6881bcd603eb290dd589d5129a74` and ZIP SHA-256 is `2874a1cdf51d6861e780a32b599172f9ca0d84fe9ad9a9bb5eb92b55e3bb8dd4`.
- Made conflicting `-PreflightOnly -TrustDevelopmentPublisher` invocation fail immediately; the focused regression proves that neither certificate import nor package deployment is called.
- Changed ordinary GUI startup to the durable `%LOCALAPPDATA%\Forge Conductor Internal Alpha` profile. Its Manager lease, named pipe, DPAPI token, view state, projects, memory, and continuity stay separate from the preserved legacy store, and a persistent header identifies the profile and exact path on every page.
- Recorded the owner's decision to defer schema-9/C008/C009 migration as nonblocking backlog while preserving the legacy database and related files unchanged.
- Rebuilt and fully rehashed the persistent-profile MSIX from application commit `dab23aa8555a37203ba11136c58bb7f799317356`; its SHA-256 is `3efd692af03e15b7d8e5dad95be8119563e08c158c48b6df1dbf26ad899578c9`. The final companion distribution records application and distribution provenance and has ZIP SHA-256 `bd098a2b672c528ce17233212980464e45dc628a5d1957b33800a5694e22c098`.
- Added bounded packaged-Manager startup diagnostics so an early child-process failure reports sanitized stderr/stdout instead of only a generic exit code.
- Fixed MSIX AppData virtualization for the durable Internal Alpha profile with one narrowly scoped manifest exclusion and a focused regression that fences the legacy profile out of that capability.
- Verified exact machine trust, read-only preflight, current-package registration, Start launch, WindowsApps GUI/Manager identity, durable registry survival across reinstall, absence of package-private profile storage, and the installed 53-tool/project/isolation/filesystem/search/Git/shell/memory-restart workflow. The disposable 0.9.4→0.9.5 update and uninstall/reinstall lifecycle remains open.

### R6 simulated acceptance closeout

- Added a reproducible isolated lifecycle that rehashes all 323 manifest-listed files in both retained candidates, validates both signed packages, stages the real 0.9.4 then 0.9.5 CLI/MCP payloads, and verifies version/self-test behavior.
- Proved settings, workspace files, two-project isolation, legacy memory, project memory, and an unrelated LM Studio MCP configuration survive the simulated upgrade, uninstall, and reinstall. The owner explicitly accepted simulated testing for this remaining gate; no actual disposable-machine package deployment is claimed.
- Hardened the full Release test matrix against Windows excluded TCP ranges, asynchronous shutdown publication, Release-disabled managed-run assertions, and the current central schema version. All 150 configured tests pass.
- Accepted the signed 0.9.5.0 distribution for Internal Alpha. Legacy schema-9/C008/C009 migration remains deferred, and the preserved legacy profile remains untouched.

### R7 — Documented delivery

- Reconciled every active first-party document with the implemented capability, exact acceptance evidence, honest limitations, and current PR dependency chain.
- Retained and independently rehashed the unchanged tested 0.9.2.0 candidate: MSIX `4f43569b45438202d10cbfb67da4e456a04d65a80bb4b33177a3c94cfb74a695`, distribution ZIP `18e43f5508499b56ec802447cfb98dfe8bfc048ba1f649eb1da657f0ae28f8dd`, source commit `d8a2d68c80f2fd090aa36466a517725a0eb59445`.
- Kept the installed lifecycle visibly open. The later owner decision defers legacy schema-9 migration outside the first Alpha gate. PR #22 merged at `35f61de3c5a8159e20752a3843e5f26bfa5290c9`; PR #23 records the earlier acceptance closeout.

### Remaining Alpha work

- No required Internal Alpha functionality or acceptance work remains. Deliver this tested closeout through the normal pull-request and merge workflow, then synchronize local and GitHub `main`. Legacy schema-9 migration remains deferred outside the accepted first Alpha scope.

<!-- alpha-phase-review:start -->
Phase review: R6 simulated acceptance closeout — 2026-09-13. Implementation and verification status: [Product status](docs/STATUS.md).
Delivery/merge status is recorded by the linked phase pull request.
<!-- alpha-phase-review:end -->
