# P06-009: Same-Token Named-Stage Hard-Link Publication Boundary

Status: Accepted

Risk disposition: Accepted; architectural mitigation deferred to P22

Date: 2026-08-25

## Context

P06-006 publishes a bounded app-owned byte record from a random, handle-relative
temporary file. P06-008 rotates diagnostics through one of sixteen named
`.forge-diagnostics-rotation-<slot>.tmp` stages owned by
`PendingDiagnosticRotationFile`. Both staged handles deny all sharing, but an
NTFS compatibility probe using the atomic production access mask proved that a
second name can still be created for the object. Sharing modes protect later
opens that request read, write, or delete access; they do not implement a
link-count lock.

A source-file oplock does not close the gap. A fresh file was created with
`FILE_OPEN_REQUIRING_OPLOCK`, an asynchronous RWH oplock was granted, and a
second thread created a fresh alias successfully while the oplock event remained
unsignaled. This matches Microsoft's `IRP_MJ_SET_INFORMATION` contract: source
oplocks do not serialize creation of a fresh hard-link destination. The
preserved class-11/class-65 probe POSIX-unlinked the only staging name; NTFS then
rejected native `FileLinkInformation` with `STATUS_ACCESS_DENIED` and
`FileRenameInformationEx` with `ERROR_ACCESS_DENIED`. A separate class-72 probe
also rejected `FileLinkInformationEx` with both flags zero and
`FILE_LINK_FLAG_POSIX_SEMANTICS`, returning `STATUS_ACCESS_DENIED`. Byte-range
locks and sharing modes likewise provide no conditional rename-on-link-count
primitive.

An arbitrary uncooperative process running with the same token can also access
or hard-link a successfully published app-data record immediately after the
Forge operation returns. Preventing that actor from using the filesystem
directly requires a distinct security principal or a kernel enforcement layer;
it cannot be achieved by another user-mode check around class-65 rename.

## Decision

- Every opened atomic leaf is rejected unless it is regular, single-link,
  non-delete-pending, reparse-free, and free of unsupported EAs at the point its
  identity is captured.
- Publication repeats that complete staged-object validation after the
  deterministic prepublish hook and immediately before
  `NtSetInformationFile(FileRenameInformationEx)`. A hook-injected hard link is
  therefore detected and the target is not published.
- The unavoidable instruction-level interval between the last link-count query
  and native rename is outside the Forge service-authority boundary for an
  arbitrary process already holding the identical Windows token and direct
  filesystem rights. No claim of containment is made for that interval.
- A detected external alias is not traversed or deleted by Forge. The engine
  disposition-deletes only the exact reserved staging link owned by its handle;
  attempting to discover or remove aliases outside the retained parent would
  exceed authority.
- Diagnostic rotation checks `FileStandardInfo` (`NumberOfLinks == 1` and
  `DeletePending == FALSE`), non-reparse/type state, and final-path identity on
  the exact staged handle immediately before native class-65/null-root,
  one-component, flags-zero publication. It rechecks the canonical destination
  before disposing the source. A detected prepublish link fails
  `path_outside_authority`, disposition-deletes only the known stage name through
  its exact handle, publishes no canonical archive, and preserves the source.
- P22 must reconsider process isolation behind a broker identity or a narrowly
  scoped filesystem minifilter if the product threat model later requires
  exclusion of hostile same-token filesystem operations. Deprecated TxF is not
  accepted as a production dependency.

## Consequences

Preexisting hard links and deterministic links injected before the final check
fail closed before target linearization. Atomic replacement leaves the old
target unchanged; diagnostic rotation preserves its source and publishes no
canonical archive. The retained anchors, relative creation, bounded cleanup,
and same-directory class-65 commits still contain path, reparse, rename, delete,
and content-write races within their stated contracts.

This record is a material known risk, not a proof that the final instruction
window is safe. Validation and release evidence must describe it explicitly.

## Evidence

- Microsoft `IRP_MJ_SET_INFORMATION` oplock-state contract
- Microsoft `FILE_LINK_INFORMATION` contract
- Microsoft `FILE_LINK_INFORMATION_EX` contract
- Microsoft `FILE_DISPOSITION_INFORMATION_EX` contract
- `.forge-codex/state/probes/p06-source-oplock-hardlink.cpp`
  (SHA-256 `7078B221C84BC9A06824E28EFB0C7DB2EF5A7894EDCF3FF124CEE0821341304A`)
- `.forge-codex/state/probes/p06-source-oplock-hardlink.exe`
  (SHA-256 `C25753C46288C0CD59067B27E25843921CAFEB5EACCB3CB837A58644B7A575DD`)
- Build record `.forge-codex/state/commands/20260826T005107667Z-cf49e950.json`
  (exit 0; SHA-256
  `B6BAF7398DFC60C861B96E88E1DD838D9757A133096CA2789F97BFF5F94F2520`)
- Run record `.forge-codex/state/commands/20260826T005120986Z-83e45bff.json`
  (exit 0; SHA-256
  `90E5EA5A9AE9AD83CD6382EBCE6CB06FFAE5EB7E0E2C12203B6C405ACF66AA1B`)
- Exact original run output: `link_wait=0 link_succeeded=1 oplock_wait=258`;
  `clear_error=5 rename_error=5`; and class-11
  `link_status=0xC0000022`. The production-access share-zero stage admitted a
  second link while the async RWH oplock remained unsignaled. The
  POSIX-unlinked surviving handle could not clear deletion, publish through
  class-65 rename, or publish through class-11 link.
- `.forge-codex/state/probes/p06-posix-unlinked-link-ex.cpp`
  (SHA-256 `6E7978B22D3CA1D0422C10E3CB461A22D265E5A05165C6AB4EC5A4ACFAAA399E`)
- `.forge-codex/state/probes/p06-posix-unlinked-link-ex.exe`
  (SHA-256 `AB579CC420B619F009B0274648F99707D61FFA70E727D7E4AEAA7263FAADCDA6`)
- Class-72 build record
  `.forge-codex/state/commands/20260826T010656506Z-af022f23.json`
  (exit 0; SHA-256
  `E9CEBB28F57A2C613B89804174EF2C9585C5E88CB3EDF468231F3EB5AEFA6439`)
- Class-72 run record
  `.forge-codex/state/commands/20260826T010700102Z-b88fffe8.json`
  (exit 0; SHA-256
  `51EDE51A9343875344FA492CB71F1B709203CAE74633C12A91684C25273A604F`)
- Exact class-72 output: flags zero and `FILE_LINK_FLAG_POSIX_SEMANTICS`
  each returned `link_status=0xC0000022`; `destination_before_close=0`,
  `clear_succeeded=0 clear_error=5`, and `destination_after_close=0` for both
  attempts. `RESULT class72_flags_zero_rejected=1 class72_posix_rejected=1`.
  Captured stdout SHA-256:
  `3B69A8D53C0305D93B0A28ACE79E8A3419132BA932156E88A00ACCE3073BF4F3`.
- `storage.atomic.final-stage-hard-link-rejection`
- `diagnostics.rotation-stage-hard-link-rejection`
