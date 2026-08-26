# Persistence and macOS migration

## Windows locations

Default per-user root:

```text
%LOCALAPPDATA%\Forge Conductor
```

Support `FORGE_CONDUCTOR_HOME` for development/tests. Paths include config, central store, projects, logs, exports, handoffs, session-host ledger, manager state, deployment backups, and evidence.

## Databases

Use Winsqlite3 with RAII wrappers, migrations, transactions, prepared statements, busy timeout, integrity checks, and backups.

Separate:

- central operational database: audit, presence, agent sessions, legacy memory, global handoff index;
- per-project database: project memory and project continuity.

## Configuration

Use versioned JSON with atomic replace and backup. Unknown fields are preserved where forward compatibility requires it. Secrets are stored separately with DPAPI.

## Import from macOS

Provide a native import wizard and CLI:

```text
forge-conductor import-macos --source <folder-or-archive> [--dry-run]
```

Import:

- config values with platform translation;
- central SQLite data;
- project registry and project databases;
- memory notes;
- context handoffs and continuity operations;
- agent sessions;
- audit records when compatible;
- playbooks/resources;
- LM Studio registration intent, not Apple paths.

Never copy LaunchAgents, Apple entitlements, absolute macOS executable paths, Metal resources, or Keychain payloads.

Every import supports dry-run, schema validation, checksums, backup, transactional commit, idempotency, detailed report, and rollback.
