# P06-006: Handle-Relative Atomic Publish

Status: Accepted

Date: 2026-08-25

## Context

P06-002 selected `ReplaceFileW` for an existing target and `MoveFileExW` for an
absent target. Both calls resolve path strings at the commit boundary. A strong
handle that denies write and delete sharing prevents target tampering, but it is
incompatible with `ReplaceFileW`, which must reopen the target for deletion.
Releasing the verified target and parent handles before a path-based commit
creates a final time-of-check/time-of-use window in which a hostile same-user
process can rename or substitute the target or a parent.

The governing security architecture requires authority to be derived through
opened handles and requires reparse-point and normalization escapes to be
rejected. That higher-precedence requirement supersedes the earlier choice of a
path-based native primitive.

Windows exposes extended rename semantics through native
`NtSetInformationFile(FileRenameInformationEx)`. A P06 compatibility matrix on
the builder host established three details that the higher-level contract alone
did not make safe to infer:

- `SetFileInformationByHandle(FileRenameInfoEx)` rejects a non-null
  `RootDirectory` with `ERROR_INVALID_PARAMETER` on this supported host;
- native `FileRenameInformationEx` with a non-null `RootDirectory` internally
  opens that directory for write and conflicts with a parent retained with read
  sharing only; and
- replacing an open target requires that target handle to share delete, even
  when `FILE_RENAME_POSIX_SEMANTICS` is present.

The documented native same-directory form instead supplies a simple name with
`RootDirectory == nullptr`. The kernel resolves that name in the already-open
source file's current parent; no path or current-working-directory lookup is
performed. With the parent retained read-shared, the old target retained with
read-and-delete sharing, and the staged source retained with no sharing, this
form successfully leaves the old handle on old bytes while new opens observe
the newly published object.

## Decision

- This record supersedes only the native commit and ambiguous-reconciliation
  decisions in P06-002. The remaining P06-002 decisions continue to apply.
- `WindowsAtomicFileStore` is an app-owned, bounded byte-record store. It does
  not promise arbitrary Windows filesystem-object metadata preservation.
- Every existing parent is retained without write or delete sharing. The final
  parent is used as the root for every staging and publish operation.
- Reads retain the same complete parent-anchor chain and open the validated
  one-component leaf with `NtCreateFile.RootDirectory` through the shared
  relative-open wrapper. No authorized read reopens the full path after
  validation.
- Case-sensitive directories are outside the app-data contract. Every opened
  directory in the authority ancestry is queried with `FileCaseSensitiveInfo`,
  and `FILE_CS_FLAG_CASE_SENSITIVE_DIR` fails closed before child access or
  mutation. This keeps case-insensitive canonical comparisons and relative
  opens semantically consistent.
- An existing target and existing `.bak` are opened relative to that parent and
  retained with read-and-delete sharing from verification through publication.
  This denies content writers and keeps the original objects and bytes stable;
  their directory entries may move, which is safe because publication replaces
  the destination entry inside the staged handle's already-bound parent rather
  than resolving the prior leaf object.
- Random temporary files are created relative to the retained parent with
  `FILE_CREATE`, delete access, write-through behavior, and no sharing. The same
  handle owns writing, flushing, cleanup, and publication. No sharing denies
  ordinary content, rename, and delete opens, but it is not treated as a
  hard-link exclusion: NTFS permits `CreateHardLinkW` against this handle.
- `.forge-tmp-` followed by exactly 32 hexadecimal characters and `.tmp` is a
  reserved record-name shape (case-insensitively) and cannot be used as a
  public atomic target. Before new staging, a bounded native class-60 directory
  query filters this namespace beneath the retained parent. Exact lowercase
  Forge names are opened relative with no sharing, checked against the
  enumerated file ID, and disposition-deleted by that same handle only when
  they are ordinary, single-link, non-delete-pending objects. Live
  share-conflicting stages are retained and count against a 32-object admission
  bound. Enumeration and cleanup are capped at 64 examined names and 32
  deletions per operation; inability to establish the bound fails before
  another stage is created.
- Before any staging mutation that may replace an existing name, the volume
  must advertise `FILE_SUPPORTS_POSIX_UNLINK_RENAME`. Absence of that capability
  fails closed without changing the target or backup.
- Publication uses the private `NtSetInformationFile` boundary with native
  information class 65 (`FileRenameInformationEx`), a validated one-component
  destination, and `RootDirectory == nullptr`, selecting the documented
  same-directory rename form for the already-open staged source. Replacement
  uses exactly `FILE_RENAME_FLAG_REPLACE_IF_EXISTS |
  FILE_RENAME_FLAG_POSIX_SEMANTICS`. An absent destination is published without
  replace semantics, so a racing creator causes a closed failure.
- The staged handle is revalidated for type, single-link state, delete-pending
  state, extended attributes, and stable file identity after the deterministic
  prepublish hook and immediately before native class-65 publication. A hard
  link or other supported-state change detected there fails before the
  linearization point. P06-009 records the remaining same-token instruction
  window and its deferred architectural treatment.
- When backup retention is requested, the old target is copied from its pinned
  handle into a bounded same-directory backup temporary, flushed, and published
  over `.bak` before the new target is published. A failure after backup publish
  but before target publish therefore leaves the old target and an equivalent
  recovery copy.
- Existing-target DACL semantics must be preserved on staged target and backup
  objects, or the operation fails before publication. Owner SID, primary-group
  SID, and the mandatory-integrity label are captured without requesting audit
  SACL access; fresh staged objects must already be semantically equivalent,
  because no privilege-escalating owner or label rewrite is attempted. The
  creation-time policy is explicit and tested. Named streams beyond the unnamed
  data stream, file-level EFS encryption, file-level compression, NTFS extended
  attributes/`FILE_NEED_EA`, multiple hard links, and delete-pending objects are
  rejected before backup copies or publication. Authorized default-stream reads
  reject multiple hard links, delete-pending objects, reparse points, and EAs;
  compression/EFS and full replacement-metadata checks apply only to mutation,
  where metadata could otherwise be silently discarded.
- New records explicitly transition from `FILE_ATTRIBUTE_TEMPORARY` to
  `FILE_ATTRIBUTE_NORMAL` and verify that durable state before publication.
  Replacements and backups preserve only `FILE_ATTRIBUTE_NORMAL` or
  `FILE_ATTRIBUTE_ARCHIVE`; every other file attribute is rejected rather than
  silently changed. Published targets and recovery backups therefore never
  retain the staging-only temporary attribute.
- `SetFileInformationByHandle(FileRenameInfo/FileRenameInfoEx)`, `ReplaceFileW`,
  `MoveFileExW`, anchor release, and name-based cleanup are prohibited as commit
  or cleanup mechanisms. Handle-bound `FileBasicInfo` and
  `FileDispositionInfo` remain permitted for supported metadata and exact-handle
  cleanup. There is no ambiguous path-based recovery branch; a successful
  handle-relative native class-65 rename is the linearization point.
- Cancellation or deadline expiry is honored before the linearization point.
  Cancellation observed after successful target publication returns success.

## Consequences

The verified parent and original leaf objects stay pinned through the commit.
The target and backup names can be changed by a same-user process with a
pre-opened delete-capable child handle, but such a change cannot redirect the
publish: the kernel replaces only the named entry in the staged handle's current
parent. A substituted leaf reparse point is replaced as an entry and is never
traversed. The no-share staged handle prevents content, rename, and deletion
opens but does not prevent NTFS hard-link creation. The final staged-state check
rejects links already visible at that boundary; P06-009 records why approved
user-mode primitives cannot make the link-count predicate and rename one kernel
transaction. Existing readers that opened with delete sharing may continue reading
the old object, while subsequent opens observe the complete new object. A reader
that withheld delete sharing causes a retryable conflict; the old target remains
unchanged and every staged file is disposition-cleaned.

The backup is a logical byte-for-byte recovery copy, not the original filesystem
object. Object IDs, short names, and other metadata outside the app-owned byte
record contract are not preserved. Unsupported metadata is rejected explicitly
where silent loss could affect security or user expectations.

An abnormal process termination can bypass RAII cleanup, but it cannot cause
unbounded self-amplifying residue: the next write performs the bounded,
handle-relative recovery pass before creating another stage. A currently live
exclusive stage is never age- or PID-deleted. Too many live, ambiguous, or
malformed reserved-name objects fail admission rather than trigger traversal or
unbounded work. The native restart regression force-terminates a child after it
retains production-shaped pre-flush and post-flush stages, then proves that a
fresh engine reclaims both closed names while preserving the exact identity of
a separately retained live stage. This process-death evidence does not emulate
machine power loss or storage-controller cache loss.

Volumes without POSIX unlink/rename support cannot replace existing atomic
records. This is an intentional fail-closed compatibility boundary for the
Windows 11 application-data store.

## Rejected alternatives

- Release handles immediately before `ReplaceFileW`: rejected because the final
  target and parent can be substituted during the unprotected interval.
- Retain a target handle with delete sharing and then publish through a path:
  rejected because delete sharing permits a name swap after the final check and
  a path-based commit could follow the substituted namespace. The accepted
  same-directory source-handle rename does not resolve that namespace.
- Retain a target handle without delete sharing: rejected after direct runtime
  evidence because Windows returns `STATUS_SHARING_VIOLATION` even with POSIX
  replacement semantics, making publication impossible.
- `CreateFile2`/`CreateFile3` full-path staging with a later authority check:
  rejected because unauthorized bytes may already have been created outside the
  trusted root.
- Transactional NTFS: rejected because it is deprecated and not a dependable
  product contract.
- Source-file batch/RH/RWH oplocks: rejected after native evidence showed that
  creating a fresh hard-link alias succeeds without breaking the source oplock.
- POSIX-unlinking the staging name and publishing the surviving handle: rejected
  because the class-11 probe returned `STATUS_ACCESS_DENIED` for
  `FileLinkInformation`, the class-65 probe returned `ERROR_ACCESS_DENIED` for
  `FileRenameInformationEx`, and the follow-up class-72 probe returned
  `STATUS_ACCESS_DENIED` for `FileLinkInformationEx` with both flags zero and
  `FILE_LINK_FLAG_POSIX_SEMANTICS`.
- Silently dropping ACLs, EFS, compression, or named streams: rejected because
  lower metadata fidelity must not weaken security or conceal data loss.

## Evidence

- `.forge-codex/instructions/architecture/SECURITY.md`
- `.forge-codex/instructions/architecture/PERSISTENCE_AND_MIGRATION.md`
- Microsoft `FILE_RENAME_INFORMATION` and `NtSetInformationFile` contracts
- Microsoft `FileRenameInformationEx` protocol contract
- Microsoft `GetVolumeInformation` filesystem capability contract
- Microsoft `ReplaceFileW` metadata-preservation contract
- `.forge-codex/state/probes/relative-posix-publish-matrix.cpp`
- `.forge-codex/state/probes/p06-posix-unlinked-link-ex.cpp`
  (SHA-256 `6E7978B22D3CA1D0422C10E3CB461A22D265E5A05165C6AB4EC5A4ACFAAA399E`)
- `.forge-codex/state/probes/p06-posix-unlinked-link-ex.exe`
  (SHA-256 `AB579CC420B619F009B0274648F99707D61FFA70E727D7E4AEAA7263FAADCDA6`)
- Build record `.forge-codex/state/commands/20260826T010656506Z-af022f23.json`
  (exit 0; SHA-256
  `E9CEBB28F57A2C613B89804174EF2C9585C5E88CB3EDF468231F3EB5AEFA6439`)
- Run record `.forge-codex/state/commands/20260826T010700102Z-b88fffe8.json`
  (exit 0; SHA-256
  `51EDE51A9343875344FA492CB71F1B709203CAE74633C12A91684C25273A604F`)
- `tests/Infrastructure/StorageWindowsTests.cpp`
- `storage.atomic.crash-recovery-child`
- `storage.atomic.crash-restart-recovery`
- Exact class-72 result: flags zero and `FILE_LINK_FLAG_POSIX_SEMANTICS`
  each returned `link_status=0xC0000022`; both destination checks remained
  false before and after close, and clearing disposition returned error 5. The
  captured stdout SHA-256 is
  `3B69A8D53C0305D93B0A28ACE79E8A3419132BA932156E88A00ACCE3073BF4F3`.
