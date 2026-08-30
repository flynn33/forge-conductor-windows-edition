# P16-022: Exact Per-User Manager Startup Policy

Status: Accepted

Date: 2026-08-30

## Context

The Manager requires one per-user Windows Task Scheduler registration that can
be inspected, created, repaired, enabled, started on demand, and removed. The
registration is durable Windows state, so a fresh Manager process must be able
to reconstruct the expected task without depending on cached process state.

Task Scheduler exposes many fields that can change launch behavior. Native XSD
durations can also contain calendar-based forms that cannot be represented
losslessly as fixed seconds. Treating a partial projection as authoritative or
coercing native values would allow an altered task to be reported as ready.
Task-folder names can also collide with registrations owned by another product
or user-controlled process.

## Decision

### Stateless service contract

- Every startup operation receives the expected `ManagerStartupDefinition`.
  The platform adapter therefore rederives the current-user identity and exact
  registration on every operation and owns no hidden product state.
- Mutation results report both the resulting status and whether durable state
  changed. A foreign registration may never report `changed=true`.
- Every call carries the normal operation identity, deadline, and cancellation
  context. The eventual native adapter owns bounded serialization, cancellation,
  and shutdown.

### Exact normalized task policy

- A closed, transport-neutral task model contains ownership metadata,
  principal fields, required privileges, triggers, repetition, executable
  actions, hidden-window behavior, and all launch-relevant task settings.
- The canonical task uses the current interactive user's SID, least privilege,
  default process-token SID behavior, one immediate enabled logon trigger, one
  hidden-window executable action, `IgnoreNew`, unlimited execution time,
  one-minute restart delay with three attempts, no idle/network/battery gate,
  hard termination enabled, priority 7, and Task Scheduler 2.4 compatibility.
- Native task enablement is kept outside structural definition equality. An
  otherwise exact disabled task is `Disabled`, not `Drifted`.
- A private fixed-seconds-or-preserved-text variant makes the duration union
  structurally exclusive. Valid native duration forms that are not
  fixed-second values are preserved as bounded text and compare unequal to the
  canonical values instead of being silently coerced.
- An owned observation is authoritative only when the native adapter attests
  that every modeled launch field was projected. An incomplete owned
  observation fails integrity validation.
- Expected and observed text is bounded strict UTF-8. Current-user SID text is
  canonical Windows SID syntax: decimal identifier authority below 2^32,
  fixed-width hexadecimal authority at or above it, and at most 15
  subauthorities.
  Executable and working-directory paths fit Task
  Scheduler's 260 UTF-16-unit limit, and the complete quoted executable plus
  arguments fit the Windows 32,767 UTF-16-unit command-line ceiling.
- Observed collections use Task Scheduler's native maxima: 48 triggers, 32
  actions, and 64 required privileges. The native adapter must reject larger
  COM counts before allocating their projections.

### Ownership and repair boundary

- The task source is `ForgeConductor.Windows.ManagerStartup.v1`.
- The ownership URI is
  `urn:forge-conductor:windows:manager:<stable-current-user-key>`. The suffix
  reuses `WindowsCurrentUserIdentity::stableKey()`: the existing lowercase,
  domain-separated SHA-256 digest over the bounded binary SID. It does not
  introduce a second text-SID digest or a competing user-identity namespace.
- Ownership is classified before requiring a complete launch projection. An
  unmistakably foreign registration is reported as `ForeignConflict` and is
  never repaired, enabled, started, or removed by Forge Conductor.
- A task with exact Forge ownership but any other normalized field difference
  is `Drifted` and can be repaired transactionally by the concrete adapter.

## Consequences

The policy can distinguish missing, exact-disabled, exact-ready, owned-drifted,
and foreign registrations without using COM or Windows handles in the domain or
application boundary. Full native observations cannot hide an extra action,
trigger, privilege, boundary, repetition, policy setting, or unsupported
duration form.

The subsequent Windows adapter must build the canonical definition from the
canonical executable, executable parent, quoted `--home` value, and the current
process token SID. It must project the same closed surface from Task Scheduler,
preserve non-fixed durations, and refuse mutation unless ownership matches.

This checkpoint establishes the model, contract, policy, and focused native
tests only. It does not implement COM Task Scheduler access, register a real
task, complete P16, or invoke G16.

## Alternatives rejected

- Caching the expected task inside the service would make behavior depend on
  process history and complicate restart recovery.
- Comparing only executable path and arguments would miss security- and
  reliability-relevant scheduler drift.
- Treating task enablement as structural drift would make a deliberate disabled
  state indistinguishable from a damaged registration.
- Converting every XSD duration to seconds would misrepresent valid calendar
  durations.
- Repairing a same-name foreign registration would overwrite state outside the
  application's ownership boundary.

## Evidence basis

- `include/ForgeConductor/Domain/ManagerStartupModels.h`
- `src/Domain/ManagerStartupModels.cpp`
- `include/ForgeConductor/Contracts/IManagerStartupService.h`
- `include/ForgeConductor/Manager/ManagerStartupTaskPolicy.h`
- `src/Manager/ManagerStartupTaskPolicy.cpp`
- `tests/Manager/ManagerStartupTaskPolicyTests.cpp`
- `.forge-codex/state/evidence/P16/manager-startup-policy-checkpoint.json`
