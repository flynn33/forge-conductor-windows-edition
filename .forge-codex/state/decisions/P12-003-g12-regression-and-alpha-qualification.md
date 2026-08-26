# P12-003: G12 Regression and Alpha Qualification

Status: Accepted

Date: 2026-08-26

## Context

P12 makes additive changes to shared Domain, Contracts, Application, and CMake
surfaces after the authoritative G11 pass. The owner has directed that one
successful rebuild/test invocation is sufficient for an affected gate, while
security hardening, broad installer matrices, clean-environment testing, and
bespoke UI polish are deferred until after alpha. Native UI automation remains
required.

## Decision

- Focused developer builds and tests may run while P12 is being implemented.
- After the source is frozen, G12 receives one authoritative x64 Debug
  invocation that configures once, builds all affected targets once, runs the
  retained affected G11 tests plus all G12 tests once, validates native DLL/EXE
  artifacts, and records exact hashes and outputs.
- The independent Validator reviews the retained command record and artifacts
  without triggering another rebuild.
- G12 does not claim P14 MCP framing, P15 external LM Studio qualification, P16
  manager IPC, packaging, clean-machine support, release security hardening, or
  final UI parity. Those remain assigned to their existing phases.
- No P12 evidence weakens the retained native UI automation requirement; that
  gate remains scheduled with the UI phases.

## Consequences

Shared-contract regression evidence is renewed without repeated full-gate
rebuilds. A G12 pass means the exact autonomous native lifecycle is proven on
this machine under the alpha scope, not that the entire product is complete.

## Evidence basis

- `.forge-codex/state/decisions/OWNER-002-alpha-release-qualification-scope.md`
- `.forge-codex/state/gate-results/G11.json`
- `.forge-codex/instructions/plans/phases.json`
- `.forge-codex/instructions/plans/gates.json`
