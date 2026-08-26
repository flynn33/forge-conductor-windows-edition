# P07-001: Winsqlite Persistence Boundary and Ownership

Status: Accepted

Date: 2026-08-25

## Context

P07 must provide the Windows persistence foundation without implementing the
project-memory, legacy-memory, agent-session, or continuity application
behaviors assigned to P08 through P11. The existing
`ForgeConductor.Persistence.Windows` CMake target is an interface placeholder.
The governing architecture requires Winsqlite3, strict object ownership,
constructor injection, bounded operations, typed errors, and no database or
native ownership types in Domain or Contracts.

## Decision

- Replace the persistence interface placeholder with a real static C++20
  library linked privately to the Windows SDK `winsqlite3` import library.
- Keep every `sqlite3`, `sqlite3_stmt`, and `sqlite3_backup` type in private
  persistence implementation headers under
  `src/Persistence/Windows/Detail/`. No Winsqlite or Windows handle type may
  appear in Domain, Contracts, or the public persistence facade.
- Use separate move-only, final RAII owners for connections, statements,
  transactions, backups, operation callback installation, and database
  namespace leases. Destructors are `noexcept`; explicit close/commit methods
  return typed `Domain::Result` values.
- Serialize access to one connection through its owning object. Do not use a
  process-wide connection, mutable global registry, service locator, or
  singleton. Cross-process schema initialization is serialized by a bounded,
  cancellable per-database namespace lock.
- Install a busy callback and a progress callback for the duration of each
  database operation. They observe the operation cancellation token and
  deadline, and cap lock waiting at three seconds even when a caller supplies a
  later deadline. No operation inherits an unbounded SQLite wait.
- Configure each connection with extended result codes, defensive mode,
  trusted-schema off, load-extension off, explicit SQLite limits, foreign keys,
  WAL, and the database-specific synchronous policy. Open with `READWRITE`,
  `CREATE`, `FULLMUTEX`, `PRIVATECACHE`, `NOFOLLOW`, and `EXRESCODE`, using the
  explicit non-default VFS owned by the database namespace.
- Map `BUSY` and `LOCKED` to `database_busy`, `INTERRUPT` to `cancelled` or
  `deadline_exceeded` from the active context, `FULL` to `storage_full`,
  `CORRUPT` and `NOTADB` to `integrity_failure`, constraint conflicts to
  `conflict`, and unsupported layouts to `unsupported_version`. Exceptions are
  converted to `internal_failure` before crossing a method boundary.
- Expose only thin, final `WindowsCentralDatabase` and
  `WindowsProjectDatabase` facades for open/migrate, schema inspection,
  integrity check, online backup, and close. They receive application paths and
  runtime diagnostics through explicit injection and own an `OpenDatabase`
  runtime lease for the connection lifetime. Repository and MCP behavior will
  compose over the private kernel in later phases.

## Consequences

P07 can prove storage mechanics without stubbing broad application contracts.
Later persistence repositories remain implementation details of the same
layer, and application services continue to depend only on Contracts. A caller
cannot retain a raw statement, connection, backup, path handle, or callback
past its documented owner.

The three-second maximum preserves the macOS compatibility bound while adding
cancellation and deadline behavior absent from a fixed `busy_timeout` alone.
Connection-level serialization intentionally favors deterministic ownership
over concurrent use of one SQLite handle; independent project databases remain
independently concurrent.

## Evidence basis

- `.forge-codex/instructions/architecture/PERSISTENCE_AND_MIGRATION.md`
- `.forge-codex/instructions/architecture/OOP_AND_OWNERSHIP.md`
- `.forge-codex/instructions/architecture/PROJECT_MEMORY.md`
- `.forge-codex/instructions/plans/phases.json` (`P07`)
- `.forge-codex/instructions/plans/gates.json` (`G07`)
- Windows SDK 10.0.26100.0
  `um/winsqlite/winsqlite3.h`, reporting SQLite 3.51.1
