# Windows Alpha resume cursor

## Current state

- Worktree: `D:\GitHub\Forge-Conductor-Windows-Edition`; branch `alpha/r6-internal-alpha-completion`; draft PR #25 targets `main`.
- Current signed application source: commit `44b0369a0a7d7456b1a696a37450f29463555b02`, tree `390fe7b20b61233c39f12cd657ad0075a3e8c2f8`.
- Current companion distribution source: commit `2c34eb7e99554faad4059dc3082e160b0f32e415`, tree `64fc6f9946627f7ab4f485e7eca42ceb1550d42c`.
- Current candidate: `out/dist/candidate-0.9.5.0-20260913-153641`. MSIX SHA-256 `692bbc20230f58a9c0b8ed1619aa85cdbb634d67450533bf9657c3bba56ceeb5`; ZIP SHA-256 `c93a4b42545a4d9a1ea3c8ebc373db77f6c81d367c14f366c94b15a32b02ca86`.
- Ordinary GUI launches now select `%LOCALAPPDATA%\Forge Conductor Internal Alpha`; the Manager page names it `Internal Alpha (persistent)` and shows the exact path. Its Manager lease, pipe, DPAPI storage, view state, projects, memory, and continuity are scoped away from `%LOCALAPPDATA%\Forge Conductor`.
- The full 89-test Infrastructure Unit suite, Release WinUI build, full Release product staging, signed package creation, extraction, and all 324 payload rehashes pass.
- `Install-Engineering.ps1` rejects `-PreflightOnly -TrustDevelopmentPublisher` before distribution access or side effects. Its focused regression records zero `Import-Certificate` and `Add-AppxPackage` calls. Ordinary preflight reaches the exact missing machine-trust boundary.
- The owner explicitly deferred legacy schema-9/C008/C009 migration for the first Internal Alpha. It is nonblocking backlog and is not implemented, tested, or passed. The old store and related files remain untouched.
- Unrelated `.forge-qwen/state/**` changes and evidence remain uncommitted and excluded.

## Exact next action

The owner opens PowerShell **as Administrator** and runs:

```powershell
Import-Certificate -FilePath 'D:\GitHub\Forge-Conductor-Windows-Edition\out\dist\candidate-0.9.5.0-20260913-153641\Publisher.cer' -CertStoreLocation 'Cert:\LocalMachine\TrustedPeople'
```

Confirm thumbprint `0AC803FF3292A2C736B1CBB31AFF88418983B992`. Then rerun current-candidate `Install-Engineering.ps1 -PreflightOnly` and install 0.9.5 in the normal account. Launch from Start and complete the persistent-profile operator workflow. Use the separately designated disposable environment for the retained 0.9.4 baseline, persistence markers, 0.9.5 update, uninstall, and reinstall sequence in [Installed acceptance handoff](../docs/INSTALLED-ACCEPTANCE-HANDOFF.md).

## Open dependency

- `B-R6-INSTALL-TRUST`: Local Machine Trusted People lacks the verified development publisher. The medium-integrity session cannot perform the exact import above. The current user has no Forge registration, the all-users query is access denied, and `.\ForgeAlphaTest` does not exist. Registered install/Start/update/uninstall/reinstall and final normal-account use remain unperformed.

R6-G2 and R6-G3 pass from real continuity and native visual/settings evidence. R6-G1 remains blocked and is not passed by preflight or packaging. Do not use the readiness wording until the registered lifecycle and final normal-account workflow pass. Keep Git ref equality separate from the intentionally dirty unrelated `.forge-qwen` working tree.
