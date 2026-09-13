# Windows Alpha resume cursor

## Current state

- Worktree: `D:\GitHub\Forge-Conductor-Windows-Edition`; acceptance closeout branch `alpha/r6-simulated-acceptance-closeout` starts from merged main `c649e80f8f32b008b7f938cce6c822bf96cebfd7`.
- Forge Conductor 0.9.5 is accepted for Internal Alpha under the owner's explicit simulated-test allowance.
- Current candidate: `out/dist/candidate-0.9.5.0-20260913-165014`. MSIX SHA-256 `3efd692af03e15b7d8e5dad95be8119563e08c158c48b6df1dbf26ad899578c9`; ZIP SHA-256 `bd098a2b672c528ce17233212980464e45dc628a5d1957b33800a5694e22c098`.
- The full x64 Release build succeeds and all 150 configured tests pass.
- `scripts/validation/Test-R6SimulatedInstalledLifecycle.ps1` passes with the real retained 0.9.4 and current 0.9.5 payloads. Both signatures and all manifest-listed files validate; settings, workspace data, project isolation, legacy/project memory, and unrelated LM Studio MCP configuration survive simulated upgrade, uninstall, and reinstall.
- Registered 0.9.5 Start launch, WindowsApps GUI/Manager identity, durable profile routing, and the installed 53-tool workflow passed earlier and remain applicable.
- Exact-package GUI, accessibility, telemetry, disconnect/reconnect, and live-provider rollover evidence remains applicable. The owner accepted simulation for the final repetition; no second-machine MSIX lifecycle is claimed.
- Legacy schema-9/C008/C009 migration is deferred, nonblocking, and unimplemented. The old profile remains untouched.
- Unrelated `.forge-qwen/state/**` changes and evidence remain uncommitted and excluded.

## Next action

Deliver the tested closeout through the repository's normal pull-request and merge workflow, then fast-forward local `main` and confirm it matches `origin/main` and GitHub `main`. No functional or acceptance blocker remains.
