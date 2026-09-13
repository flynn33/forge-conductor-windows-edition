# Windows Alpha resume cursor

## Current state

- Worktree: `D:\GitHub\Forge-Conductor-Windows-Edition`; branch `alpha/r6-internal-alpha-completion`; draft PR #25 targets `main`.
- Current signed application source: commit `53901f110e9d7c5f360da65eb4419a37f4e24613`, tree `c14b60302688538f7d49fd0f77a20b73ecad2395`.
- Current companion distribution source: commit `53901f110e9d7c5f360da65eb4419a37f4e24613`, tree `c14b60302688538f7d49fd0f77a20b73ecad2395`.
- Current candidate: `out/dist/candidate-0.9.5.0-20260913-160324`. MSIX SHA-256 `157cc35181e1b940d353c38278047f522c0ac5e62bd12f24cee314417aa4b1cc`; ZIP SHA-256 `437d96e584c00dcf1995277e7a069d9ff665b16b45863ed2a7984adf616d49b5`.
- Ordinary GUI launches now select `%LOCALAPPDATA%\Forge Conductor Internal Alpha`; the persistent header names it `Internal Alpha (persistent)` and shows the exact path. Its Manager lease, pipe, DPAPI storage, view state, projects, memory, and continuity are scoped away from `%LOCALAPPDATA%\Forge Conductor`.
- The full 91-test Infrastructure Unit suite, Release WinUI build, full Release product staging, signed package creation, extraction, and all 324 payload rehashes pass.
- `Install-Engineering.ps1` rejects `-PreflightOnly -TrustDevelopmentPublisher` before distribution access or side effects. Its focused regression records zero `Import-Certificate` and `Add-AppxPackage` calls. Ordinary preflight reaches the exact missing machine-trust boundary.
- The owner explicitly deferred legacy schema-9/C008/C009 migration for the first Internal Alpha. It is nonblocking backlog and is not implemented, tested, or passed. The old store and related files remain untouched.
- Unrelated `.forge-qwen/state/**` changes and evidence remain uncommitted and excluded.

## Exact next action

The owner opens PowerShell **as Administrator** and runs:

```powershell
Import-Certificate -FilePath 'D:\GitHub\Forge-Conductor-Windows-Edition\out\dist\candidate-0.9.5.0-20260913-160324\Publisher.cer' -CertStoreLocation 'Cert:\LocalMachine\TrustedPeople'
```

Confirm thumbprint `0AC803FF3292A2C736B1CBB31AFF88418983B992`. Then rerun current-candidate `Install-Engineering.ps1 -PreflightOnly` and install 0.9.5 in the normal account. Launch from Start and complete the persistent-profile operator workflow. Use the separately designated disposable environment for the retained 0.9.4 baseline, persistence markers, 0.9.5 update, uninstall, and reinstall sequence in [Installed acceptance handoff](../docs/INSTALLED-ACCEPTANCE-HANDOFF.md).

## Open dependency

- `B-R6-INSTALL-TRUST`: Local Machine Trusted People lacks the verified development publisher. The medium-integrity session cannot perform the exact import above. The current user has no Forge registration, the all-users query is access denied, and `.\ForgeAlphaTest` does not exist. Registered install/Start/update/uninstall/reinstall and final normal-account use remain unperformed.

R6-G2 and R6-G3 pass from real continuity and native visual/settings evidence. R6-G1 remains blocked and is not passed by preflight or packaging. Do not use the readiness wording until the registered lifecycle and final normal-account workflow pass. Keep Git ref equality separate from the intentionally dirty unrelated `.forge-qwen` working tree.
