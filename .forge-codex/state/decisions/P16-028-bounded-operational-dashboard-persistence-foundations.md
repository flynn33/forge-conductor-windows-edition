# P16-028: Bounded Operational Dashboard Persistence Foundations

Status: Accepted

Date: 2026-08-30

## Context

The production Manager dashboard must report current agent sessions, recent
session history, connected clients, and diagnostic log records from owned
runtime state. Its persistence boundary cannot infer a client's workspace from
an ephemeral process identifier, return unbounded database content, or permit
malformed persisted values to enter domain snapshots.

The Manager also needs an exact administrative close operation for a concrete
session and a bounded diagnostic-log reader. These operations must preserve the
repository's transaction, cancellation, deadline, authority, and resource
ownership rules before they are composed into the production Manager host.

## Decision

- Central schema version 7 stores the canonical working directory with every
  client-presence row. Migration retains only legacy rows whose owner and
  working directory can be proven, and normalizes retained timestamps to the
  fixed-width millisecond UTC representation used by current writers.
- Session and presence ordering is backed by indexes whose leading columns
  match the dashboard queries. Fixed-width UTC text makes the indexed lexical
  order chronological for current and migrated rows.
- `WindowsDashboardOperationalRepository` is a neutral persistence adapter. A
  single SQLite statement returns bounded open sessions, recent sessions, and
  client presence for one consistent database snapshot. It rejects malformed
  identifiers, paths, timestamps, process identifiers, and UTF-8 as integrity
  failures while preserving operational cancellation, deadline, lock, and
  database errors.
- Result counts use one-row overflow probes where overflow is an error. Shared
  aggregate text budgets are enforced before values are retained: one MiB for
  session text and 512 KiB for presence text.
- `WindowsAgentSessionRepository::administrativelyClose` closes only the named
  concrete session in an immediate transaction, preserves enriched run data,
  clamps the close time against the prior update time, removes only a matching
  active pointer, rewrites the run projection, and rolls back atomically on
  failure.
- `WindowsDiagnosticLogTailReader` reads only the newest bounded JSONL records
  with fixed-size backward scans. It preserves original order and rejects
  partial, malformed, oversized, non-UTF-8, NUL-containing, bare-CR, reparse,
  hard-linked, or authority-escaping input.

## Consequences

The next Manager slice can compose truthful operational snapshots and
diagnostic tails without adding SQL or filesystem behavior to a view model or
transport adapter. Repository memory use is bounded independently of database
size, cancellation and deadlines retain their typed meaning, and a selected
session can be closed without disturbing a replacement active session.

Schema version 7 intentionally discards legacy client-presence rows whose
canonical working directory cannot be proven. Presence is ephemeral and is
re-established by live clients; inventing an authority-bearing path would be
unsafe and behaviorally incorrect.

This checkpoint does not provide the Manager operational adapter, doctor
service, telemetry-unavailable projection, production composition root,
real-process evidence, live Edge dashboard behavior, retained native UI
automation, or the authoritative G16 gate.

## Evidence basis

- `src/Persistence/Windows/Migrations/CentralMigrations.cpp`
- `src/Persistence/Windows/WindowsClientPresenceRepository.cpp`
- `src/Persistence/Windows/WindowsAgentSessionRepository.cpp`
- `src/Persistence/Windows/WindowsDashboardOperationalRepository.cpp`
- `src/Infrastructure/Windows/WindowsDiagnosticLogTailReader.cpp`
- `tests/Persistence/CentralMigrationTests.cpp`
- `tests/Persistence/ClientPresenceRepositoryWindowsTests.cpp`
- `tests/Agents/AgentSessionRepositoryWindowsTests.cpp`
- `tests/Persistence/DashboardOperationalRepositoryWindowsTests.cpp`
- `tests/Infrastructure/WindowsDiagnosticLogTailReaderTests.cpp`
