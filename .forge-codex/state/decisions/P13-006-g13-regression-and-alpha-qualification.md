# P13-006: G13 Regression and Alpha Qualification

Status: Accepted

Date: 2026-08-27

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
- The infrastructure unit executable has a 120-second CTest fail-safe timeout.
  It remains eligible for normal CTest parallel scheduling.
- The first authoritative G13 build succeeded, but its subsequent test pass
  exposed a nondeterministic G06 test-harness hang. The diagnostics rotation
  test suspended an arbitrary same-process worker instruction after observing
  a staging filename. That worker could hold a CRT, filesystem, or sink lock
  needed by the test thread. `RUN_SERIAL` was rejected because it cannot repair
  an intra-process deadlock. This attribution is a code-analysis diagnosis
  corroborated by two live parent-process snapshots: PIDs 9644 and 27652 each
  retained a worker in the suspended wait state while the main test thread
  stopped making progress. The failed command output alone proves the isolated
  infrastructure-suite failure, not its internal cause.
- Diagnostics rotation tests use a private, injected, bounded cooperative
  checkpoint after exclusive staging-file creation and retain the existing
  checkpoint immediately before staged-handle publication validation. The
  cancellation, collision, and crash-recovery fixtures synchronize only at
  those owned boundaries. Arbitrary `SuspendThread` and `ResumeThread` calls
  are prohibited from the suite.
- G13 may reuse the successful fresh-build evidence once for this repair. The
  recovery path pins the baseline commit, its direct repair commit and tree,
  the failed record identifier and file hash, both output hashes, the exact
  command, working directory, role, and parallelism. It rejects tracked,
  staged, unstaged, or untracked build-input differences from the reviewed
  repair and rejects reuse after any prior recorded successful recovery. It
  then incrementally rebuilds every affected G06/G13 target and completes the
  same parallel test, inventory, artifact-hash, and repository-integrity
  assertions. This implements the owner's direction that a successful fresh
  rebuild is not repeated merely to recover a gate-harness defect while
  ensuring all changed inputs are rebuilt.
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

The recovery path cannot replace a failed build or accept unrelated build
input changes. It preserves the successful fresh build and adds a bounded
incremental rebuild for the exact diagnostics repair before rerunning the full
retained test selection.

## Evidence basis

- `.forge-codex/state/decisions/OWNER-002-alpha-release-qualification-scope.md`
- `.forge-codex/state/gate-results/G06.json`
- `.forge-codex/state/gate-results/G12.json`
- `.forge-codex/state/decisions/P13-005-native-services-to-mcp-parity-boundary.md`
- `.forge-codex/state/commands/20260827T001102639Z-955cd3e3.json`
- `.forge-codex/state/commands/20260827T001102639Z-955cd3e3.stdout.txt`
- Repair commit `03f8b5267a60ccff532f26f204cc7b2eb390eb50`, whose
  parent is the successful fresh-build baseline
  `05e93de83fcf31aef32cb12e6a324a18ecfc27c2`
- `.forge-codex/instructions/plans/phases.json`
- `.forge-codex/instructions/plans/gates.json`
