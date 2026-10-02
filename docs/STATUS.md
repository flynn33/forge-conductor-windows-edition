# Product status

The 1.3.5 source candidate corrects Work Space model discovery and Responses-session bootstrap to consume LM Studio's native loaded-model inventory. It also aligns durable managed-run goals with bounded instruction-package content and addresses restart-idempotency, dispatch-gating, checkpoint-refresh, repeated-rollover identity, and orphan-cleanup gaps in automatic continuity. See [the 1.3.5 candidate notes](releases/1.3.5.md).

| Area | Implemented behavior |
|---|---|
| Navigation | Four normal destinations: Workspace, Rig, Activity, and Settings. |
| Workspace | Project/provider selection, loaded-LLM discovery through `/api/v1/models`, persistent ordered package queue, CLU binding/coverage/findings, independent continuity toggle, readiness summary, and guarded Display All memory paging. |
| Packages | Streamed hashing, deterministic universal inventory, explicit directory/reparse/failure records, bounded safe text derivation, paging/resume, retry, removal, revision identity, ordered run attachment, and a 128 KiB durable agent-goal envelope. An entry that cannot fit beside a task fails explicitly. Admission persists an exact dispatch-pending cursor plan; multi-row updates are atomic, restart replay reconciles the plan before provider/tool effects, and exact replay cannot consume newly changed package state. |
| Governance | Local/remote policy binding, immutable revision and coverage state, evidence evaluation, deduplicated findings, notifications, correction evidence, and redacted export. |
| Continuity | Manager-owned context continuity is skipped when the selected project/provider preference is off. Durable start idempotency preserves the originally admitted assignment, repeated rollovers receive unique sequenced operation/handoff identities, pre-successor checkpoint refresh uses an exact prior-digest CAS, and terminal runs abandon unconsumed checkpoints. A lease-owned pre-ingress pass abandons checkpoint-only orphans from an earlier Manager, while periodic maintenance preserves live checkpoints and resumes only durable successor intent. It is not controlled through CLU tools. |
| Execution | Manager-owned Responses runs, authorized native tool calls, pause/resume/stop, reattachment, and persisted provider binding. |
| Telemetry | Native CPU, RAM, GPU, disk, volumes, processes, provider, store, continuity, and workflow observations. Unavailable observations remain explicit. |
| Activity/Doctor | Bounded project-correlated package and CLU activity is projected with persisted identifiers. Settings Doctor checks project/package queue/cursor, CLU repository/notifications/export, continuity provider binding, memory, migration, and schema alignment. |
| Data maintenance | Scrollable project-memory rows support visible per-row deletion and extended multi-selection deletion through `project_memory.forget`; continuity reset and exact-confirmation scope reset remain separate. Native Manager coverage proves 101-record Display All paging and both deletion modes. |
| Native host tools | Git and PowerShell 7 are discovered by exact executable path and receive a bounded explicit Windows tool environment. Caller `PATH`/`PATHEXT`/`COMSPEC` values are replaced and HKCU `PATH` is excluded. |
| LM Studio MCP | Primary, Fallback, and CLU registrations and synchronized bridges use an exact 180-second outer request deadline. Initialization and `forge_status` disclose the authorized project, ordered instruction-package folders, and active policy identity. Same-host model traffic uses unauthenticated loopback; HTTP 401/403 reports that Require Authentication must be disabled. Discovery admits only loaded LLM instance IDs, accepts a bounded 2 MiB inventory, and excludes downloaded-but-unloaded and embedding models. Missing or stale values are repairable drift. Fallback remains separately selected; it is not an automatic call replay path. |
| Persistence | Durable per-user state with legacy instruction-package migration and compatible governance-state migration. |
| Packaging | Signed MSIX workflow with payload hashes, source provenance, certificate verification, and installer helper. |

Policy interpretation is intentionally honest: binary/opaque entries and access gaps remain visible instead of being omitted. CLU findings and policy coverage inform correction work but do not block unrelated execution or claim semantic proof beyond recorded evidence.

Project-memory Browse/Display All and automatic continuity are separate systems. Browse pages durable records; continuity creates and resumes provider successors at a managed-run context boundary. Evidence for one is not evidence for the other.

A fresh disposable post-fix live Manager-owned continuity run passed on the Release x64 source build, including successor create/bootstrap, structured acknowledgement, predecessor fencing, and productive successor work. This does not qualify packaged WinUI interaction or the installed operator workflow. Existing LM Studio desktop chats without a native task binding are not automatically enrolled.

Version 1.3.5 has an uninstalled development-signed MSIX candidate at `out/dist/release-1.3.5.0-20261002-002411/ForgeConductor-1.3.5.0-x64.msix` (SHA256 `bd4cb85f3567950697c6dc83a81fac6a9a6834a1bf68066bf70c82454ec0332c`). Release x64, G13 StaticOnly, and isolated simulated upgrade/reinstall validation passed. Installed UI testing and operator acceptance remain open. The `v1.3.4` GitHub release remains the latest published package.

Ordinary data remains at `%LOCALAPPDATA%\Forge Conductor`. The historical `--alpha-root` option supports isolated validation. Historical reports describe their own artifacts and are not evidence for newer binaries.
