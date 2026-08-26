# P06-008: Handle-Relative Diagnostic Rotation

Status: Accepted

Date: 2026-08-25

## Context

The diagnostic sink retains the final log directory as a traverse/list/read-
attributes handle with read sharing only. That anchor rejects competing
write/delete directory opens and in-place reparse changes for the whole log
transaction. Direct host probes established that both Win32 rename information
and native `NtSetInformationFile` with a non-null destination directory require
an internal write-capable directory open and fail with a sharing violation under
this anchor.

Log rotation is archival retention, not the atomic publication boundary used by
configuration and export. Releasing or weakening the final directory anchor to
make rename succeed would reopen the higher-precedence reparse and namespace
race that P06 must close.

## Decision

- Keep the final diagnostic root anchored with traverse/list/read-attributes
  access and `FILE_SHARE_READ` for the entire sink transaction. Do not release or
  reopen it for rotation.
- Open, create, append, lock, rotate, and delete diagnostic children only through
  validated one-component names relative to that retained handle.
- Rotate a log through a bounded 64 KiB copy into one of sixteen reserved,
  entropy-selected same-directory staging names. Staging is no-share and
  `CREATE_NEW`; cancellation, deadline, and shutdown are checked before each
  chunk, and the complete staging handle is flushed before publication.
- Publish the exact staged handle with the Windows 11 native class-65,
  null-`RootDirectory`, one-component same-directory rename primitive already
  validated for atomic storage. Each destination is deleted or vacated before
  its predecessor is moved, so publication uses flags `0` (`CREATE_NEW`
  semantics); a destination collision fails closed instead of unlinking or
  replacing an unexpected leaf. After all fallible publication preparation,
  immediately revalidate the exact staged handle's type, canonical name, reparse
  state, delete-pending state, and single-link identity before the native call.
  Revalidate that handle at its final name and the retained source handle before
  source disposition.
- Any handled failure disposition-deletes the known staging or newly published
  name through the exact handle and leaves the source intact. It cannot discover
  or delete an out-of-band hard-link name. A crash during copy can leave only a
  noncanonical reserved staging name; the next locked transaction removes all
  bounded staging slots before log access. A crash after publication but before
  source disposition can retain duplicate complete redacted records.
- Every diagnostic file handle is rejected before read, write, or deletion when
  `FileStandardInfo` reports more than one hard link or delete-pending state.
  Every retained directory anchor is rejected when
  `FILE_CS_FLAG_CASE_SENSITIVE_DIR` is present because the relative wrapper uses
  `OBJ_CASE_INSENSITIVE`.
- Diagnostic export continues to use `WindowsAtomicFileStore`; this decision
  applies only to internal JSONL rotation.
- `FILE_SHARE_NONE` alone does not prevent a new hard link. The retained parent
  blocks ordinary path-based `CreateHardLinkW`, but a same-token process that
  retained a compatible parent handle before strong anchoring can reopen the
  stage for attributes and issue native class-11 same-directory link metadata.
  The immediate handle check narrows but cannot eliminate the same-token
  check-to-native-call interval. P06-009 records that shared atomic and
  diagnostics threat boundary; this ADR makes no kernel-containment claim for
  arbitrary out-of-band code running under the same Windows token.
- The deterministic prepublication observer is a private Detail-only test seam.
  The supported `WindowsDiagnosticSink` API exposes only its normal production
  constructor; a private final `WindowsDiagnosticSinkTestAccess` factory alone
  can call the observer-taking constructor. `Impl` owns the injected observer,
  and `PendingDiagnosticRotationFile` retains its exact staged handle and cleanup
  responsibility across the callback and final staged-file validation.

## Consequences

Rotation is a bounded archival copy followed by an atomic same-directory name
publication, with a documented duplicate-on-crash interval after publication.
The stronger directory authority boundary remains live throughout every
filesystem mutation. Partial copies are never published under canonical archive
names, and crash residue is confined to a fixed, startup-cleaned set of staging
names. The abrupt-process-death regression compares the retained source and the
recovered archive byte-for-byte with the pre-crash source. A hard link present
at the prepublication check fails closed without a canonical archive and
preserves the source; the residual same-token micro-race remains an explicitly
accepted risk under P06-009.

## Rejected alternatives

- Release the strong anchor immediately before `MoveFileExW`: rejected because a
  same-user process can substitute the directory during that interval.
- Retain a write-sharing directory handle solely for rename: rejected because it
  permits competing write-capable directory handles during the transaction and
  is unnecessary for an archival log.
- Ignore rename failure and continue appending: rejected because it violates the
  active-file byte cap.

## Evidence

- `.forge-codex/instructions/architecture/SECURITY.md`
- `.forge-codex/instructions/plans/resource-budgets.json`
- `.forge-codex/state/decisions/P06-004-diagnostics-etw-redaction-and-runtime-ownership.md`
- `.forge-codex/state/decisions/P06-009-same-token-hard-link-publication-boundary.md`
- `.forge-codex/state/probes/relative-posix-publish-matrix.cpp`
- `tests/Infrastructure/WindowsDiagnosticSinkTests.cpp`
