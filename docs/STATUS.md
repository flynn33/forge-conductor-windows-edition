# Product status

The 1.3.3 release retains the simplified Workspace, complete project-memory maintenance, constrained-host tool discovery, and reliable outer MCP deadline from 1.3.2. It adds explicit LM Studio disclosure of the authorized project folder, ordered instruction-package folders, and active development-policy source/revision. Verification is recorded in [the 1.3.3 validation record](validation/LM-STUDIO-WORKSPACE-CONTEXT-1.3.3.md).

| Area | Implemented behavior |
|---|---|
| Navigation | Four normal destinations: Workspace, Rig, Activity, and Settings. |
| Workspace | Project/provider selection, persistent ordered package queue, CLU binding/coverage/findings, independent continuity toggle, readiness summary, and guarded Display All memory paging. |
| Packages | Streamed hashing, deterministic universal inventory, explicit directory/reparse/failure records, bounded safe text derivation, paging/resume, retry, removal, revision identity, and ordered run attachment. |
| Governance | Local/remote policy binding, immutable revision and coverage state, evidence evaluation, deduplicated findings, notifications, correction evidence, and redacted export. |
| Continuity | Existing Manager-owned context continuity remains available and is skipped when the selected project/provider preference is off. It is not controlled through CLU tools. |
| Execution | Manager-owned Responses runs, authorized native tool calls, pause/resume/stop, reattachment, and persisted provider binding. |
| Telemetry | Native CPU, RAM, GPU, disk, volumes, processes, provider, store, continuity, and workflow observations. Unavailable observations remain explicit. |
| Activity/Doctor | Bounded project-correlated package and CLU activity is projected with persisted identifiers. Settings Doctor checks project/package queue/cursor, CLU repository/notifications/export, continuity provider binding, memory, migration, and schema alignment. |
| Data maintenance | Scrollable project-memory rows support visible per-row deletion and extended multi-selection deletion through `project_memory.forget`; continuity reset and exact-confirmation scope reset remain separate. |
| Native host tools | Git and PowerShell 7 are discovered by exact executable path and receive a bounded explicit Windows tool environment even when the MCP launcher starts with a minimal `PATH`. |
| LM Studio MCP | Primary, Fallback, and CLU registrations and synchronized bridges use an exact 180-second outer request deadline. Initialization and `forge_status` disclose the authorized project, ordered instruction-package folders, and active policy identity. Missing or stale values are repairable drift. Fallback remains separately selected; it is not an automatic call replay path. |
| Persistence | Durable per-user state with legacy instruction-package migration and compatible governance-state migration. |
| Packaging | Signed MSIX workflow with payload hashes, source provenance, certificate verification, and installer helper. |

Policy interpretation is intentionally honest: binary/opaque entries and access gaps remain visible instead of being omitted. CLU findings and policy coverage inform correction work but do not block unrelated execution or claim semantic proof beyond recorded evidence.

LM Studio desktop chats and Manager-owned tasks remain separate. Existing desktop chats without a native task binding are not automatically enrolled in managed continuity.

Live supported-provider successor creation, restore, acknowledgement, and fencing have not been rerun for this 1.3.3 release. Enabled preferences are reported as `Preparing`; this status does not claim live continuity qualification or an `Active` lifecycle.

The `v1.3.3` GitHub release is the latest published package.

Ordinary data remains at `%LOCALAPPDATA%\Forge Conductor`. The historical `--alpha-root` option supports isolated validation. Historical reports describe their own artifacts and are not evidence for newer binaries.
