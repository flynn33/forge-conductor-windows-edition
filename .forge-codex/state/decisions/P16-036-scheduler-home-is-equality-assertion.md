# P16-036: Scheduler Home Is an Equality Assertion

Status: Accepted

Date: 2026-08-30

## Context

P16-022 and P16-024 require the exact per-user Task Scheduler action to retain
the resolved Manager home as a quoted `--home` argument. P16-032 subsequently
established that the production Manager independently resolves its current-user
Windows application root with no explicit root and no environment override.
Treating the persisted argument as a root-selection input would conflict with
that lease-ordered environment contract and could allow scheduler drift to
redirect production state before exclusive Manager ownership was proved.

Controlled real-process fixtures still need isolated roots, but an installed
production command line must not become a second root-authority mechanism.

## Decision

- `ForgeConductor.Manager.exe` accepts only the optional `--home <absolute
  local Windows path>` equality assertion and the macOS-compatible `--open`
  browser request. Unknown, duplicate, missing, malformed, relative,
  oversized, embedded-NUL, and invalid-UTF-16 arguments fail with typed domain
  errors. The unproven `--open-browser` alias is not accepted.
- The production composition root independently resolves the current-user
  known-folder home through the read-only Manager process-environment
  inspection. It never passes `--home` into environment options, application
  path construction, capability issuance, or directory preparation.
- When `--home` is present, the root compares that assertion with its
  independently resolved canonical home and fails before instance-lease
  acquisition and before any application-owned directory mutation on a
  mismatch. Absence of the assertion does not change the resolved root.
- The canonical Task Scheduler definition continues to persist `--home` so a
  task launched after known-folder or installation drift fails closed instead
  of silently selecting another state tree.
- Test and real-process fixtures select isolated roots only through injected
  `ManagerProcessEnvironmentOptions`. Fixture roots do not enter production
  argument parsing, and production continues to disable environment override.

## Consequences

P16-022/P16-024 startup reproducibility and P16-032 production root authority
now compose without two competing owners. A stale or altered scheduled action
cannot redirect configuration, diagnostics, database, project, or export state;
it terminates before taking the singleton lease or creating directories.

The `--home` spelling remains behavioral parity evidence and a durable drift
assertion, not a supported production data-root override. Tests that require a
different root must construct the composition environment explicitly.

## Alternatives rejected

- Using `--home` as `explicitDataRoot` would make the scheduler command line an
  authority input and violate the production option fixed by P16-032.
- Dropping `--home` from the task would weaken exact launch drift detection and
  abandon the established macOS behavior without a replacement assertion.
- Reading an environment variable for fixture isolation would reintroduce a
  mutable production override and make test authority implicit.

## Evidence basis

- `.forge-codex/state/decisions/P16-022-exact-per-user-manager-startup-policy.md`
- `.forge-codex/state/decisions/P16-024-native-task-scheduler-canonicalization.md`
- `.forge-codex/state/decisions/P16-032-lease-ordered-manager-process-environment-and-terminal-ownership.md`
- `.forge-inputs/macos-audit/forge_conductor_audit_final/Forge-Conductor-Key-Evidence.md`
- `src/Hosts/Manager/ManagerProcessArguments.h`
- `src/Hosts/Manager/ManagerProcessArguments.cpp`
- `tests/Manager/ManagerProcessArgumentsTests.cpp`
