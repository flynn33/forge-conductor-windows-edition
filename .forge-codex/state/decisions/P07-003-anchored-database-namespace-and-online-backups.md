# P07-003: Anchored Database Namespace and Online Backups

Status: Accepted

Date: 2026-08-25

## Context

The built-in Windows SQLite VFS opens the main database and WAL/SHM sidecars by
path. `SQLITE_OPEN_NOFOLLOW` is useful but does not transfer the P06
handle-relative authority model to every VFS lookup. The macOS project store's
raw pre-migration main-file copy is also not a coherent backup when committed
pages remain in WAL.

Opening through the built-in VFS after a separate path validation would restore
a leaf validation-to-open race. A full replacement of Windows locking and
shared-memory behavior would also be unnecessarily large. P07 therefore needs
a forwarding VFS whose ownership is explicit and whose file opens are pinned,
while the inbox VFS continues to implement byte-range locks and shared memory.

## Decision

- A final `DatabaseNamespaceLease` owns the canonical database directory,
  retained handles for every existing ancestor, a bounded exclusive
  initialization-lock handle, and the expected main/WAL/SHM identities. It
  rejects reparse points, case-sensitive directories, alternate streams,
  unsupported path forms, pre-existing multi-link database or sidecar files,
  and identity changes at operation boundaries.
- The namespace uses a multi-process anchor mode: ancestors request list,
  traverse, and attribute access; the final parent also requests add-file;
  handles share read and write but never delete. `FILE_DELETE_CHILD` is not
  retained. Exact child deletion uses a separately validated child handle.
- A final `AnchoredSqliteVfs` is registered as a non-default, per-namespace
  forwarding VFS. Its registration and name are owned by the database facade,
  bounded by the open-database budget, and removed only after every connection
  and file closes. It never calls process-global `sqlite3_shutdown`.
- Database filenames and sidecar suffixes are fixed constants selected by the
  facade. Callers cannot pass an arbitrary database filename or VFS name.
  SQLite URI handling and extension loading remain disabled.
- Each VFS `xOpen` first opens or creates the exact main, WAL, SHM, or rollback
  journal leaf through the anchored parent, validates its regular-file,
  non-reparse, non-delete-pending, single-link identity, and retains a
  no-delete lease before delegating I/O to the inbox Windows VFS. Unexpected
  basenames, open roles, URI forms, unnamed temporary files, and attach-related
  files are rejected. `xDelete`, `xAccess`, and `xFullPathname` remain inside
  the same fixed namespace; ordinary I/O, SQLite locks, and shared-memory calls
  forward to the inbox implementation.
- Revalidate directory anchors and all present database/sidecar identities
  before and after migrations, integrity checks, checkpoints, and backup
  publication. Prefer persistent WAL/SHM identities; cleanup is explicit
  bounded maintenance rather than an ambient pathname deletion.
- Serialize initialization with an overlapped `LockFileEx` request. A stop
  callback signals a cancellation event, and the owner waits on the lock and
  cancellation events only until the operation deadline and three-second busy
  ceiling. Cancellation drains the native request before releasing its event,
  file handle, or directory anchors.
- Use `sqlite3_backup_init`, bounded page batches, and the operation callbacks
  for every migration or explicit backup. A staged destination is accepted only
  after `quick_check` returns exactly `ok`, the destination connection closes,
  its file handle is flushed, its single-link identity is rechecked, and it is
  atomically published in the anchored sibling namespace. Interrupted staging
  files are bounded and cleaned on the next locked initialization.
- A corrupt or non-database source cannot be trusted to drive the SQLite backup
  API. Quarantine therefore captures exactly the main, WAL, SHM, and rollback
  journal roles that exist at admission, retains those exact handles and file
  identities through evidence commit, and streams them into a checksummed
  evidence set. Absent roles must remain absent at every boundary; injected or
  substituted roles fail closed.
- Quarantine accepts at most 4 GiB (4,294,967,296 bytes) per captured file and
  at most 8 GiB (8,589,934,592 bytes) across the cohort. Size admission is an
  overflow-safe preflight before evidence creation. An over-limit corrupt
  cohort remains in place, returns the original `integrity_failure` with a
  recovery diagnostic, and makes no evidence claim.
- The source and evidence byte counts, SHA-256 digests, role set, and retained
  identities are revalidated immediately before committing the manifest. After
  manifest commit, any content, identity, role, or link-count mismatch returns
  `integrity_failure` with the committed evidence identifier and retains both
  source and evidence. Otherwise every captured source role is removed through
  its still-retained exact handle; all dispositions are attempted and partial
  removal is reported as an integrity-recovery failure rather than success.
- Once an integrity failure is detected, preservation uses a fresh non-cancelled
  operation context with a ten-minute deadline while retaining the initiating
  operation and correlation identifiers. Later cancellation or expiry of the
  caller's context cannot replace the primary `integrity_failure`. Rollback and
  connection-release failures are appended while all safe release steps still
  run; quarantine begins only after every SQLite/VFS handle has been released.
  Migration rollback also preserves that primary typed error if native rollback
  fails. Integrity close marks the connection poisoned before native close.
- Every connection enables `SQLITE_DBCONFIG_NO_CKPT_ON_CLOSE`. Connection close
  owns no checkpoint work because even a passive checkpoint may process an
  unbounded WAL without progress callbacks. Persistent WAL/SHM files remain
  normal cohort state; SQLite autocheckpointing or a future explicitly bounded,
  separately owned maintenance operation may compact them. Ordinary close,
  quarantine close, and fallback destruction perform native release only, so
  neither implicit SQLite close behavior nor a destructor can rewrite Main or
  unlink a possibly corrupt WAL/SHM cohort.
- Quarantine names derive from the operation identifier and never overwrite an
  existing artifact.
- The migration backup is mandatory and fail-closed. Existing source bytes are
  not modified if the backup, validation, flush, or publication step fails.

## Residual boundary

As recorded for P06 atomic publication, approved user-mode primitives cannot
eliminate every instruction-level check-to-use interval against arbitrary
uncooperative code already running under the same user token. P07 rejects
pre-existing and deterministically injected hard links, pins the main-file and
directory identities, validates sidecars at operation boundaries, and makes no
stronger containment claim. System-level mitigation remains assigned to the
installer/service hardening phase P22.

Native characterization tests must prove that the retained leases coexist with
the inbox VFS's WAL and `xShmUnmap` behavior in multiple processes. If that
characterization fails, P07 must stop at the exact blocker; ordinary pathname
open is not an accepted fallback.

## Evidence basis

- `.forge-codex/instructions/architecture/SECURITY.md`
- `.forge-codex/instructions/architecture/PERSISTENCE_AND_MIGRATION.md`
- `.forge-codex/state/decisions/P06-006-handle-relative-atomic-publish.md`
- `.forge-codex/state/decisions/P06-009-same-token-hard-link-publication-boundary.md`
- Corrected source safeguard inventory in
  `.forge-codex/state/baseline/p02-persistence-data-inventory.json`
