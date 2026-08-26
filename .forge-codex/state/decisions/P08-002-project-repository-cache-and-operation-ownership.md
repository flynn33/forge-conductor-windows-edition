# P08-002: Project Repository Cache and Operation Ownership

Status: Accepted

Date: 2026-08-26

## Context

P08 requires simultaneous isolated project repositories with profile limits of
4, 8, and 16. An entry must not be evicted while an operation or transaction is
active, and shutdown must close every connection without relying on a singleton
or an unbounded queue. P07 already provides one serialized Winsqlite connection,
namespace lease, VFS, and runtime-ownership lease per project database.

## Decision

- Implement a final Windows repository factory behind
  `IProjectMemoryRepositoryFactory`. It owns the only strong cache reference for
  each open repository and tracks a monotonic LRU generation.
- A returned `shared_ptr<IProjectMemoryRepository>` is an explicit operation
  pin. Eviction is permitted only when the cache is the sole owner
  (`use_count() == 1`). The application service retains the pin through the
  complete synchronous repository call. If the bound is full and every entry is
  pinned, opening another project returns a retryable `database_busy` result;
  it never closes an admitted operation.
- Cache mutation is serialized and bounded. Database open and victim close occur
  outside the cache mutex. Pending opens reserve capacity before releasing the
  mutex, so published entries plus reservations can never exceed the active
  profile's 4/8/16 bound. A duplicate-open race is reconciled deterministically
  and the redundant repository is closed. At most one entry or reservation per
  project exists, and a close request against a pending open returns retryable
  `database_busy` instead of reporting a false successful close.
- Each final repository owns exactly one `WindowsProjectDatabase`, and that
  database continues to own exactly one serialized Winsqlite connection. SQL and
  Winsqlite types remain private to `ForgeConductor.Persistence.Windows`.
- Repository operations acquire the P07 serialized admission before touching the
  connection. All statements and transactions are stack-owned RAII objects.
  Batch and import mutations use one immediate transaction.
- Export and import additionally hold a bounded repository artifact-admission
  lease across database encoding, filesystem retention/publication, hostile JSON
  validation, preview, and transactional import. Repository close shuts this
  admission first and waits at most the contract's 60-second operation ceiling
  before closing the database. The Windows artifact store independently uses a
  fixed 16-stripe bounded executor keyed by project ID, so same-project artifact
  mutations serialize without creating one process-wide product-state owner.
- Eviction closes statements through lexical RAII and closes the connection. It
  does not checkpoint. Any future checkpoint is a separately admitted,
  cancellable maintenance operation; P07's no-checkpoint-on-close rule remains
  controlling.
- `close(project)` removes the entry from admission, rejects while externally
  pinned, and closes outside the cache lock. Shutdown stops admission, drains
  synchronous owners, and closes entries in deterministic order.

## Consequences

The resource-profile maximum is a hard gate rather than a hint. Independent
projects can execute concurrently while each connection remains serialized.
Eviction cannot invalidate a live operation, and close/destruction never starts
unbounded maintenance work.

## Evidence basis

- `.forge-codex/instructions/architecture/OOP_AND_OWNERSHIP.md`
- `.forge-codex/instructions/architecture/PROJECT_MEMORY.md`
- `.forge-codex/instructions/plans/resource-budgets.json`
- `.forge-codex/state/decisions/P07-001-winsqlite-persistence-boundary-and-ownership.md`
- `.forge-codex/state/decisions/P07-003-anchored-database-namespace-and-online-backups.md`
