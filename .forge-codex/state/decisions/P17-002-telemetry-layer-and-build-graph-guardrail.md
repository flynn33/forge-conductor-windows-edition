# P17-002: Telemetry Layer and Build-Graph Guardrail

Status: Accepted

Date: 2026-08-30

## Context

P17-001 introduces an independently testable Windows telemetry implementation
that depends inward on Contracts and Domain. The G04 build-scaffold validator
uses an exact CMake layer graph. Its expected graph still described the P04
scaffold and had not been synchronized with the Manager protocol/startup,
Dashboard protocol/Windows, Session Host JSON, Infrastructure Manager-protocol,
or CLI composition edges introduced by later passed phases. Adding the new
telemetry layer therefore exposed both the new expected edge and inherited
guardrail drift.

The owner has directed the project to avoid repeated full-gate rebuilds: an
already passing build remains passing unless a relevant change invalidates it.
The P17 slice changes the graph but does not require replaying the complete G04
Debug/Release gate to validate its static expectations.

## Decision

- Register `ForgeConductor.Telemetry.Windows` through `forge_add_layer` with
  the public alias `ForgeConductor::Telemetry.Windows` and the sole public
  dependency `ForgeConductor::Contracts`.
- Keep Windows native libraries private link-only implementation dependencies.
- Synchronize the G04 exact expected graph with every current
  `forge_add_layer` declaration and the current CLI composition root.
- Preserve the default G04 behavior and its authoritative Debug/Release build
  and test path. Do not reinterpret or rerun the passed gate for this focused
  checkpoint.
- Validate the static prefix of G04 after the synchronization and include the
  result in the P17 focused evidence record.

## Consequences

- The new telemetry target cannot acquire a lateral Infrastructure dependency
  without failing the exact graph guardrail.
- The build-scaffold validator once again describes the graph it is intended to
  protect.
- The owner-approved no-redundant-rebuild scope is preserved; G04 remains
  passed while the P17 build validates only affected Release targets.

## References

- `.forge-codex/state/decisions/P17-001-typed-telemetry-observations-and-collector-boundaries.md`
- `.forge-codex/state/decisions/OWNER-002-alpha-release-qualification-scope.md`
- `CMakeLists.txt`
- `scripts/validation/Test-G04BuildScaffold.ps1`
