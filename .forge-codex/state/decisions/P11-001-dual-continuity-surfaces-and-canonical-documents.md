# P11-001: Dual Continuity Surfaces and Canonical Documents

Status: Accepted

Date: 2026-08-26

## Context

The pinned macOS product exposes two continuity families with different data
models and durability scopes. `session_checkpoint`, `session_handoff`,
`context_get`, and `context_list` use schema-version-1 context packets stored in
the central database and projected to `memory/handoffs`, `LATEST`, and
`current-task.md`. The seven `continuity.*` operations use schema `1.0`
lifecycle handoffs and project-local rollover state. Collapsing either model
into the other would lose source behavior and complicate migration.

Application code must not parse JSON. Exact canonical bytes and SHA-256 values
are nevertheless durable compatibility data and cannot be reconstructed from a
caller-supplied field summary.

## Decision

- Keep two typed aggregates:
  - legacy context continuity, authoritative in central `context_handoffs`;
  - project lifecycle continuity, authoritative in each project's
    `memory.sqlite`.
- Add distinct domain models and repository/service contracts. The lifecycle
  model remains in `ContinuityModels`; the source-compatible packet model is
  `LegacyContinuityModels`.
- Add an injected `IContinuityDocumentCodec`. Its Windows implementation is
  private Infrastructure code using the approved nlohmann/json package. It
  returns typed values plus canonical UTF-8 bytes and never exposes a JSON
  implementation to Domain or Application.
- Canonical lifecycle documents use recursively sorted keys, compact UTF-8,
  schema version `1.0`, explicit nulls where the source does, and exact value
  types. Hashing replaces `integrity.content_sha256` with 64 ASCII zeroes and
  hashes those exact bytes.
- Decoding rejects duplicate keys at every object depth, malformed UTF-8,
  embedded NUL, excess depth, excess bytes, wrong types, unsupported schema,
  and a mismatched content hash.
- Preserve the checked-in macOS project-v1 handoff as the primary byte/hash
  oracle. Its expected content digest is
  `fb34ad2e12b31c79de72ef2fedc294232e21e179357c44bb241227245382586f`.
- Legacy readable files are projections only. SQLite remains authoritative;
  projection failures produce a repair warning and startup repair reconstructs
  every packet plus the latest pointers.
- P14 owns MCP schemas, aliases, JSON-RPC parsing, envelopes, and wire error
  mapping. P11 exposes typed requests and outcomes only.

## Consequences

The two source-compatible surfaces can evolve without schema conflation.
Canonical JSON remains below the Application layer, while exact persisted bytes
and hashes are testable through a narrow injected contract. P11 must retain
null-versus-absent behavior, empty next-action command strings, Unicode and
escaping golden cases, and bounded collections.

## Evidence basis

- `.forge-inputs/macos/Forge-Conductor-MacOS-main/Sources/ForgeConductorCore/Domain/HandoffPacket.swift`
- `.forge-inputs/macos/Forge-Conductor-MacOS-main/Sources/ForgeConductorCore/Domain/ContinuityModels.swift`
- `.forge-inputs/macos/Forge-Conductor-MacOS-main/Sources/ForgeConductorCore/Application/ContextContinuityService.swift`
- `.forge-inputs/macos/Forge-Conductor-MacOS-main/Tests/ForgeConductorTests/ContinuityTests.swift`
- `tests/Persistence/Fixtures/project-v1.sql`
