# Project-scoped memory

## Identity

Project memory is explicitly scoped. Never select a project because it is merely the newest continuity record.

`ProjectIdentityResolver`:

- canonicalizes an existing directory using handles and `GetFinalPathNameByHandleW`;
- resolves reparse points and rejects path escapes;
- derives optional Git remote identity from source evidence;
- maps aliases and repository identity to a UUID;
- stores a bounded alias list;
- supports many simultaneous projects;
- exposes explicit project selection to GUI, CLI, MCP, and session host.

## Storage

Use one Winsqlite3 database per project under:

```text
%LOCALAPPDATA%\Forge Conductor\projects\<project-id>\memory.sqlite
```

Store registry metadata separately with atomic replacement and a cross-process lock. Use WAL where supported, busy timeout, foreign keys, schema migrations, prepared statements, and bounded transactions.

## Repository cache

- constrained profile: maximum 4 open project repositories;
- standard profile: maximum 8;
- expanded profile: maximum 16;
- LRU eviction closes statements, checkpoints WAL as policy allows, and closes the connection;
- active transactions cannot be evicted.

## Required tools

Implement all twelve `project_memory.*` tools with compatible schemas:

- initialize
- remember
- remember_batch
- search
- get
- update
- forget
- list_recent
- link
- export
- import
- status

Requirements include deduplication, redaction, optimistic version checks, bounded pagination, stable ordering, tombstones, links, checksummed export/import, preview mode, project isolation, deadlines, idempotency, integrity checks, and typed errors.

## Settings and reset

The Manager settings surface and CLI must support:

- reset one project's memory;
- reset one project's continuity only;
- reset both for one project;
- detach an alias without deleting memory;
- export before reset;
- reset all projects;
- purge legacy global memory separately.

Destructive operations require a typed confirmation value in the API, create an audit event, close cached repositories, execute transactionally, and verify deletion. Automated tests use noninteractive confirmation tokens.
