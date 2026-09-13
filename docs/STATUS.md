# Product status

Updated September 13, 2026 for plan `windows-alpha-recovery-2026-09-12`.

Forge Conductor 0.9.5 is accepted for Internal Alpha. R0–R7 and PR #25 are merged at main `c649e80f8f32b008b7f938cce6c822bf96cebfd7`. The signed candidate is source-bound, installs and launches from Start in the normal account, attaches its packaged Manager from WindowsApps, uses the durable `%LOCALAPPDATA%\Forge Conductor Internal Alpha` profile, and exposes all 53 native tools. The owner accepted reproducible simulation instead of an additional actual-machine walkthrough and disposable account lifecycle for the final R6 gate.

The simulation uses the real retained 0.9.4 and current 0.9.5 payload binaries in a disposable staged-install root. It verifies both package signatures and all 323 manifest-listed payload hashes per candidate, then proves version/self-test behavior, settings, workspace files, two-project identity/isolation, legacy memory, project memory, and an unrelated LM Studio MCP configuration across upgrade, uninstall, and reinstall. It does not claim an actual MSIX upgrade on a second machine.

| Area | Accepted evidence | Status |
|---|---|---|
| Native backend | CLI, Manager, SessionHost, and WinUI GUI build in x64 Release. The complete configured Release matrix passes: 150/150 tests. | pass |
| Native GUI and telemetry | Exact 0.9.5 packaged GUI evidence covers the full page matrix, live telemetry, disconnect/reconnect, keyboard focus, High Contrast, and repaired 150% text. Presentation and Manager controller paths pass in the complete matrix. | pass under owner-approved simulated gate |
| Managed provider path | Exact 0.9.4 run `34c078f8-6256-4b03-80e1-936e2e50f2f4` rolled over to `4ddd93de-6cc6-413f-b5fd-90da72e074d8`, completed both effects, and returned `DONE`. The 80-tool managed-run regression passes with Release assertions active. | pass |
| Projects, MCP, memory, and tools | Registered 0.9.5 exposes 53 tools and passes project isolation, filesystem, search, Git, shell, legacy-memory, and project-memory restart checks. The simulated lifecycle retains the same state from real 0.9.4 to real 0.9.5 payloads. | pass |
| Settings and maintenance | Effective Manager/provider/shell/logging/session/context settings and transactional scoped resets are implemented and regression-covered. A non-default configuration survives simulated upgrade/uninstall/reinstall byte-for-byte. | pass |
| Installer and data | MSIX trust, preflight, registration, Start launch, WindowsApps identity, narrow unvirtualized profile contract, same-version reinstall, package signatures, and full payload rehashes pass. | pass |
| Legacy data | `%LOCALAPPDATA%\Forge Conductor` and its schema-9 files remain untouched. | deferred, nonblocking |

Current ignored distribution: `out/dist/candidate-0.9.5.0-20260913-165014/`.

- MSIX SHA-256: `3efd692af03e15b7d8e5dad95be8119563e08c158c48b6df1dbf26ad899578c9`
- ZIP SHA-256: `bd098a2b672c528ce17233212980464e45dc628a5d1957b33800a5694e22c098`
- application commit/tree: `dab23aa8555a37203ba11136c58bb7f799317356` / `a0b6b25eb93ca30495c0348b214c51f69cfedf78`
- package identity/version: `ForgeConductor.Windows.Alpha` / `0.9.5.0`
- signer thumbprint: `0AC803FF3292A2C736B1CBB31AFF88418983B992`

Reproduce the final lifecycle gate with `pwsh -NoProfile -File scripts/validation/Test-R6SimulatedInstalledLifecycle.ps1`. Generated evidence stays under ignored `out/validation`. Run the complete product matrix with `ctest --test-dir out/build/windows-msvc-x64 -C Release --output-on-failure`.

There are no known required Alpha blockers. Git delivery state and intentionally uncommitted `.forge-qwen/state/**` runtime files remain separate concerns.

<!-- alpha-phase-review:start -->
Phase review: R6 simulated acceptance closeout — 2026-09-13. Implementation and verification status: accepted Internal Alpha.
Delivery/merge status is recorded by the linked phase pull request.
<!-- alpha-phase-review:end -->
