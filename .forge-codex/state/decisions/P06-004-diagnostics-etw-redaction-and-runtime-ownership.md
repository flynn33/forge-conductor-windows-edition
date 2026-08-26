# P06-004: Diagnostics, ETW Redaction, and Runtime Ownership

Status: Accepted

Date: 2026-08-25

## Context

Diagnostics must remain useful without retaining secrets or creating unbounded memory. Runtime leak evidence also requires fixed owner counters that return exactly to zero after shutdown flows.

## Decision

- Every event is validated and redacted before it enters memory, JSONL, export, or ETW-derived data.
- Private keys, credential canaries, paths, prompts, queries, commands, and arbitrary field values never enter ETW.
- ETW registration is application-owned RAII through `EventRegister`/`EventUnregister`; `EventWriteTransfer` emits fixed numeric severity/category/process/timestamp descriptors only.
- ETW registration/write failure degrades that channel when bounded redacted file logging remains healthy; it does not recursively log arbitrary exception text.
- JSONL rotation occurs before an append would exceed the active 4/8/10 MiB profile cap. The active file counts toward the exact 5/8/10 file total.
- The in-memory ring is newest-last and bounded by both 4,000 records and the active single-file encoded-byte cap.
- Export produces deterministic JSON and Markdown artifacts with independent SHA-256 checksums.
- Runtime ownership uses a move-only `RuntimeOwnershipLease` issued by `IRuntimeDiagnostics`. Release callbacks capture only weak control state, so a lease never prolongs the registry.
- Fixed counters cover operations, callbacks, threads, repositories, telemetry snapshots, timers, child processes, process readers, and databases. Telemetry pending capacity is exactly one; process and reader capacities are 64 and 128.
- Acquisition after cancellation, deadline, or shutdown fails. Move/destruction decrements exactly once and never underflows.

## Consequences

Diagnostics are bounded and privacy-safe across every retention channel. Shutdown tests can prove exact owner-level zero instead of relying only on aggregate process memory.

## Rejected alternatives

- A singleton runtime registry: rejected because it hides lifetime and prevents repeatable shutdown.
- Owning callbacks from leases: rejected because they can form cycles and keep the registry alive.
- Record-count-only retention: rejected because encoded memory remains unbounded.
- Arbitrary ETW strings after best-effort redaction: rejected because channel policy must remain structurally private.

## Evidence

- `.forge-inputs/macos/Forge-Conductor-MacOS-main/Sources/ForgeConductorCore/Infrastructure/DiagnosticLog.swift`
- `.forge-inputs/macos/Forge-Conductor-MacOS-main/Sources/ForgeConductorCore/Infrastructure/RuntimeObservability.swift`
- `.forge-codex/instructions/architecture/OOP_AND_OWNERSHIP.md`
- `.forge-codex/instructions/architecture/SECURITY.md`
