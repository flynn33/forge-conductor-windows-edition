<p align="center">
  <img src="images/banner-memory.jpg" alt="Forge Conductor persistence" width="100%">
</p>

# Persistence

The 1.3.14 source repair removes `FILE_ADD_FILE` from SQLite's retained directory anchor while retaining traversal/read access and namespace pins. The observed schedule/logger stall came from that write-capable handle contesting the diagnostic directory's no-write sharing. Diagnostic sharing protection remains intact; file creation still goes through native main/WAL/SHM opens and OS permissions. Fresh live WAL/SHM diagnostic-coexistence and native creation-denial cases passed with all 67 persistence internal tests in 22.12 seconds. The initial creation-denial fixture wrongly assumed close removed sidecars despite `NO_CKPT_ON_CLOSE`; its explicit checkpoint and identity-pinned closed-sidecar cleanup corrected the fixture without changing product code. The subsequent complete disposable-profile scheduling/reconnect probe passed. Original failed probes and hang evidence remain in the [capability verification ledger](https://github.com/flynn33/forge-conductor-windows-edition/blob/main/docs/validation/HOST-CAPABILITIES-1.3.14.md); the complete source matrix passed as recorded in that ledger, while signed-package/installed Qwen qualification remains pending.

Forge Conductor uses Windows `winsqlite3`, atomic JSON files, and DPAPI-backed secret storage behind RAII C++ repositories.

## Production profile

```text
%LOCALAPPDATA%\Forge Conductor
```

This profile stores configuration, central operations, registered projects, per-project memory, continuity, evidence, deployment backups, view state, exports, and bounded logs. Normal MSIX removal preserves it.

## Stores

| Store | Contents |
| --- | --- |
| Central database | audit, presence, durable agent sessions, legacy memory/handoffs, Manager/run metadata |
| Per-project database | memory records, tags, links, project sessions, instruction-package records, continuity state |
| Project registry | stable project identities and authorized aliases |
| Configuration | versioned atomic JSON with forward-preserved fields where required |
| Secret storage | DPAPI CurrentUser values, separate from JSON/database payloads |
| Session ledger | bounded logical-session identity and recovery metadata; no transcripts |
| Independent workers | separate bounded sealed worker receipts with frozen scope, actual output/errors, and interruption evidence |
| Scheduled tasks | separately authorized atomic Manager-owned file with future triggers, frozen scope, actual history and notification receipts |

## Database discipline

- foreign keys and prepared statements
- explicit transactions
- immutable migration history
- migration hashes and schema checks
- busy timeouts and WAL where supported
- backup before destructive migration
- future schema refusal without mutation
- integrity checks and bounded repositories

Released schema history migrates forward in place. Historical audit rows are not assigned a fabricated project scope.

Schedule admission persists a unique run before provider submission. Future triggers restore after Manager restart; uncertain interrupted effects block automatic replay until an explicitly authorized new attempt. Provider failure, notification failure and receipt persistence failure retain their distinct actual outcomes. This scheduler recovery does not establish native chat UI-handoff recovery after worker eviction.

Independent worker and read-only reviewer receipts retain complete sealed results by run ID. Their services each bound simultaneous runs to sixteen; finished terminal threads release those slots without removing receipts. The unpublished 1.3.15 candidate removed the earlier reviewer sixteen-record lifetime admission limit, retained in 1.3.17. Terminal reviewer status reloads the durable receipt and reports actual evidence integrity and SHA-256. Each serialized file is bounded to 4 MiB, including supported raw output up to 256 KiB and escaped task metadata. Recurring work does not automatically delete earlier human-readable outputs. Retained history needs disk space; actual storage failures remain explicit.

## Configuration and secrets

Configuration writes use atomic replace and backup. Secrets such as Manager nonces and dashboard bearer material are encrypted with DPAPI CurrentUser and redacted before logs, memory, telemetry, or exported evidence.

## Package/update behavior

The MSIX manifest narrowly excludes only the intended production profile from AppData write virtualization. A package contract test rejects a broader exclusion. Updates retain data; explicit reset and purge remain separate operator actions.

**Next:** [Project Memory](Project-Memory) · [Security](Security) · [Packaging](Packaging)
