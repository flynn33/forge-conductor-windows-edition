# P16-038: Hash-Bound G16 Qualification and Independent Finalization

Status: Accepted

Date: 2026-08-30

## Context

P16 requires runtime evidence for single Manager ownership, authenticated named
pipe and loopback control, startup registration, bounded restart, and terminal
shutdown. OWNER-002 permits one authoritative affected-target build and one G16
test invocation. The independent Validator must review that evidence without
rebuilding or rerunning the full gate.

The feature-parity JSON and TSV ledgers cannot truthfully transition until the
authoritative command has passed. Their post-pass commit changes Git HEAD even
though it does not change the qualified binaries or tests. Conversely, any
other tracked build or governance input change must invalidate the evidence.

Interrupted execution also needs a fail-closed outcome. A stale CMake cache,
binary, CTest inventory, or LastTest log cannot be promoted into passing gate
evidence merely because it remains on disk.

## Decision

`scripts/validation/Test-G16ManagerIpcDashboard.ps1` is the evidence-producing
G16 runner. It does not mutate the gate or phase ledgers. Terminal G16 and P16
state remains the independent Validator's responsibility after the outer
command record and parity-ledger transition exist.

The runner:

- holds one exclusive workspace-scoped lock for normal qualification and
  hash-bound evidence review;
- resolves every authoritative tool from the exact toolchain-state path and
  binds live executable hashes, vcpkg checkout identity, CMake cache, and
  configured compiler state;
- binds static analysis, helper execution, and terminal evidence to one source
  fingerprint and revalidates source, runner, toolchain, and artifact identity
  immediately before success;
- pins the complete ordered CMake and runtime CTest command contract for all 83
  G16 tests, including every fixture and CLI argument;
- records a versioned attempt checkpoint before each irreversible build or test
  transition, validates each candidate checkpoint before replacing the
  canonical checkpoint, and performs all fail-capable preflight before counting
  the helper invocation as started;
- writes `build_passed` only after capturing an immutable aggregate of the
  configured build state, all test command artifacts, production binaries,
  binary-policy scans, and embedded dashboard resources;
- moves any preexisting transient LastTest log into append-only attempt history
  before `test_started`;
- writes `test_passed` only after the exact 83-test LastTest log is durable and a
  fresh post-test cache, compiler, inventory, test-artifact, production-artifact,
  and resource bundle equals the pre-test bundle;
- refuses to infer success from an interrupted `build_started` or
  `test_started` checkpoint, while preserving it in hash-linked history before
  a subsequent explicit same-context retry; and
- archives a prior attempt, summary, and logs with a hash-linked rollover
  manifest before one replacement invocation when the source or live toolchain
  context changes. A passing rollover always preserves and parses the durable
  CTest log; its build-tree copy is additionally bound when still present.

The immutable source fingerprint excludes exactly these two mutable governance
ledgers:

- `.forge-codex/instructions/plans/feature-parity-matrix.json`
- `.forge-codex/instructions/plans/feature-parity-matrix.tsv`

Both files remain subject to the normal tracked/untracked cleanliness checks.
All other tracked build and governance inputs remain in the aggregate source
fingerprint. This permits only the post-pass parity commit between the Builder
evidence and Validator review.

After the authoritative command passes, the Builder updates only the supported
G16 parity rows and commits them. The Validator invokes the runner with
`-Resume`, `-StaticOnly`, the canonical summary path, and its exact SHA-256. The
resume path revalidates current source, toolchain, attempt, binaries, CTest
commands, resources, and durable log without invoking a build or test. Only
then may the Validator write the report and terminalize G16 and P16.

## Consequences

- One successful G16 build/test invocation is sufficient for the current-machine
  alpha; no duplicate Validator rebuild is needed.
- An interrupted attempt cannot silently become a pass from stale output.
- A changed affected source or live toolchain receives one new authoritative
  invocation, while the predecessor evidence remains recoverable and linked.
- A parity-only commit does not invalidate already qualified runtime artifacts.
- The overall run remains in progress after P16; later UI, packaging, install,
  profiling, and other non-deferred alpha gates remain required.

## References

- `.forge-codex/state/decisions/OWNER-002-alpha-release-qualification-scope.md`
- `.forge-codex/state/decisions/P16-001-single-owner-manager-and-current-user-control-plane.md`
- `.forge-codex/state/decisions/P16-037-production-manager-composition-boundaries.md`
- `.forge-codex/instructions/plans/gates.json`
- `.forge-codex/instructions/plans/feature-parity-matrix.json`
