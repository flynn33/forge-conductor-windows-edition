# P11-004: Legacy CAS, Projections, Reset, and Lifetimes

Status: Accepted

Date: 2026-08-26

## Context

The source legacy service performs a read/merge/write of context packets across
multiple MCP processes and serializes it with macOS locks. Windows requires an
explicit durable concurrency rule, deterministic cross-process ordering,
repairable projections, scoped reset, and documented shutdown ownership.

## Decision

- Legacy rows carry a monotonically allocated `write_sequence`. Insert requires
  absence; update uses the previously read sequence. Application reloads and
  merges only caller-explicit fields on conflict, retrying at most eight times.
  Exhaustion returns `conflict`.
- A single `BEGIN IMMEDIATE` central transaction allocates the sequence, writes
  the packet, and updates the `continuity/latest` and
  `continuity/resume_ready` pointer notes. Timestamp ties are ordered by durable
  sequence, never wall-clock coincidence.
- Preserve imported `packet_json`. Read canonical `payload_json` when present
  and fall back to `packet_json`; validate `content_sha256` when present. The
  first accepted Windows update writes canonical payload, hash, and the
  source-compatible packet representation.
- Snapshot only open sessions owned by the calling client, deduplicate by
  session id, and cap at 128. Continuing an explicit packet preserves prior
  snapshots only while the durable session remains open, replacing entries as
  ownership is reattached.
- Preserve the source 4,000-character narrative bound, generated/custom resume
  seed distinction, field aliases, fill-only automatic updates, and projection
  repair behavior.
- `WindowsLegacyContinuityRepository final` attaches to the composition-owned
  central database and closes only its own admission. The composition root
  drains services and attached repositories before closing the database.
- Project continuity reset requires exact typed confirmation and deletes only
  lifecycle handoffs, operations, transitions, and active-session pointers. It
  must not alter project memory records, tags, links, artifacts, project
  metadata/aliases, or ordinary journal rows.
- Central legacy packet cleanup is a separate idempotent transaction because
  the central and project databases do not share one transaction boundary. The
  reset report identifies which scopes committed so repetition converges.

## Consequences

Disjoint concurrent edits survive, projection failures cannot erase the
authoritative write, and reset remains continuity-only. Cross-database reset is
explicitly convergent rather than falsely described as atomic.

## Evidence basis

- `.forge-inputs/macos/Forge-Conductor-MacOS-main/Sources/ForgeConductorCore/Application/ContextContinuityService.swift`
- `.forge-inputs/macos/Forge-Conductor-MacOS-main/Sources/ForgeConductorCore/Domain/HandoffPacket.swift`
- `.forge-inputs/macos/Forge-Conductor-MacOS-main/Tests/ForgeConductorTests/ContinuityTests.swift`
- `include/ForgeConductor/Application/ProjectMemoryRepositoryCache.h`
- `include/ForgeConductor/Persistence/Windows/WindowsCentralDatabase.h`
