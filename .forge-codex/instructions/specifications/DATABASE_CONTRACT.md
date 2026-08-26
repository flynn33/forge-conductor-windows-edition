# Database contract

Codex must extract the exact macOS table/migration inventory and document the Windows mapping before implementation.

Minimum central store domains:

- schema migrations;
- audit events;
- client presence;
- agent sessions/runs;
- legacy memory notes;
- legacy context handoffs;
- manager/deployment state;
- bounded diagnostics metadata.

Minimum per-project domains:

- project metadata;
- memory records;
- tags and record tags;
- typed links;
- event journal;
- continuity handoffs;
- rollover operations;
- state transitions;
- session ledger references.

Every migration is forward-only, transactional when supported, idempotent, backed up, and tested from every released schema fixture. Database code uses Winsqlite3 prepared statements and RAII.
