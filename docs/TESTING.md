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

## Current 1.0 baseline

The September 13, 2026 release audit completed the full x64 Release product build and passed all 150 configured CTest entries. Static native-stack, no-Python, and attribution gates passed, as did the production package-persistence contract. A release distribution is regenerated from the final commit so its embedded commit, tree, executable hashes, and signature can be independently checked.

Historical 0.9.x installed-candidate and lifecycle evidence remains under the archived delivery records. Those artifacts are useful regression evidence but are not presented as the 1.0 package.
