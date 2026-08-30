# P16-035: Capacity-One Manager Maintenance Reconciliation

Status: Accepted

Date: 2026-08-30

## Context

The Manager must own stale-session pruning, durable continuity recovery, and
LM Studio drift reconciliation without an unbounded task queue, overlapping
passes, or dependency-owned timers. The transition watchdog already has a
separate one-thread lifecycle, so the process host needs a bounded composite
that preserves each worker's cancellation and exact join contract.

## Decision

- `ManagerMaintenanceService` implements one synchronous, capacity-one pass.
  It owns no thread, timer, callback, retry loop, or service locator and borrows
  every dependency through its constructor.
- A pass checks cancellation and deadline before admission and between stages,
  prunes stale agent sessions, recovers all incomplete continuity operations,
  and inspects LM Studio deployment drift. A partial continuity report is a
  retryable typed failure.
- Independent later stages still run after an earlier dependency failure. The
  first failure is returned after all still-live stages complete, so one
  unhealthy subsystem cannot indefinitely suppress another maintenance duty.
- Present LM Studio drift authorizes the exact
  `install-lmstudio-plugin` Write request with canonical
  `{"preserve_foreign_entries":true}` arguments and protocol `2025-11-25`,
  then requires a successful two-plugin deployment result. An absent optional
  installation is not deployed or activated.
- The read capability is exact Read-only, non-shell authority. The distinct
  deployment capability shares its dedicated maintenance project and caller,
  has a nonzero generation, grants Read, Write, Create, Delete, and Execute,
  and enables the narrowly bounded shell behavior required by the existing
  pre- and post-deployment native serve verifier. This is functional
  compatibility with the P15 deployment contract, not GUI automation.
- `ManagerMaintenanceWorker` owns one `std::jthread`, runs one immediate pass,
  waits on a cancellable event boundary for each later interval, creates fresh
  operation and correlation identities with a bounded deadline, and cannot
  queue or overlap passes. Stable worker identity is published before service
  callbacks, so recursive shutdown signals cancellation without waiting behind
  a concurrent external exact-join owner.
- `ManagerProcessWorkerGroup` owns one through eight injected transition
  workers, starts them in order, signals and exactly joins them in reverse
  order, rolls back a failed start, and gives concurrent shutdown callers one
  finalization owner. The retained transition worker uses the same
  self-before-wait rule for controller callbacks.

## Consequences

Periodic work is resource-bounded and process-owned, continuity and LM Studio
repair cannot overlap, and the watchdog plus maintenance owners can be injected
through the single host lifecycle boundary. Typed failures are retried only by
the next scheduled pass; there is no exponential task or callback backlog.

The production composition root must still build the concrete authority roots,
services, workers, and terminal lifetime order. Real-process maintenance,
startup, shutdown, and residue evidence plus the authoritative G16 gate remain
pending.

## Evidence basis

- `include/ForgeConductor/Contracts/IManagerMaintenanceService.h`
- `src/Composition/Windows/ManagerMaintenanceService.h`
- `src/Composition/Windows/ManagerMaintenanceService.cpp`
- `src/Hosts/Manager/ManagerMaintenanceWorker.h`
- `src/Hosts/Manager/ManagerMaintenanceWorker.cpp`
- `src/Hosts/Manager/ManagerProcessWorkerGroup.h`
- `src/Hosts/Manager/ManagerProcessWorkerGroup.cpp`
- `tests/Composition/Windows/ManagerMaintenanceServiceTests.cpp`
- `tests/Manager/ManagerMaintenanceWorkerTests.cpp`
- `tests/Manager/ManagerProcessWorkerGroupTests.cpp`
- `.forge-codex/state/decisions/P16-026-capacity-one-manager-transition-watchdog.md`
