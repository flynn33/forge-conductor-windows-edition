# P16-037: Production Manager Composition Boundaries

Status: Accepted

Date: 2026-08-30

## Context

The P16 Manager process must compose the completed application, persistence,
dashboard, continuity, LM Studio, transport, and native host slices without
changing their public contracts. Three production ambiguities remained at the
composition boundary:

- LM Studio maintenance needs distinct read and deployment capabilities, while
  `WindowsLMStudioDeploymentService` accepts one workspace-authority service.
- LM Studio is an optional external host, so its absence must not prevent the
  per-user Manager from serving unrelated product surfaces.
- Task Scheduler owns startup registration, while the Manager executable owns
  process lifecycle and must not register or rewrite its own scheduled action.

The process environment also requires the singleton lease to outlive every
service that borrows paths, credentials, callbacks, repositories, or ingress.

## Decision

- `ManagerCompositionRoot` is the executable-owned composition root. It first
  performs read-only process and path inspection, then acquires the exact
  current-user instance lease, then prepares the five application directories.
  The prepared environment is declared as the first implementation member so
  reverse destruction releases its lease last.
- The root owns every concrete implementation and injects only public
  interfaces into application services. Ingress and workers shut down before
  dashboard services, optional-host services, continuity services,
  repositories, diagnostics, secure storage, and the terminal lease.
- LM Studio uses separate immutable read and deployment capabilities issued for
  the same maintenance project and caller. `ManagerLmStudioAuthorityRouter`
  routes authorization and narrowing only by the retained authority identity;
  it refuses ambiguous capability issuance through the aggregate interface.
- When no complete authority-bound LM Studio application/configuration pair can
  be resolved, `UnavailableLmStudioDeploymentService` reports a typed
  `host_capability_unavailable` result. It does not fabricate host status,
  deployment success, or background work, and unrelated Manager surfaces stay
  available.
- Controlled real-process fixtures may disable external-host discovery only
  through an injected composition option that is absent from the production
  command line. Production keeps discovery enabled; the fixture uses the same
  truthful unavailable adapter so lifecycle tests cannot repair the operator's
  LM Studio installation while exercising isolated Manager state.
- The production Manager accepts a Scheduler-projected home only through the
  P16-036 equality assertion. Application paths are constructed independently
  from the prepared canonical environment and are checked for exact agreement
  before they are injected.
- Installer and CLI startup commands remain the only owners of Task Scheduler
  registration and repair. `ForgeConductor.Manager.exe` is the exact action
  target and process-lifecycle owner; it does not self-register or mutate its
  launch definition.
- Dashboard resources are compiled directly into the final Manager executable.
  The sibling CLI helper is staged beside that executable and is admitted only
  through its exact execute-only browser-launch authority.

## Consequences

The production binary has one explicit ownership graph, one lease owner, and a
bounded shutdown order. Optional LM Studio absence is visible and recoverable
without weakening other Manager behavior. Distinct capabilities remain
distinguishable even though the existing deployment implementation consumes a
single authority interface.

Task Scheduler policy can be tested and repaired independently from Manager
runtime construction. A scheduled Manager launch still fails closed on home or
binary drift, while direct foreground launches remain supported.

## Alternatives rejected

- Giving LM Studio maintenance one broad capability would erase the read/write
  distinction required by the public service contracts.
- Patching the sealed Forsetti interfaces or the infrastructure deployment
  service to accept a service locator would violate one-way dependency and
  sealed-framework rules.
- Treating optional LM Studio discovery failure as fatal would make an external
  application control availability of unrelated Forge Conductor features.
- Self-registering the scheduled task from Manager startup would create two
  lifecycle owners and make startup failures capable of mutating persistence.
- Linking dashboard resources only into tests or a helper DLL would make the
  production module resource lookup untruthful.

## Evidence basis

- `.forge-codex/state/decisions/P16-032-lease-ordered-manager-process-environment-and-terminal-ownership.md`
- `.forge-codex/state/decisions/P16-034-authority-bound-lm-studio-selected-read-scope.md`
- `.forge-codex/state/decisions/P16-035-capacity-one-manager-maintenance-reconciliation.md`
- `.forge-codex/state/decisions/P16-036-scheduler-home-is-equality-assertion.md`
- `src/Hosts/Manager/ManagerCompositionRoot.h`
- `src/Hosts/Manager/ManagerCompositionRoot.cpp`
- `src/Composition/Windows/ManagerLmStudioAuthorityRouter.h`
- `src/Composition/Windows/UnavailableLmStudioDeploymentService.h`
