# P16-034: Authority-Bound LM Studio Selected Read Scope

Status: Accepted

Date: 2026-08-30

## Context

LM Studio discovery begins with a bounded read capability broad enough to
inspect registry-, location-, and process-derived candidates. Manager Doctor
and maintenance must not retain that discovery scope after selection. The
selected status alone did not retain sufficient provenance to prove which
candidate evidence and authority identity produced its paths.

## Decision

- `WindowsLMStudioCandidateSelector` extracts the existing bounded precedence
  and hostile-JSON validation behavior from `WindowsLMStudioEnvironment`. It
  returns the selected status beside one immutable evaluation for every input
  candidate and the exact project, caller, authority identifier, and generation
  used for selection.
- Native discovery may retain the same canonical application or configuration
  path from multiple independent sources. Same-kind observations coalesce and
  do not broaden the final authority; classifying one path as both application
  and configuration is a conflict. Multiple selected resources, resource-less
  valid candidates, or a candidate that names both resource kinds are rejected.
- The selection authority and final read-scope authority use distinct injected
  identifiers. They retain the same dedicated maintenance project and caller,
  use nonzero generations, and cannot become value-identical capabilities.
- Resolution verifies the selection identity, evaluation/evidence consistency,
  candidate bound, selected status paths, cancellation, and deadline before it
  creates an issuer.
- The final capability contains at most four canonical, nonoverlapping roots:
  the selected LM Studio configuration parent, selected LM Studio executable
  parent, exact Forge CLI parent, and Forge data root. Equal roots deduplicate;
  overlapping ancestors and drive-root broadening fail closed.
- The issued scope grants only Read, explicitly denies Write, Create, Delete,
  and Execute, and disables shell execution. A move-only aggregate retains the
  immutable issuer beside the capability for its whole use lifetime.

## Consequences

Doctor and maintenance read only the exact selected host and Forge resources,
while retained discovery remains explainable and can include corroborating
native evidence. Candidate discovery behavior remains shared with the already
qualified environment service instead of being reimplemented in the Manager
root.

Resolution currently requires one valid selected application and one valid
selected configuration. A missing optional LM Studio installation or malformed
configuration remains a truthful degraded composition result rather than a
fabricated authority. The production root, deployment-capable write scope,
real-process evidence, and authoritative G16 gate remain pending.

## Evidence basis

- `include/ForgeConductor/Infrastructure/Windows/WindowsLMStudioCandidateSelector.h`
- `src/Infrastructure/Windows/WindowsLMStudioCandidateSelector.cpp`
- `include/ForgeConductor/Infrastructure/Windows/WindowsLMStudioEnvironment.h`
- `src/Infrastructure/Windows/WindowsLMStudioEnvironment.cpp`
- `src/Composition/Windows/ManagerLmStudioReadScopeResolver.h`
- `src/Composition/Windows/ManagerLmStudioReadScopeResolver.cpp`
- `tests/Infrastructure/WindowsLMStudioEnvironmentTests.cpp`
- `tests/Composition/Windows/ManagerLmStudioReadScopeResolverTests.cpp`
- `.forge-codex/state/decisions/P15-001-evidence-based-lm-studio-deployment-and-maintenance-authority.md`

