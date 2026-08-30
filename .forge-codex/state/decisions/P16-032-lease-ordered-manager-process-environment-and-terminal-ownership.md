# P16-032: Lease-Ordered Manager Process Environment and Terminal Ownership

Status: Accepted

Date: 2026-08-30

## Context

The production Manager must resolve its per-user roots, executable identities,
and resource profile without mutating process state before it proves exclusive
per-user ownership. A path string alone is not enough evidence that the image
inspected before lease acquisition is the same regular file used afterward.
The composition root also needs one terminal value whose lifetime keeps the
instance lease later than every process service.

## Decision

- `ManagerProcessEnvironment::inspect` is read-only. It resolves the canonical
  application data, configuration, diagnostics, exports, and projects roots;
  the running `ForgeConductor.Manager.exe`; its exact sibling
  `forge-conductor.exe`; physical memory; the selected resource profile; and
  its immutable budgets.
- The native platform probe opens executable paths for attributes and stable
  file identity, rejects directory and reparse-point images, retains the
  canonical DOS path plus volume serial and 128-bit file identifier, and
  releases every native handle before returning.
- Production uses the current-user Windows application root with no explicit
  root and no environment override. Explicit roots and opt-in environment
  behavior remain injectable only for existing controlled hosts and tests.
- The caller first acquires `WindowsManagerInstanceLease`, then transfers it to
  `prepareAfterLease`. Preparation re-inspects every root and executable
  identity and rejects any mismatch before creating a directory.
- Preparation creates or validates exactly five regular non-reparse
  directories in parent-before-child order. A failure releases the transferred
  lease; success returns a move-only `PreparedManagerProcessEnvironment` that
  owns the exact immutable snapshot beside the live lease.
- The production composition root must declare that prepared environment as
  its terminal owner and destroy it only after ingress, workers, runtime,
  repositories, and other borrowed dependencies are quiescent.
- The real-process fixture runs under the exact `ForgeConductor.Manager.exe`
  leaf with the built `forge-conductor.exe` staged as its sibling. It exercises
  the native image identities, physical-memory profile, read-only inspection,
  lease transfer and release, exact directory set, and non-reparse cleanup on
  this Windows machine rather than substituting an injected platform probe.

## Consequences

No application-owned directory mutation can precede exclusive Manager
ownership, executable replacement between discovery and startup is detected,
and the lease has an explicit RAII owner rather than a process-global or
incidental local variable. The production composition root and executable are
still required to enforce the final destruction order in a real process.

This checkpoint does not complete P16 or invoke the authoritative G16 gate.

## Evidence basis

- `src/Composition/Windows/ManagerProcessEnvironment.h`
- `src/Composition/Windows/ManagerProcessEnvironment.cpp`
- `tests/Composition/Windows/ManagerProcessEnvironmentTests.cpp`
- `tests/Composition/Windows/ManagerProcessEnvironmentRealTests.cpp`
- `include/ForgeConductor/Infrastructure/Windows/WindowsManagerInstanceLease.h`
- `.forge-codex/state/decisions/P16-001-single-owner-manager-and-current-user-control-plane.md`
