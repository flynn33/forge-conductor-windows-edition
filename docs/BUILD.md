# Build on Windows
These instructions distinguish **verified backend commands** from **Alpha entry points still under implementation**.
Do not report an unexecuted future command as working.

## Existing toolchain
Use Windows 11 x64 and Visual Studio 2022 / Build Tools with MSVC v143, the Desktop C++ tools,
CMake integration, Windows SDK 10.0.26100.0 and the C++ Windows App SDK/WinUI components required by the GUI project.
The checked-in presets require CMake 3.28 or later and select `Visual Studio 17 2022`.
Use an installed compatible stable Windows App SDK and C++/WinRT package, record/pin its version in the project,
and retain that version through Alpha. Do not chase a newly released SDK or change to C# templates.
PowerShell 7 is used by the package helpers. The production product remains native C++.

Run the supplied `tooling/Get-AlphaPreflight.ps1 -Repository <root>` once to identify missing inputs.
Discover local Visual Studio with vswhere rather than copying the prior developer's absolute paths.
The old `.forge-codex/state/toolchain.json` is a fallback inventory, not portable truth.
Do not relocate it until `scripts/build.ps1` and `scripts/test.ps1` have been updated to their replacement.

## Pinned Forsetti recovery is required before configure
The existing CMake integration expects:
`.forge-inputs/forsetti-framework/Forsetti-Framework-Windows-main`.
Use the supplied `Restore-Forsetti.ps1` with an existing verified directory or the original archive.
Its known archive SHA-256 is:
`3fc89cba058830a53a2e20bd015b3c89e13c77151ca63c0ec132d0dacef0204d`.
The complete tree digest and hashing convention are recorded in
`.forge-codex/state/baseline/p03-forsetti-source-lock.json`.

`scripts/build.ps1` now invokes the checked-in `scripts/alpha/Restore-Forsetti.ps1` before configure.
The restored directory is cached locally and verified on subsequent invocations. No package directory outside
the checkout is required. Missing input is downloaded from the fixed upstream revision below.
On September 12, 2026 the recovered 171-file tree matched the original complete digest exactly.
The upstream archive contains two case-colliding pull-request templates: recovery preserves the uppercase
filename and lowercase entry's bytes, matching the original Windows extraction. No framework code is changed.

A concrete fixed upstream candidate is available at commit `63b9db87c575b2c72bfb6b3c988fcd7abd7fabe5` of
`flynn33/Forsetti-Framework-Windows`. Its CMakeLists.txt exactly matches the required file hash.
After checking existing local inputs, use this supplied recovery path:
```powershell
# Optional explicit restore from the repository root; the build also performs this step.
.\scripts\alpha\Restore-Forsetti.ps1 -Repository $PWD -FetchKnownCandidate
```
The downloaded archive's bytes need not match the old main-branch ZIP; the helper instead requires the
complete extracted source-tree digest to match the original lock. It refuses to replace an existing different tree.
A full-tree mismatch remains an explicit dependency reconciliation task, not an invitation to bypass checks.

No exact dependency archive is distributed with this instruction kit. First search the known local `.forge-inputs`
and prior supplied package locations, not the entire disk. Read the source-lock manifest to identify the exact files.
If the original input is irretrievable, locate a verified upstream snapshot and implement a transparent pin migration;
record the incompatibility and continue independent GUI/docs work rather than silently replacing Forsetti with stubs.

## Existing backend commands
Run from the repository root in a native developer PowerShell:
```powershell
$env:VCPKG_ROOT = '<actual verified vcpkg root>'
# build.ps1 resolves nlohmann-json in classic mode for the sealed dependency,
# and the main project uses manifest mode through CMakePresets.json.
.\scripts\build.ps1 -Configuration Debug -Architecture x64 `
  -Target ForgeConductor.Cli,ForgeConductor.Manager,ForgeConductor.SessionHost
```

Equivalent preset route, after required dependency setup and classic JSON preinstall:
```powershell
cmake --preset windows-msvc-x64
cmake --build --preset windows-msvc-x64-debug --parallel 4 `
  --target ForgeConductor.Cli ForgeConductor.Manager ForgeConductor.SessionHost
```
Use available parallelism appropriate to the machine. Do not apply model tool-count rules to compilation.
Do not use `--fresh` every time. Out-of-source build directory is `out/build/windows-msvc-x64`.
The generated configuration's executable locations must be discovered from the actual build, not assumed
from a stale CMakeCache committed at the repository root.

## Native app and packaging entry points
`-Product Backend / App / All` is implemented; All is the default. An explicit `-Target` keeps the previous
target-only behavior unless `-Product` is also supplied. App/All build the required production backend targets
and the WinUI MSBuild project, not every test. The app helper discovers a VS 2022 installation with the
v143 UWP/XAML tools; Desktop C++ Build Tools alone was insufficient on the tested machine.
Windows App SDK 2.4.0 and C++/WinRT 3.0.260818.1 are pinned. Outputs and executable hashes are recorded
in `out/app/x64/<configuration>/staging-manifest.json`.

Executed successfully on September 12, 2026 (Debug and Release builds plus the source-bound development-signed 0.9.3.0 package):
```powershell
.\scripts\build.ps1 -Configuration Debug -Architecture x64 -Product All
.\scripts\build.ps1 -Configuration Release -Architecture x64 -Product All
.\scripts\package.ps1 -Configuration Release -Architecture x64 -DevelopmentSigning
```
Debug app also built and launched through the MSBuild helper. The package script verified the four staged executable hashes, complete runtime/resources payload, manifest identity, signer/public certificate, embedded provenance, and every extracted payload hash. Candidate 0.9.3.0 is bound to commit `a348b41d57b24e06822276f799cfb0d21a57ab5d` and tree `5759d6d80b8228679398db41f893275270515000`. The signed internal distribution is under `out/dist/`; installed acceptance remains blocked by machine trust. Development signing uses a non-exportable current-user certificate.
`FORGE_SIGNING_PFX` remains supported for explicitly supplied signing credentials.
Installation requires machine trust for the signing publisher; see the bundled install helper and current status.

## Common failures and cheapest correction
| Failure | Correction |
|---|---|
| Sealed source missing | Restore the exact dependency; do not delete the CMake check. |
| JSON cannot be found inside Forsetti | Verify the selected vcpkg root/triplet and classic preinstall already implemented in build.ps1. |
| SDK 26100 unavailable | Install that SDK with VS components or explicitly reconcile the preset; do not use another machine's cache. |
| GUI library/CRT mismatch | Build x64 and the same Debug/Release and dynamic CRT settings on both sides. |
| XAML/MSBuild targets missing | Repair/install the C++ WinUI toolchain; avoid an improvised .NET application rewrite. |
| Packaged process cannot find sibling CLI | Stage all real production executables and resolve install-relative paths; never point at out/build. |
| `Add-AppxPackage` returns `0x800B0109` | Verify the included certificate hash, then run the bundled helper from Administrator PowerShell with `-TrustDevelopmentPublisher`; do not disable Windows trust checks. |
| Provider rejects ordinary run | Inspect the managed-run error and LM Studio `/v1/responses` compatibility; the Manager owns the ordinary run and native tool loop. Do not change `/v1/forge` ports. |

Fresh isolated profiles create the configuration, data, projects, logs, memory, and handoff directories before the Manager constructs workspace authorities. Use `--alpha-root` only with a disposable path under ignored output for development checks.

The current Alpha build path must remain restorable from a fresh clone using the documented commands.
Keep build outputs, local signing state, caches, tokens and machine paths out of tracked source.

<!-- alpha-phase-review:start -->
Phase review: R6 continuation — 2026-09-12. Implementation and verification status: [Product status](STATUS.md).
Delivery/merge status is recorded by the linked phase pull request.
<!-- alpha-phase-review:end -->
