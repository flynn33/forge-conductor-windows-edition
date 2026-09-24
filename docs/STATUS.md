# Product status

Version 1.2.0 adds automatic project preparation and development-policy adoption. Release qualification evidence is tracked in [Automatic setup validation](validation/AUTOMATIC-SETUP-1.2.0.md).

| Area | Implemented behavior |
|---|---|
| Setup | Folder selection starts/attaches the Manager, registers and verifies the project, prepares a compatible downloaded LM Studio model, and checks an actual response. Retry and cancellation are available. |
| Governance | GitHub/local policy snapshots, pinned commit/digest, complete text retrieval, explicit adoption and review, common tool authorization for reviewed paths and exact commands. Private repositories use existing Git authentication. |
| Help | 42 searchable offline articles covering setup, model recovery, policy, tools, continuity and evidence. |
| Execution | Manager-owned Responses runs, authorized native tool calls, pause/resume/stop, reattachment and context continuity. Provider bindings persist across restart. |
| Projects/tools | Stable identities, authorized aliases, isolated memory, instruction packages and 58 native tools, including read-only policy retrieval. |
| Telemetry | Native CPU, RAM, GPU, disk, volumes, processes, provider, store, continuity and workflow observations. Unavailable observations remain explicit. |
| Persistence | Durable per-user state; immutable migration ledger supports released stores. Validation uses disposable profiles. |
| Packaging | Signed MSIX, payload hashes, source provenance, certificate and installer helper. The release metadata identifies development signing when used. |

Policy import does not automatically prove arbitrary prose requirements. The reviewer must resolve source obligations; the runtime enforces the resulting accepted permissions. Exact approved shell commands are not an operating-system filesystem sandbox. Model readiness does not prove every task or dependency will work.

LM Studio desktop chats and Manager-owned tasks remain separate. Existing desktop chats without a native task binding are not automatically enrolled in managed continuity. The dedicated CLU role exposes four continuity-control tools.

Ordinary data remains at `%LOCALAPPDATA%\Forge Conductor`. The historical `--alpha-root` option supports isolated validation. Historical candidate reports describe their own artifacts and are not evidence for newer binaries.
