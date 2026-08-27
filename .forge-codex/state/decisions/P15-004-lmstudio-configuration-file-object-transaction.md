# P15-004: LM Studio Configuration File-Object Transaction

Status: Accepted

Date: 2026-08-27

## Context

The sixth G15 real-host attempt completed both predeployment serve smokes and
staged both Forge-owned LM Studio plugins. It then failed while publishing the
merged `mcp.json`. The existing configuration file was owned by
`BUILTIN\Administrators`; a same-directory file created by the non-elevated
owner process was owned by the current user. Their primary group and DACL were
equal, and neither descriptor carried an explicit mandatory label, so owner
identity was the only observed mismatch.

The shared atomic replacement engine correctly rejects a staged object whose
owner, primary group, or mandatory label differs from the target. The current
non-elevated token cannot assign the Administrators owner. Repeating that
replacement during rollback therefore cannot restore an original file with
this valid machine-local ownership history.

## Decision

- Keep the global `AtomicReplaceEngine` metadata invariant unchanged.
- Do not change the user's LM Studio ownership or ACL and do not require
  elevation.
- Read and validate the original configuration through the existing bounded
  atomic reader.
- Stage the merged `mcp.json` as a new file below the deployment transaction
  tree.
- Journal the configuration backup move before invoking it. Move the original configuration file object
  into the transaction backup, then move the staged file object into the live
  path. On rollback, move it back from the
  transaction backup after removing the new live object.
- Roll back in reverse order: remove the new live object and move the original
  file object back. This preserves the original bytes, owner, group, DACL,
  labels, alternate streams, and file identity without privileged metadata
  mutation.
- When the configuration was initially absent, rollback removes the newly
  published object.
- Preserve an unrelated caller-owned `mcp.json.bak`; the deployment transaction
  does not use the atomic store's sidecar backup feature.
- Retain the transaction tree when rollback itself fails so the original
  configuration backup is not destroyed.

## Rejected alternatives

- Weakening or bypassing the atomic engine's owner, group, or mandatory-label
  checks would reduce an application-wide integrity invariant.
- Changing the host file's owner or DACL would mutate user-managed security
  state and require privileges the current process does not have.
- Requiring elevation would make routine per-user deployment dependent on UAC
  and would not preserve the existing file object by construction.
- Restoring only the original bytes through another atomic replacement would
  reproduce the same owner mismatch and would not preserve file identity.

## Consequences

LM Studio configuration publication now uses the same explicit stage, backup,
commit, validation, and reverse-rollback model as the two plugin roles. The
focused native suite tracks synthetic file-object identities, asserts exact
configuration move ordering, proves existing-configuration and absent-file
rollback, exercises before/after ambiguous move failures, retains the original
backup on rollback failure, and covers all 17 precommit mutation boundaries.

The owner-authorized post-alpha security campaign remains deferred. This
decision corrects transaction compatibility and data preservation; it does not
weaken security hardening or broaden host authority.

## Evidence basis

- `.forge-codex/state/evidence/P15/windows-lm-studio-real-host-attempt-6-failed.json`
- `.forge-codex/state/evidence/P15/g15-attempt-6-predeployment-diagnostic.json`
- `.forge-codex/state/commands/20260827T160759833Z-4fa891ce.json`
- `.forge-codex/state/commands/20260827T161149424Z-f231584a.json`
- `.forge-codex/state/commands/20260827T162901514Z-e2606ac4.json`
- `src/Infrastructure/Windows/WindowsLMStudioDeploymentService.cpp`
- `src/Infrastructure/Windows/Detail/AtomicReplaceEngine.cpp`
- `tests/Application/LMStudioDeploymentServiceTests.cpp`
- `.forge-codex/state/decisions/OWNER-002-alpha-release-qualification-scope.md`

