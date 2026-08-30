# P16-029: Manager Operational Adapter and Compatible Diagnostic Anchoring

Status: Accepted

Date: 2026-08-30

## Context

The Manager dashboard application boundary requires one concrete source for an
atomic operational snapshot, bounded diagnostic lines, and administrative
session close. The implementation must remain outside the neutral Manager host
layer because it composes application, persistence, and Windows infrastructure
types.

Production stores the central database directly under the application data
root and diagnostics under its `logs` child. The database retains a
write-capable handle on the data root. The diagnostic tail reader previously
reopened every ancestor with read-only sharing, so a dashboard diagnostic read
could wait until its deadline while the central database was open.

## Decision

- `ManagerDashboardOperationalDataSource` is an outer Windows-composition
  adapter. It borrows the operational repository, session repository,
  diagnostic reader, and clock; it owns no independent shutdown transition.
- Operational snapshots delegate to the repository's single-statement
  projection and map presence records one for one. Repository errors retain
  their typed classifications; allocation or mapping exceptions fail safely as
  internal errors.
- Diagnostic reads preserve the reader's caller-supplied line, per-line, and
  aggregate bounds. Administrative close delegates the exact session ID and
  summary with the injected UTC clock.
- The diagnostic tail reader performs an `OPEN_EXISTING` component walk. Every
  component is verified and reverified for type, reparse state, case policy,
  canonical identity, cancellation, and deadline. Non-final ancestors allow
  write sharing so the database's data-root lease can coexist, while every
  retained handle denies delete sharing and the final diagnostics root also
  denies write sharing.
- The actual sanctioned packaged-path identity of the final root becomes the
  authority for handle-relative lock and master-file identity checks. A missing
  diagnostics root is an empty read-only result and is never created by the
  tail reader.

## Consequences

The Manager application can obtain one truthful operational projection, read
bounded diagnostics while the central database is live, and close a concrete
session without introducing persistence or filesystem operations into the
transport or view layer. The final diagnostics namespace remains protected
during the read, and intermediate junctions or case-sensitive ancestors still
fail closed.

The composition root must retain all borrowed dependencies until dashboard
admission has stopped and in-flight calls have drained. This checkpoint does
not provide the doctor service, telemetry-unavailable projection, production
Manager executable composition, real-process evidence, live Edge behavior,
native UI automation, or the authoritative G16 gate.

## Evidence basis

- `src/Composition/Windows/ManagerDashboardOperationalDataSource.h`
- `src/Composition/Windows/ManagerDashboardOperationalDataSource.cpp`
- `src/Infrastructure/Windows/WindowsDiagnosticLogTailReader.cpp`
- `tests/Composition/Windows/ManagerDashboardOperationalDataSourceTests.cpp`
- `tests/Infrastructure/WindowsDiagnosticLogTailReaderTests.cpp`

