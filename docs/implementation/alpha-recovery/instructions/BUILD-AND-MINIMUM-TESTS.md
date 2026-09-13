# Build and minimum functionality tests

## Keep the working Windows stack
The current source documents Windows 11 x64, C++20, MSVC/v143 and the existing Visual Studio/Windows SDK toolchain. `scripts/build.ps1` already builds Backend/App/All and invokes the existing Forsetti restoration helper; `scripts/alpha/Build-App.ps1` uses the WinUI C++ project and stages sibling executables. Preserve those working pins unless a specific reproducible defect requires changing one. Do not install a newer compiler just to finish the application. [S16, S21, S22]

Run build tooling from the provided D: checkout in PowerShell 7/native Windows, not from WSL/macOS or the installed product directory. Discover the existing working environment once. The build helper may consult `.forge-codex/state/toolchain.json`; retiring that old instruction framework does not authorize deletion of a live build input. Preserve configured vcpkg/Forsetti dependency state and the verified source recovery. Never replace sealed dependencies with empty stubs to satisfy a build.

## Actual existing commands

```powershell
Set-Location -LiteralPath 'D:\GitHub\Forge-Conductor-Windows-Edition'
.\scripts\build.ps1 -Configuration Debug -Architecture x64 -Product All -Parallel 4
```

The helper builds/stages `ForgeConductorApp.exe`, `forge-conductor.exe`, `ForgeConductor.Manager.exe` and `ForgeConductor.SessionHost.exe` under `out/app/x64/<Configuration>`. The backend build is `out/build/windows-msvc-x64`. These names are observed source facts, not paths to existing binaries on this package author's machine. [S21–S22]

For a backend-only coherent change, build affected actual targets, for example:

```powershell
.\scripts\build.ps1 -Configuration Debug -Architecture x64 -Product Backend `
  -Target ForgeConductor.Manager,ForgeConductor.Continuity.AutomationTests -Parallel 4
```

For an already configured build tree and relevant test binaries:

```powershell
cmake --build out/build/windows-msvc-x64 --config Debug `
  --target ForgeConductor.Continuity.AutomationTests ForgeConductor.Manager.ProtocolTests `
  ForgeConductor.SessionHost.ContinuityEndToEndTests -j 4
ctest --test-dir out/build/windows-msvc-x64 -C Debug -N
ctest --test-dir out/build/windows-msvc-x64 -C Debug `
  -R '^ForgeConductor\.(Continuity\.AutomationTests|Manager\.ProtocolTests|SessionHost\.ContinuityEndToEndTests)$' `
  --output-on-failure --no-tests=error
```

Confirm actual registered test names with `ctest -N` before selecting tests. The named continuity/protocol/session targets are identified in the committed handoff; `ForgeConductor.Manager.NamedPipeRoundTripTests` is also reported in the P1 receipt. A build target and CTest registration are not necessarily the same. Adjust a selection to the actual registration instead of silently passing an empty match. No `--no-tests=ignore`, `|| true`, fabricated success receipt or fixture in the production composition. [S04, S17, S19]

A Release candidate uses:

```powershell
.\scripts\build.ps1 -Configuration Release -Architecture x64 -Product All -Parallel 4
.\scripts\package.ps1 -Configuration Release -Architecture x64 -DevelopmentSigning
```

Choose the configured signing path instead of `-DevelopmentSigning` when the owner already supplies it. Do not print secrets or publish private keys. Packaging is covered separately in [Installer/data](INSTALLER-AND-DATA.md).

## Safe real GUI smoke
Reuse explicit `--alpha-root`; do not invent another isolation flag. The existing GUI propagates it to the Manager (S19). Choose a disposable profile below ignored output and keep its identity visible:

```powershell
$alphaRoot = Join-Path (Get-Location).Path 'out\alpha-profiles\gui-smoke'
Start-Process -FilePath '.\out\app\x64\Debug\ForgeConductorApp.exe' `
  -ArgumentList ('--alpha-root "{0}"' -f $alphaRoot)
```

First inspect the current implementation and arguments if local code has advanced. A process launch alone is not a successful GUI walkthrough. Observe the actual window/controls, real Manager attachment and meaningful actions. Do not terminate a process just because it matches a stale PID from a handoff. CLI/MCP isolation propagation must be inspected for its current supported contract rather than assumed from the GUI flag.

## Test economy and bug policy
After a coherent change, build affected targets once and run the existing test most directly covering the behavior. Add a regression case only where the bug/feature is not already covered. Usually the success path and one meaningful failure/boundary case are sufficient. Continuity needs the handful of state/identity cases specified below because a false pass would invalidate the central feature. No coverage-percentage gate or arbitrary assertion count is required.

Do not run all 90 infrastructure tests or all historical gates after every edit. Reuse still-applicable evidence. A documentation-only change needs JSON/link/diff review, not a native build. A public interface/composition change warrants the dependent focused tests, not only the leaf unit. Before the final candidate run the small integrated R6 pass once, fixing and rerunning only the failed/affected paths. Do not delete old tests to make counts look better; keep unrelated legacy suites opt-in.

There is no mandatory new CI framework. Preserve and satisfy actual repository-required checks. If a lightweight Windows PR check already exists, reuse it; do not replace honest local Windows results with a Linux/fixture green badge. Test source fixes and installation behavior with the real native build.

## Minimal functional coverage by phase

| Phase | Minimum needed to advance implementation |
|---|---|
| R0 | Git/work preservation; parse plan/status; root links/docs; owner push/phase PR. No app build solely for docs. |
| R1 | Affected builds; normal call/result correlation; context below/at threshold and duplicate accounting; invalid acknowledgment/cancel ownership; actual GUI/Manager control/reattach. |
| R2 | Collector valid/invalid/stale mapping; real CPU/RAM/process sampling and native refresh/reconnect; one keyboard/scaling pass. |
| R3 | Existing disposable local workflow plus second-project/foreign-MCP preservation; every native destination walked once. |
| R4 | Settings valid/invalid/repeated save and readback; scoped reset/cancel/late write; keyboard slider/exact value. |
| R5 | Release packaging/signature/payload; supported disposable migration and newer-store refusal; valid upgrade identity/version path. |
| R6 | Actual installed product + one actual live context-forced productive successor + one update/uninstall preservation + required GUI/settings acceptance. |
| R7 | Only functional/package delta checks if bytes changed; all-doc review, installer identity and actual PR/main synchronization. |

## Evidence format
Record the action/command, source commit or tested source tree plus dirty provenance, configuration/architecture, observed result/exit code and a single useful receipt location. State `passed`, `failed`, `blocked` or `not_run` accurately. Raw transient logs stay under ignored `out/alpha-evidence`; commit concise redacted acceptance summaries. An old report of 90/90 tests is inherited evidence, not a fresh run by the next agent.

No repeated broad benchmarking, stress/leak campaigns, all-architecture build matrix, multi-agent validators, Mac-specific gates or user-data-destructive tests. Serious reproducible functional/data-loss bugs affecting required scope must be fixed; do not defer them for cosmetic completeness.

<!-- alpha-phase-review:start -->
Phase review: R7 — 2026-09-12. Implementation and verification status: [Product status](../../../STATUS.md).
Delivery/merge status is recorded by the linked phase pull request.
<!-- alpha-phase-review:end -->
