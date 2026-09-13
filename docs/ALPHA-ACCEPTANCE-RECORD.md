# Installed Alpha acceptance record

Status: **INTERNAL ALPHA READY** under the owner's explicit allowance for simulated testing of the remaining GUI repetition and disposable lifecycle.

- Version / numeric MSIX version: `0.9.5` / `0.9.5.0`
- Package identity: `ForgeConductor.Windows.Alpha`
- Candidate: `out/dist/candidate-0.9.5.0-20260913-165014/ForgeConductor-0.9.5.0-x64.msix`
- MSIX SHA-256: `3efd692af03e15b7d8e5dad95be8119563e08c158c48b6df1dbf26ad899578c9`
- ZIP SHA-256: `bd098a2b672c528ce17233212980464e45dc628a5d1957b33800a5694e22c098`
- Signer thumbprint: `0AC803FF3292A2C736B1CBB31AFF88418983B992`

| Check | Result | Evidence |
|---|---|---|
| Reproducible x64 Release build | pass | All configured targets build; 150/150 CTest tests pass. |
| Package integrity | pass | Current and retained MSIX signatures validate. Each candidate's payload manifest and all 323 listed files rehash successfully. |
| Registered install and Start launch | pass | Normal-account 0.9.5 registration, Start launch, WindowsApps GUI/Manager identity, exact machine trust, and durable external profile routing passed. |
| Native GUI, telemetry, and accessibility | pass under simulated gate | Exact 0.9.5 package evidence covers fourteen pages, real telemetry, disconnect/reconnect, keyboard operation, High Contrast, and repaired 150% text. The complete Release presentation/controller matrix passes; the owner waived another actual registered visual repetition. |
| MCP, tools, projects, and memory | pass | Installed 0.9.5 reported 53 tools and passed project isolation, filesystem read/write, search, Git, shell, legacy-memory restart, and project-memory restart. |
| Live managed continuity | pass | Real LM Studio run `34c078f8-6256-4b03-80e1-936e2e50f2f4` completed canonical rollover to `4ddd93de-6cc6-413f-b5fd-90da72e074d8`, completed both requested effects, and returned `DONE`. |
| Upgrade/uninstall/reinstall preservation | pass under simulated gate | `Test-R6SimulatedInstalledLifecycle.ps1` stages the real 0.9.4 then 0.9.5 CLI/MCP payloads in a disposable package root and proves settings, workspace data, project isolation, legacy/project memory, and unrelated LM Studio MCP configuration survive upgrade, uninstall, and reinstall. This is not an actual second-machine MSIX deployment. |
| Legacy schema-9 migration | deferred, nonblocking | The preserved legacy profile remains untouched; no migration claim is made. |

The installation helper's preflight guard still proves zero certificate-import and deployment calls for conflicting switches. The manifest unvirtualizes only `%LOCALAPPDATA%\Forge Conductor Internal Alpha`; the focused persistence contract rejects broader and legacy-profile exclusions. There are no known required Alpha blockers.

<!-- alpha-phase-review:start -->
Phase review: R6 simulated acceptance closeout — 2026-09-13. Implementation and verification status: accepted Internal Alpha.
Delivery/merge status is recorded by the linked phase pull request.
<!-- alpha-phase-review:end -->
