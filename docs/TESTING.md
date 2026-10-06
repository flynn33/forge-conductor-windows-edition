# Release verification

Forge Conductor uses native unit, integration, protocol, persistence, presentation, packaging-contract, and simulated lifecycle coverage. Release acceptance is based on a complete x64 Release build and the entire configured CTest matrix, not a reduced smoke subset. The test entry point first builds the configured test graph so fresh and incremental runners cannot execute stale fixture binaries.

## Required release checks

From a Visual Studio 2022 Developer PowerShell:

```powershell
./scripts/build.ps1 -Configuration Release -Architecture x64 -Product All -Parallel 4
./scripts/test.ps1 -Configuration Release -Architecture x64 -Parallel 4
./scripts/Run-Static-Gates.ps1
./scripts/package.ps1 -DevelopmentSigning
```

Production publication replaces `-DevelopmentSigning` with an approved PFX and password. Packaging accepts only the exact executables recorded by a clean committed staging manifest, verifies their hashes, signs the MSIX, validates its signature, and emits distribution hashes and provenance.

## Coverage boundaries

- Domain, application, Manager protocol, MCP, telemetry, persistence, tools, memory, continuity, settings, reset, and presentation behavior run in the native CTest graph.
- Persistence tests migrate supported legacy fixtures, verify the released version-9 layout and ledger, prove a version-9 open is byte-stable, and confirm a future schema is refused without mutation.
- GUI controller and presentation tests cover every destination, disconnect/reconnect, command states, accessibility metadata, High Contrast resources, and scaled layout behavior.
- Package contract tests enforce the stable identity and narrow production data exclusion.
- Simulated lifecycle tests use disposable roots and copied fixtures. Tests and packaging must never mutate the operator's live `%LOCALAPPDATA%\Forge Conductor` store.
- Provider-dependent productive inference is reported separately when LM Studio and a loaded model are available; provider absence must produce an actionable disconnected state rather than fabricated success.

## 1.3.13 local-model workflow verification

The complete x64 Release graph was rebuilt by `scripts/test.ps1 -Configuration Release -Architecture x64 -Parallel 4` and passed **156/156 configured CTest entries** in **49.13 seconds**, exit code 0. This run includes the asynchronous Responses transport deadlines, partial-body cancellation, shutdown, provider-cancel POST, reviewer source/receipt compatibility, recovered workspace bindings, consistent cwd authorization, real filesystem ACL denials, and bounded UTF-8 PowerShell stdin regressions. Direct comparisons also matched the original PowerShell command exit behavior in 38 cases across PowerShell 7 and Windows PowerShell 5.1.

Live provider runs and final installed artifact checks have separate identities and limits in the [1.3.13 repair record](validation/LM-STUDIO-ISSUE-REPAIRS-1.3.13.md). The published `release-verification.json` and executable provenance record bind final package qualification to the actual release payload; earlier source-probe hashes remain historical evidence.

## 1.3.6 telemetry-layout verification

The full x64 Release CTest matrix passed 153/153 in 32.36 seconds with exit code 0. Release App/backend/package builds exited 0 and all three static gates passed.

An in-process observer measured actual WinUI XAML with an injected isolated ManagerConnection: 16 telemetry polls at the existing 500 ms cadence, sampled every 20 ms. The baseline Rig position moved 130.667 logical units with 25 transitions. The fixed source had zero transitions, with injected CPU values still changing from 30.0% through 34.0%. This is controlled-data native layout evidence, not visual inspection of the installed App.

Installed original 1.3.6 package identity, running App/Manager, and matching App/Manager/CLI payload hashes were verified. Publication preserves this exact signed artifact from source commit `9912541debc92c1117cceab0f6322c4e62728ee4`; later documentation commits do not change product source, and the differently hashed later rebuild is not published. Native-control initialization failed with MODULE_NOT_FOUND for kernel.js; installed-window visual acceptance is not claimed. See [1.3.6 release notes](releases/1.3.6.md).

## Historical 1.3.5 native-chat verification

The complete x64 Release backend and App builds exited 0. The final configured CTest matrix reported `100% tests passed out of 153` and `Total Test time (real) = 31.54 sec`, with exit code 0. All three static gates passed. These Release results are separate from the nine affected Debug suites used during native-chat development.

The first full Release run passed 152/153: `process.packaged-localappdata-working-directory` in test 108 encountered Windows sharing error 32 because its temporary fixture was inside the installed Manager's shared LocalAppData directory. The test fixture was moved to a unique sibling directory while retaining the packaged working-directory assertion. The focused executable passed 25/25 process cases, then the full Release matrix passed 153/153 with the installed App and Manager still running. No production behavior or test assertion was weakened to release that lock.

The native-chat check used isolated Debug binaries and is separate from the configured Release suite. The native predecessor/successor files contain packet handoff, all three integrations, packet retrieval, and following Forge calls. See [release notes](releases/1.3.5.md) for exact IDs.

Auto Continuity was verified with a reserve-triggered pause, not physical context exhaustion. Rollover was verified while the primary MCP worker stayed alive. Interrupted handoff after idle-process eviction is not durable and is not claimed.

## Historical 1.0 baseline

The September 13, 2026 release audit completed the full x64 Release product build and passed all 150 configured CTest entries. Static native-stack, no-Python, and attribution gates passed, as did the production package-persistence contract. A release distribution is regenerated from the final commit so its embedded commit, tree, executable hashes, and signature can be independently checked.

Historical 0.9.x installed-candidate and lifecycle evidence remains under the archived delivery records. Those artifacts are useful regression evidence but are not presented as the 1.0 package.
