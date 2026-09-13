# Windows Alpha resume cursor

## Current state

- Worktree: `D:\GitHub\Forge-Conductor-Windows-Edition`; branch `alpha/r6-internal-alpha-completion`; draft PR #25 targets `main`.
- Current signed application source: commit `dab23aa8555a37203ba11136c58bb7f799317356`, tree `a0b6b25eb93ca30495c0348b214c51f69cfedf78`.
- Current companion distribution source: commit `dab23aa8555a37203ba11136c58bb7f799317356`, tree `a0b6b25eb93ca30495c0348b214c51f69cfedf78`.
- Current candidate: `out/dist/candidate-0.9.5.0-20260913-165014`. MSIX SHA-256 `3efd692af03e15b7d8e5dad95be8119563e08c158c48b6df1dbf26ad899578c9`; ZIP SHA-256 `bd098a2b672c528ce17233212980464e45dc628a5d1957b33800a5694e22c098`.
- Ordinary GUI launches now select `%LOCALAPPDATA%\Forge Conductor Internal Alpha`; the persistent header names it `Internal Alpha (persistent)` and shows the exact path. Its Manager lease, pipe, DPAPI storage, view state, projects, memory, and continuity are scoped away from `%LOCALAPPDATA%\Forge Conductor`.
- The full 91-test Infrastructure Unit suite, Release WinUI build, full Release product staging, signed package creation, extraction, and all 324 payload rehashes pass.
- `Install-Engineering.ps1` rejects `-PreflightOnly -TrustDevelopmentPublisher` before distribution access or side effects. Its focused regression records zero `Import-Certificate` and `Add-AppxPackage` calls. Ordinary preflight and registered installation pass with the exact publisher trusted at machine scope.
- The current 0.9.5 package is registered. Start launches `ForgeConductorApp.exe` from its WindowsApps payload, the packaged Manager runs from the same payload with the durable Internal Alpha root, and installed MCP/tool/project/memory persistence checks pass. The package-private virtualized profile remains absent.
- The owner explicitly deferred legacy schema-9/C008/C009 migration for the first Internal Alpha. It is nonblocking backlog and is not implemented, tested, or passed. The old store and related files remain untouched.
- Unrelated `.forge-qwen/state/**` changes and evidence remain uncommitted and excluded.

## Exact next action

In a session with sanctioned native-app control, finish the current registered package's essential visual telemetry, Settings/reset, and managed-continuity walkthrough in the owner's normal account. Separately provision or sign in to a disposable Windows 11 x64 account or test machine, install retained candidate 0.9.4 first, create the required markers, upgrade to 0.9.5, then uninstall/reinstall and verify preservation. Follow [Installed acceptance handoff](../docs/INSTALLED-ACCEPTANCE-HANDOFF.md); do not downgrade the owner's normal account.

## Open dependency

- `B-R6-DISPOSABLE-LIFECYCLE`: the verified publisher is trusted and current 0.9.5 normal-account installation passes, but `.\ForgeAlphaTest` is not provisioned and no other disposable test environment is available. The retained 0.9.4→0.9.5 upgrade plus uninstall/reinstall lifecycle remains unperformed.
- `B-R6-NATIVE-UI-SURFACE`: the final package launches from Start and its registered paths are verified, but this session's sanctioned Computer Use surface exposes no native Windows applications. The final registered visual telemetry, Settings/reset, and managed-continuity walkthrough remains unperformed; prior exact-version native visual/accessibility evidence remains valid.

R6-G2 and R6-G3 pass from real continuity and native visual/settings evidence. R6-G1 remains blocked by the disposable lifecycle and is not passed by the successful normal-account install alone. Keep Git ref equality separate from the intentionally dirty unrelated `.forge-qwen` working tree.
