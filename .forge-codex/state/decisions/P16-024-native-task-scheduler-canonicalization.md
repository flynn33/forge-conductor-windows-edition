# P16-024: Native Task Scheduler Canonicalization

Status: Accepted

Date: 2026-08-30

## Context

The first current-machine lifecycle run of the P16 Task Scheduler adapter
exposed native persistence rules that the transport-neutral P16-022 policy did
not yet model. Windows rejected an empty `Repetition` element because its
interval is required, and rejected `DeleteExpiredTaskAfter=PT0S` on the
boundary-less logon trigger. Once registration succeeded, the scheduler
persisted `RegistrationInfo.URI` as the deterministic task-store path,
normalized the current-user SID fields to an account name, and reported the
unified scheduling engine as enabled for the Task Scheduler 2.4 definition.

Newly registered tasks also return the documented
`SCHED_S_TASK_HAS_NOT_RUN` success status from last-run getters. Treating that
positive status as a generic COM failure made an exact registration impossible
to inspect or remove through the application-owned path.

## Decision

This decision amends the ownership-URI paragraph and the implied unified-engine
value in P16-022. All other P16-022 boundaries remain in force.

- The canonical ownership URI is the exact deterministic task-store path:
  `\ForgeConductor.Manager.v1.<stable-current-user-key>` with the optional
  `.<purpose-suffix>` included. The stable key remains the existing 64-character
  lowercase digest, and a suffix remains nonempty bounded safe ASCII
  (`A-Z`, `a-z`, `0-9`, `_`, or `-`).
- The builder constructs the task path once, converts that same value to strict
  UTF-8, and uses it as `RegistrationInfo.URI`. The concrete adapter rejects a
  resolved registration if its lookup path and ownership URI differ.
- The canonical Task Scheduler 2.4 registration enables the unified scheduling
  engine because that is the value Windows persists and projects for this
  definition on the supported alpha machine.
- An absent trigger repetition remains absent; the writer does not request an
  `IRepetitionPattern` merely to write empty values.
- The canonical zero expiration delay is represented by an absent native
  `DeleteExpiredTaskAfter` value. The reader normalizes that absence back to the
  policy's fixed zero duration.
- `SCHED_S_TASK_HAS_NOT_RUN` is accepted only for the last-result and
  last-run-time getters, where it maps to empty optional history. Other positive
  COM statuses remain failures at exact boundaries.
- Principal and logon-trigger identity getters may return either the canonical
  current SID or a Windows account name. A bounded `LookupAccountNameW`
  resolution is accepted only when it resolves byte-for-byte to the current
  process-token SID; unresolved or different accounts fail closed. The model
  continues to compare canonical SID text.

## Consequences

An exact task now round-trips through the actual Windows representation without
being misclassified as foreign or drifted. Ownership remains source-and-URI
exact, so a different purpose suffix is a different registration and can never
be repaired or removed as if it were the requested task.

Any pre-alpha task created with the superseded URN ownership value is foreign
to this contract and will not be mutated automatically. No migration is added
because that representation was never shipped; an explicit narrowly validated
migration would be required if such a task had to be adopted later.

The account-name normalization is not a general aliasing rule. It exists only
at the Windows persistence boundary and proves equality with the current SID
before returning the canonical value.

## Alternatives rejected

- Accepting both the URN and task path as owned would create two ownership
  identities for the same native name and weaken the mutation boundary.
- Ignoring URI or unified-engine drift would allow scheduler-persisted behavior
  to evade the exact policy.
- Writing empty repetition fields or `PT0S` expiration unconditionally would
  continue producing schema-invalid logon tasks.
- Accepting arbitrary account names would weaken the current-user invariant.
- Treating every positive HRESULT as success would conceal unexpected COM
  outcomes outside the documented not-yet-run getters.

## Evidence basis

- Microsoft Task Scheduler schema and COM contracts:
  <https://learn.microsoft.com/en-us/windows/win32/taskschd/task-scheduler-schema>
- Registration URI task-store-path contract:
  <https://learn.microsoft.com/en-us/openspecs/windows_protocols/ms-tsch/4b178ab9-afd9-46e1-88c6-fa3bda613121>
- Repetition interval requirement:
  <https://learn.microsoft.com/en-us/windows/win32/taskschd/taskschedulerschema-repetitiontype-complextype>
- Expiration-delay semantics:
  <https://learn.microsoft.com/en-us/windows/win32/api/taskschd/nf-taskschd-itasksettings-put_deleteexpiredtaskafter>
- Unified scheduling engine contract:
  <https://learn.microsoft.com/en-us/windows/win32/api/taskschd/nf-taskschd-itasksettings2-put_useunifiedschedulingengine>
- Task Scheduler success constants:
  <https://learn.microsoft.com/en-us/windows/win32/taskschd/task-scheduler-error-and-success-constants>
- `src/Infrastructure/Windows/Detail/ManagerStartupDefinitionBuilder.cpp`
- `src/Infrastructure/Windows/Detail/WindowsTaskSchedulerStartupPlatform.cpp`
- `tests/Manager/ManagerStartupTaskPolicyTests.cpp`
- `tests/Manager/ManagerStartupDefinitionBuilderTests.cpp`
- `tests/Manager/WindowsManagerStartupLifecycleTests.cpp`
- `.forge-codex/state/evidence/P16/manager-startup-task-scheduler-checkpoint.json`
