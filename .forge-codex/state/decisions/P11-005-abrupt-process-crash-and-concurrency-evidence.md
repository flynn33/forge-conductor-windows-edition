# P11-005: Abrupt Process Crash and Concurrency Evidence

Status: Accepted

Date: 2026-08-26

## Context

Exception injection does not establish SQLite behavior after real process
termination. G11 requires lifecycle and crash recovery evidence, and the owner
has retained functionality while reducing each gate to one authoritative
build/test invocation.

## Decision

- Use a separate native `Continuity.ProcessFixture` child and terminate it with
  the Windows process API at deterministic repository/effect boundaries.
- Exercise ten boundaries: checkpoint-intent commit, checkpoint-persisted
  commit, successor-create intent, create effect before successor commit,
  successor commit, bootstrap intent, bootstrap effect before acknowledgement,
  acknowledgement commit, predecessor-sealing intent, and completed/pointer
  commit.
- After every restart prove one successor, one distinct bootstrap effect, exact
  acknowledgement, terminal operation, correct active-session pointer, intact
  ordered transition/checksum chain, and no orphaned handoff.
- Run two real processes against one legacy packet with disjoint explicit edits;
  prove the durable merged union and the correct latest projection.
- Repository tests also cover same-project CAS contention, multi-project
  isolation, real bounded `BEGIN IMMEDIATE` database contention, 64 repeated
  stable recovery reads, cancellation/deadline rollback, storage-full mapping,
  and continuity-only reset preserving all seeded project-memory content.
- G11 includes Domain/codec, legacy Application, legacy Windows repository,
  project Windows repository, coordinator, process fixture, and the modified
  project migration target. Retained earlier gates run static checks only.
- Under OWNER-002 there is one authoritative x64 Debug G11 build/test
  invocation. A Validator reviews the resulting command, output, hashes, and
  scope without rebuilding.

## Consequences

G11 evidence distinguishes durable crash recovery from in-process exception
handling and avoids duplicate full-gate rebuilds. Passing G11 remains a
continuity-slice claim, not a native-host, MCP-wire, installer, UI, or complete
alpha claim.

## Evidence basis

- `.forge-codex/state/decisions/OWNER-002-alpha-release-qualification-scope.md`
- `.forge-inputs/macos/Forge-Conductor-MacOS-main/Sources/ForgeConductorCore/Application/ContinuityCoordinator.swift`
- `.forge-inputs/macos/Forge-Conductor-MacOS-main/Tests/ForgeConductorTests/ContinuityCoordinatorTests.swift`
- `tests/Agents/AgentSessionProcessFixture.cpp`
