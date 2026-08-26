# P11-003: Side-Effect Intents, Idempotent Recovery, and Host Boundaries

Status: Accepted

Date: 2026-08-26

## Context

Rollover includes non-transactional host effects: successor creation,
bootstrap delivery, acknowledgement, and predecessor sealing. A process can
stop between any effect and its durable result. The macOS coordinator records
intent first and queries by idempotency key before creation, but its test crash
exceptions do not prove process-loss behavior and its retry record does not
persist the exact resume phase.

## Decision

- `ContinuityCoordinator final` depends only on abstract project repository,
  canonical document codec, host adapter, clock, UUID generator, and diagnostics
  contracts. Dependencies outlive it; there is no registry, singleton, detached
  task, or background thread in P11.
- The durable progression is:
  `idle -> checkpoint_preparing -> checkpoint_persisted -> successor_creating
  -> successor_created -> bootstrap_sending -> acknowledged
  -> predecessor_sealing -> completed`.
- Before every host effect, commit its intent. Never hold a mutex or SQLite
  transaction while invoking the adapter.
- Successor creation uses the durable idempotency key. When the adapter exposes
  key lookup, reconciliation queries before creating; otherwise creation is
  replayed only when the adapter explicitly declares idempotency. The returned
  session is validated against project, operation, predecessor, and idempotency
  bindings before persistence.
- Bootstrap receives the exact persisted canonical handoff bytes. An
  acknowledgement must match project, operation, handoff, predecessor,
  successor, adapter, and canonical SHA-256.
- Recoverable failures transition through `failed_recoverable` to `retry_wait`
  with bounded error text, retry time, and exact resume state. Retrying before
  `retry_at` performs no host call.
- Cancellation is `cancelling -> cancelled` and is durable. A cancellation that
  wins before commit rolls back; a successfully committed effect returns its
  committed result even if cancellation is observed immediately afterward.
- Explicit resume reconciles and validates the acknowledged successor before
  entering predecessor sealing or publishing the active-session pointer. A
  failed host reconciliation therefore leaves the acknowledged operation
  nonterminal and recoverable without mutating the pointer.
- P11 proves the full coordinator with a deterministic host fake and truthful
  capability-unavailable behavior that stops at a durable checkpoint. P12 owns
  the real native host adapter/plugin, automatic budget-triggered rollover,
  provider-specific behavior, and end-to-end successor activation.

## Consequences

Every recovery decision is derivable from durable state. Host effects may be
replayed safely without duplicate successors or distinct bootstrap effects.
P11 makes no claim that the native session host is already composed.

## Evidence basis

- `.forge-inputs/macos/Forge-Conductor-MacOS-main/Sources/ForgeConductorCore/Application/ContinuityCoordinator.swift`
- `.forge-inputs/macos/Forge-Conductor-MacOS-main/Tests/ForgeConductorTests/ContinuityCoordinatorTests.swift`
- `include/ForgeConductor/Domain/ContinuityModels.h`
- `include/ForgeConductor/Contracts/IContinuityCoordinator.h`
