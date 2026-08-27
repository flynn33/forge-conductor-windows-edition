# P15-002: Packaged LocalAppData Process Path Equivalence

Status: Accepted

Date: 2026-08-27

## Context

The G15 real-host predeployment smoke launches the selected Forge executable
with `%LOCALAPPDATA%\Forge Conductor` as its working directory. On the owner
machine, a desktop child launched from the packaged Codex parent has no package
identity of its own, while Windows opens that logical directory through
`%LOCALAPPDATA%\Packages\OpenAI.Codex_2p2nqsd0c76g0\LocalCache\Local\Forge Conductor`.
Both names identify the same NTFS directory. P12-004 already accepts this exact
operating-system mapping for app-owned filesystem paths, but the process
supervisor required ordinal textual equality and rejected the same directory.

## Decision

- Reuse the P12-004 packaged-LocalAppData equivalence predicate when validating
  retained process executable and working-directory handles.
- Accept a differing handle-final path only when it is the complete bounded
  `LocalAppData\Packages\<safe-family>\LocalCache\Local` mapping plus the
  unchanged requested suffix.
- Preserve P12-004 behavior for the packaged-parent child case where
  `GetCurrentPackageFamilyName` returns `APPMODEL_ERROR_NO_PACKAGE`.
- Retain all existing absolute-local-drive, trusted-root, component-relative
  open, reparse, type, case-sensitivity, hard-link, deletion, and pre-launch
  revalidation checks.
- Keep the logical Forge home in LM Studio configuration and process requests;
  do not couple Forge deployment to the parent application's package family.

## Rejected alternatives

- Adding the Codex package `LocalCache` path as a Forge authority root would
  couple the application to a development host and grant authority outside the
  product's logical data root.
- Disabling final-handle equality or accepting suffix-only aliases would weaken
  unrelated process launch protections.
- Moving G15 to a temporary Forge home would stop qualifying the production
  `WindowsApplicationPaths` behavior used on this machine.

## Consequences

Packaged-parent and ordinary unpackaged launches share one bounded path-identity
policy. The focused process regression exercises a production data-root working
directory on this machine. Foreign-package rejection and broader process-path
hardening remain part of the owner-authorized post-alpha security campaign.

## Evidence basis

- `.forge-codex/state/evidence/P15/windows-lm-studio-real-host-attempt-4-failed.json`
- `.forge-codex/state/commands/20260827T151026597Z-92e2bf74.json`
- `.forge-codex/state/commands/20260827T151128087Z-39498297.json`
- `src/Infrastructure/Windows/Detail/WindowsPathResolver.cpp`
- `src/Infrastructure/Windows/WindowsProcessSupervisor.cpp`
- `tests/Infrastructure/Windows/WindowsProcessSupervisorTests.cpp`
- `.forge-codex/state/decisions/P12-004-packaged-localappdata-path-identity.md`
- `.forge-codex/state/decisions/OWNER-002-alpha-release-qualification-scope.md`
