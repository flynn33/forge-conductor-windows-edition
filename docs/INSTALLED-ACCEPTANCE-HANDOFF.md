# Installed acceptance handoff

The mandatory Internal Alpha acceptance pass is complete under the owner's explicit simulated-test allowance. This document retains the exact candidate identity and shows how to reproduce the final simulated lifecycle. An actual disposable-machine MSIX lifecycle is optional follow-up, not a release blocker.

## Candidate identity

Current candidate: `out/dist/candidate-0.9.5.0-20260913-165014`

- MSIX: `ForgeConductor-0.9.5.0-x64.msix`
- MSIX SHA-256: `3efd692af03e15b7d8e5dad95be8119563e08c158c48b6df1dbf26ad899578c9`
- engineering ZIP SHA-256: `bd098a2b672c528ce17233212980464e45dc628a5d1957b33800a5694e22c098`
- package identity/version: `ForgeConductor.Windows.Alpha` / `0.9.5.0`
- publisher: `CN=Forge Conductor Alpha Development`
- signer thumbprint: `0AC803FF3292A2C736B1CBB31AFF88418983B992`
- public certificate SHA-256: `d8eef97d802a198d9e0fe65da6adcb6df71812e0b0947f756bf12c401340eec3`
- application commit/tree: `dab23aa8555a37203ba11136c58bb7f799317356` / `a0b6b25eb93ca30495c0348b214c51f69cfedf78`

Retained baseline: `out/dist/candidate-0.9.4.0-20260913-115325/ForgeConductor-0.9.4.0-x64.msix`, SHA-256 `937e503c3198d829907aff2a349067ad8f21c7cec54df071d668a531eb65c586`.

`Publisher.cer` contains no private key. Private signing material must never be distributed.

## Accepted current-host evidence

The exact development publisher is trusted in Local Machine Trusted People. Normal-account preflight, registration, Start launch, WindowsApps GUI/Manager identity, durable external profile routing, and the installed 53-tool workflow pass. A same-version uninstall/reinstall preserved the durable project registry byte-for-byte, and the package-private virtualized profile stayed absent. Prior exact-package evidence covers the complete native page matrix, real telemetry, keyboard focus, High Contrast, repaired 150% text, disconnect/reconnect, and live-provider continuity.

## Reproduce the simulated lifecycle

From PowerShell 7 at the repository root:

```powershell
pwsh -NoProfile -File scripts/validation/Test-R6SimulatedInstalledLifecycle.ps1
```

The script:

1. validates both distribution identities, versions, package SHA-256 values, Authenticode signatures, payload-manifest hashes, and all 323 listed payload files;
2. stages the real 0.9.4 payload in a uniquely named disposable install root;
3. creates two project identities, a workspace file, legacy memory, project memory, a non-default configuration, and an unrelated LM Studio MCP sentinel outside the package root;
4. replaces only the guarded disposable package root with the real 0.9.5 payload and verifies all state plus the 53-tool catalog;
5. removes the guarded package root, proves external data is unchanged, reinstalls 0.9.5, and reopens the persisted memory.

The generated JSON receipt remains under ignored `out/validation/r6-simulated-lifecycle-*`. This proves the application/data contract with real candidate binaries; it does not claim AppX registration behavior on a second Windows machine.

Legacy schema-9/C008/C009 migration remains deferred and nonblocking. Do not point either candidate at `%LOCALAPPDATA%\Forge Conductor`; that preserved profile is outside first-Alpha scope.

<!-- alpha-phase-review:start -->
Phase review: R6 simulated acceptance closeout — 2026-09-13. Implementation and verification status: [Product status](STATUS.md).
Delivery/merge status is recorded by the linked phase pull request.
<!-- alpha-phase-review:end -->
