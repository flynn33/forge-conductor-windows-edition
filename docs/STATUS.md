# Product status

Release 1.3.0 implements the simplified Workspace workflow, universal ordered instruction-package intake, CLU development governance, and an independent Manager-owned project-plus-provider automatic-continuity preference. Verification is recorded in [the 1.3.0 validation record](validation/SIMPLIFIED-WORKFLOW-CLU-1.3.0.md), and the published engineering distribution is development-signed.

| Area | Implemented behavior |
|---|---|
| Navigation | Four normal destinations: Workspace, Rig, Activity, and Settings. |
| Workspace | Project/provider selection, persistent ordered package queue, CLU binding/coverage/findings, independent continuity toggle, and readiness summary. |
| Packages | Streamed hashing, deterministic universal inventory, explicit directory/reparse/failure records, bounded safe text derivation, paging/resume, retry, removal, revision identity, and ordered run attachment. |
| Governance | Local/remote policy binding, immutable revision and coverage state, evidence evaluation, deduplicated findings, notifications, correction evidence, and redacted export. |
| Continuity | Existing Manager-owned context continuity remains available and is skipped when the selected project/provider preference is off. It is not controlled through CLU tools. |
| Execution | Manager-owned Responses runs, authorized native tool calls, pause/resume/stop, reattachment, and persisted provider binding. |
| Telemetry | Native CPU, RAM, GPU, disk, volumes, processes, provider, store, continuity, and workflow observations. Unavailable observations remain explicit. |
| Activity/Doctor | Bounded project-correlated package and CLU activity is projected with persisted identifiers. Settings Doctor checks project/package queue/cursor, CLU repository/notifications/export, continuity provider binding, memory, migration, and schema alignment. |
| Persistence | Durable per-user state with legacy instruction-package migration and compatible governance-state migration. |
| Packaging | Signed MSIX workflow with payload hashes, source provenance, certificate verification, and installer helper. |

Policy interpretation is intentionally honest: binary/opaque entries and access gaps remain visible instead of being omitted. CLU findings and policy coverage inform correction work but do not block unrelated execution or claim semantic proof beyond recorded evidence.

LM Studio desktop chats and Manager-owned tasks remain separate. Existing desktop chats without a native task binding are not automatically enrolled in managed continuity.

Live supported-provider successor creation, restore, acknowledgement, and fencing have not been rerun for this 1.3.0 release. Enabled preferences are reported as `Preparing`; this status does not claim live continuity qualification or an `Active` lifecycle.

The `v1.3.0` GitHub release carries the source-bound development-signed engineering package and checksums. It supersedes the earlier package built from `d514719e42baa1ca647fd26fbb51666a4670cb3d`.

Ordinary data remains at `%LOCALAPPDATA%\Forge Conductor`. The historical `--alpha-root` option supports isolated validation. Historical reports describe their own artifacts and are not evidence for newer binaries.
