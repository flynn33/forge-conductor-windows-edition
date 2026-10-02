# Release verification

Forge Conductor uses native unit, integration, protocol, persistence, presentation, packaging-contract, and simulated lifecycle coverage. Release acceptance is based on a complete x64 Release build and the entire configured CTest matrix, not a reduced smoke subset. The test entry point first builds the configured test graph so fresh and incremental runners cannot execute stale fixture binaries.

## Required release checks

From a Visual Studio 2022 Developer PowerShell:

```powershell
./scripts/build.ps1 -Configuration Release -Architecture x64 -Product All -Parallel 4
./scripts/build.ps1 -Configuration Release -Architecture x64 -Product Backend -Parallel 4
./scripts/test.ps1 -Configuration Release -Architecture x64 -Parallel 4
./scripts/Run-Static-Gates.ps1
./scripts/validation/Test-PackagePersistenceContract.ps1
./scripts/package.ps1 -DevelopmentSigning -ReleaseChannel prerelease
./scripts/validation/Test-ReleaseLifecycle.ps1 `
  -PreviousCandidate 'out/dist/release-1.3.4.0-20260930-131641' `
  -CurrentCandidate 'out/dist/release-1.3.5.0-YYYYMMDD-HHMMSS'
```

Replace the lifecycle command's timestamp placeholder with the distribution directory emitted by packaging. Production publication replaces `-DevelopmentSigning` with an approved PFX and password. Packaging accepts only the exact executables recorded by a clean committed staging manifest, verifies their hashes, signs the MSIX, validates its signature, and emits distribution hashes and provenance.

## Coverage boundaries

- Domain, application, Manager protocol, MCP, telemetry, persistence, tools, memory, continuity, settings, reset, and presentation behavior run in the native CTest graph.
- Persistence tests migrate supported legacy fixtures through the immutable ledger to central schema 11, verify that a released version-9 store survives the guarded C010 and C011 upgrades without fabricated provenance or project scope, prove a current schema-11 reopen is byte-stable, and confirm that a future schema is refused without mutation.
- GUI controller and presentation tests cover every destination, disconnect/reconnect, command states, accessibility metadata, High Contrast resources, and scaled layout behavior.
- Package contract tests enforce the stable identity and narrow production data exclusion.
- Simulated lifecycle tests use disposable roots and copied fixtures. Tests and packaging must never mutate the operator's live `%LOCALAPPDATA%\Forge Conductor` store.
- Provider-dependent productive inference is reported separately when LM Studio and a loaded model are available; provider absence must produce an actionable disconnected state rather than fabricated success.

## 1.3.5 candidate-specific evidence

- The loopback provider regression supplies downloaded-but-unloaded, loaded embedding, and duplicate loaded-LLM entries in an inventory larger than 64 KiB but within the 2 MiB bound. Its catalog key differs from its loaded instance ID, proving Work Space discovery and Test select the loaded instance.
- Manager preference coverage proves false-to-true-to-false changes update one project/provider record through optimistic versions, a same-value save performs no rewrite, and restart readback returns the durable value. Protocol codec coverage proves protocol-v1 managed-run output remains exactly 19 fields while decode accepts the optional transitional continuity field.
- Managed-run persistence tests prove the automatic-continuity preference remains part of idempotency after restart and exact pre-enrichment request replay survives later package, policy, and preference changes. Coordinator and Windows-repository tests prove an exact checkpoint replay succeeds, a later checkpoint reaches successor bootstrap/resume, unconsumed checkpoint states are abandoned on terminal/failure paths, a lease-owned pre-ingress pass abandons checkpoint-only orphans, periodic recovery preserves live checkpoint-only state, and a stale or post-successor refresh is rejected by digest CAS.
- Instruction-package regressions prove an interpreted entry that cannot fit beside the requested task returns `PayloadTooLarge` before a run starts or its queue cursor advances. They also prove that the exact cursor plan is persisted with a dispatch-pending run, multi-row cursor updates roll back atomically, provider and tool effects do not begin after a cursor failure, and restart replay reconciles either an unapplied or already committed plan before releasing dispatch.
- The machine-tool regression injects executable-shaped Git and PowerShell shims into ambient `PATH` and proves the shared Manager, CLI/MCP, policy, Doctor, native-shell, and LM Studio serve-verification resolver ignores them. The six-consumer static audit rejects independent ambient resolution. Coverage also proves HKLM strings are read raw with no-expand semantics, caller-controlled variables cannot rewrite machine configuration, and caller `PATH`, `PATHEXT`, and `COMSPEC` are replaced by a machine-toolchain environment that excludes per-user Forge, LM Studio, and dotnet tool directories.
- The real-Manager project-memory regression loads 101 records through guarded Display All paging, deletes one exact record, then deletes two selected records through separate `project_memory.forget` calls and verifies authoritative readback after each mode.
- A live same-host LM Studio check discovered and tested loaded model `openai/gpt-oss-20b` and completed the Responses contract probe without provider authentication.
- A post-fix live automatic-continuity test passed with loaded model `openai/gpt-oss-20b`, using a disposable project, profile, and instruction package. It proved Manager-owned create/bootstrap, structured acknowledgement, predecessor fencing, and productive successor work. This source-build check does not replace installed UI acceptance.

The completed model-discovery/Responses check qualifies only that live path. The continuity path remains unqualified until its fresh post-fix run passes. Neither check installs the MSIX, exercises packaged WinUI controls, enrolls an existing LM Studio desktop chat, or replaces owner acceptance.

## Historical 1.0 baseline

The September 13, 2026 release audit completed the full x64 Release product build and passed all 150 configured CTest entries. Static native-stack, no-Python, and attribution gates passed, as did the production package-persistence contract. A release distribution is regenerated from the final commit so its embedded commit, tree, executable hashes, and signature can be independently checked.

Historical 0.9.x installed-candidate and lifecycle evidence remains under the archived delivery records. Those artifacts are useful regression evidence but are not presented as the 1.0 package.
