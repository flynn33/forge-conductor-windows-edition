# P13-006: G13 Regression and Alpha Qualification

Status: Accepted

Date: 2026-08-26

## Context

P13 adds native filesystem, glob, text-search, Git, PowerShell, PDF, and
workspace-authority services to shared Domain, Contracts, Infrastructure, and
CMake surfaces after the authoritative G12 pass. The owner has directed that
one successful rebuild and test invocation is sufficient for an affected alpha
gate. Dedicated security hardening, broad installer matrices, clean-environment
testing, and bespoke UI polish are deferred; planned UI automation remains in
scope.

## Decision

- Focused developer builds and tests may run while the P13 slice is changing.
- After the implementation commit is frozen and pushed, G13 receives one
  authoritative fresh x64 Debug invocation. It builds the affected Domain,
  Contracts, Infrastructure, and NativeTools targets, then runs the four
  retained G06 infrastructure registrations and eight G13 registrations in one
  `G06|G13` CTest selection. The eighth registration is the retained G11 legacy
  continuity persistence test affected by the directory-listing contract.
- The authoritative runner verifies strict source formatting, sealed Forsetti
  inputs, repository integrity, platform-neutral public contracts, native tool
  architecture invariants, x64 PE identity, and SHA-256 hashes for the three
  affected libraries and fifteen executable artifacts.
- The independent Validator reviews the recorded command, output, frozen commit
  and tree, artifact hashes, and test log without starting another rebuild.
- G13 qualifies only the native service layer. The seventeen MCP parity rows
  remain open until P14 supplies registered schemas, routing, aliases, defaults,
  metadata, and wire-response evidence.

## Consequences

Shared-contract and retained process-supervisor behavior receive regression
evidence without a redundant full rebuild. A G13 pass establishes that the
bounded native services work on this alpha machine; it does not claim MCP,
manager, GUI, installer, or whole-product completion.

## Evidence basis

- `.forge-codex/state/decisions/OWNER-002-alpha-release-qualification-scope.md`
- `.forge-codex/state/gate-results/G06.json`
- `.forge-codex/state/gate-results/G12.json`
- `.forge-codex/state/decisions/P13-005-native-services-to-mcp-parity-boundary.md`
- `.forge-codex/instructions/plans/phases.json`
- `.forge-codex/instructions/plans/gates.json`
