# P10-004: Abrupt-Crash and Concurrent-Ownership Evidence

Status: Accepted

Date: 2026-08-26

## Context

The source tests described as restart recovery create another application stack
over the same home after the first stack remains in ordinary language scope.
They prove durable reopening but not rollback or commit behavior when a process
dies without destructors, shutdown, or connection close. G10's crash/restart
acceptance and the product's autonomous recovery requirements need stronger
runtime evidence.

## Decision

- G10 includes a native helper process that uses the production Windows agent
  repository and can be terminated at explicit transaction checkpoints supplied
  through a test-only injected observer. Production open factories never read
  an environment variable or expose a runtime crash switch.
- Parent tests terminate the child with the Windows process API so no C++
  destructor, service shutdown, or graceful SQLite close runs. Every native
  handle in the harness has a scoped owner and every wait has a deadline.
- Pre-commit checkpoints for start, completion, and reattachment must reopen to
  the complete prior state. A post-commit/pre-reply checkpoint must reopen to
  the complete committed state. Every reopen runs `quick_check` before semantic
  assertions.
- Start evidence covers same-client supersede, new row, active projection, and
  run projection. Completion evidence covers report/summary, closed status,
  matching active-pointer removal, and retained run projection. Reattachment
  evidence covers expected-owner compare-and-swap, destination supersede, old
  pointer removal, and one new pointer.
- A concurrent two-owner transfer test uses independent process/connection
  attempts and proves exactly one winner, one typed conflict, one final owner,
  and one matching active pointer.
- Cancellation, expired deadline, busy database, close/shutdown, and repeated
  recovery are separately tested. A lower row count or successful reopen alone
  is not accepted as crash-consistency proof.
- Test checkpoints are a persistence test seam only. They do not alter schema,
  normal code flow, product configuration, installed files, or release behavior.

## Consequences

G10 evidence distinguishes real abrupt termination from object reconstruction
and proves both sides of the transaction boundary. Later continuity and
installer stress phases can reuse the same parent/helper pattern without
introducing product crash controls.

## Rejected alternatives

- Clean service reconstruction alone: rejected because it runs normal RAII
  cleanup and cannot prove rollback after process death.
- Killing at arbitrary wall-clock delays: rejected because the reached mutation
  point would be unknowable and evidence would be nondeterministic.
- A production environment-variable failpoint: rejected because hostile or
  accidental configuration could terminate an installed product.
- Directly editing a SQLite file in the parent: rejected because it would not
  exercise the production transaction implementation.

## Evidence basis

- `.forge-inputs/macos/Forge-Conductor-MacOS-main/Tests/ForgeConductorTests/CoreTests.swift`
- `.forge-inputs/macos/Forge-Conductor-MacOS-main/Tests/ForgeConductorTests/ContinuityTests.swift`
- `.forge-codex/instructions/docs/KNOWN_MACOS_DEFECTS_NOT_TO_PORT.md`
- `.forge-codex/instructions/specifications/DATABASE_CONTRACT.md`
- `.forge-codex/instructions/plans/gates.json` — G10

