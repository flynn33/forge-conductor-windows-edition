# Installed acceptance handoff

This handoff has two environments. Use `.\ForgeAlphaTest` or a dedicated Windows 11 x64 machine for the destructive lower-version/update/uninstall lifecycle. Use the owner's normal account for final 0.9.5 Start launch and everyday workflow with the fresh persistent `%LOCALAPPDATA%\Forge Conductor Internal Alpha` profile. Preserve `%LOCALAPPDATA%\Forge Conductor` and its schema-9 files untouched.

## Candidate identity

The current update candidate is the ignored local directory `out/dist/candidate-0.9.5.0-20260913-160324`:

- MSIX: `ForgeConductor-0.9.5.0-x64.msix`
- MSIX SHA-256: `157cc35181e1b940d353c38278047f522c0ac5e62bd12f24cee314417aa4b1cc`
- engineering ZIP SHA-256: `437d96e584c00dcf1995277e7a069d9ff665b16b45863ed2a7984adf616d49b5`
- signed application source commit/tree: `53901f110e9d7c5f360da65eb4419a37f4e24613` / `c14b60302688538f7d49fd0f77a20b73ecad2395`
- distribution source commit/tree: `53901f110e9d7c5f360da65eb4419a37f4e24613` / `c14b60302688538f7d49fd0f77a20b73ecad2395`
- package identity/version: `ForgeConductor.Windows.Alpha` / `0.9.5.0`
- publisher: `CN=Forge Conductor Alpha Development`
- signer/public-certificate thumbprint: `0AC803FF3292A2C736B1CBB31AFF88418983B992`
- public certificate SHA-256: `d8eef97d802a198d9e0fe65da6adcb6df71812e0b0947f756bf12c401340eec3`
- install helper SHA-256: `671c491b0cb9636652d15de6e0906ae1bd5436d7ec38375792179626c85aabbb`
- `README.txt` SHA-256: `ec7e09f4109ef3d22b7e6ba6d7b41d6ee10e967a764eddcec4e16d4972a18f7d`
- `distribution.json` SHA-256: `2e0dc106c5113c769de4aee23724134d4c8987bb1a40dc9a217975a08f533343`

The MSIX contains 324 verified payload files and the persistent-profile application change. Package creation, signing, unpacking, and every payload rehash passed. The current application change required a full rebuild. The package was then signed, unpacked, and verified against every recorded payload hash; schema-2 metadata binds the application and distribution to the committed source above. `Publisher.cer` contains no private key. No PFX or private signing material belongs in the handoff.

The retained lower candidate for the update test is `out/dist/candidate-0.9.4.0-20260913-115325/ForgeConductor-0.9.4.0-x64.msix`, SHA-256 `937e503c3198d829907aff2a349067ad8f21c7cec54df071d668a531eb65c586`. Its engineering ZIP SHA-256 is `2e0598163d582999f18d26f373cb49f23334b17425ac2277f1eee50912ed0403`.

## Required administrator action

Open PowerShell **as Administrator** and run this exact command:

```powershell
Import-Certificate -FilePath 'D:\GitHub\Forge-Conductor-Windows-Edition\out\dist\candidate-0.9.5.0-20260913-160324\Publisher.cer' -CertStoreLocation 'Cert:\LocalMachine\TrustedPeople'
```

Confirm the returned thumbprint is `0AC803FF3292A2C736B1CBB31AFF88418983B992`. This is the specific approval required before installation can proceed. The current non-elevated session established on September 13 that the thumbprint is absent at machine scope, the current account has no Forge package registration, an all-users package query is access denied, and local account `.\ForgeAlphaTest` does not exist. Current User trust is insufficient.

## Remaining installed checks

1. Provision the verified certificate with the command above and designate `.\ForgeAlphaTest`, another explicitly disposable account, or a dedicated Windows 11 x64 test machine.
2. Sign in to that environment and install retained candidate `out/dist/candidate-0.9.4.0-20260913-115325/ForgeConductor-0.9.4.0-x64.msix` first. Confirm package registration, Start launch, and that GUI, Manager, CLI, and SessionHost resolve to its registered payload.
3. In installed 0.9.4, register a disposable project; set a non-default setting; write and read a project-memory marker; add a persistence marker and foreign MCP entry; then close and reopen the GUI and confirm the Manager, project, settings, memory, and foreign entry survive.
4. From current candidate `candidate-0.9.5.0-20260913-160324`, run `Install-Engineering.ps1 -PreflightOnly` without the trust switch. Confirm `machine_trust: true`, `prior_version: 0.9.4.0`, and `ready_for_install: true`. Then run the helper without either switch and confirm its timestamped receipt records `prior_version` `0.9.4.0` and registered version `0.9.5.0`.
5. Launch updated 0.9.5 and exercise the native pages, Manager attachment, real telemetry, LM Studio MCP, representative filesystem/Git/shell tools, project memory, Settings and confirmed reset, and managed continuity. Verify all declared markers and the foreign MCP entry survived the upgrade. Uninstall in the test account, confirm registration and Start entry are gone while the disposable profile remains, reinstall 0.9.5, and verify the declared project, settings, memory, and foreign-entry state is recovered.
6. In the owner's normal account, install current 0.9.5, launch **Forge Conductor** from Start, and confirm the persistent header shows `Internal Alpha (persistent)` with data path `C:\Users\james\AppData\Local\Forge Conductor Internal Alpha`. Complete the essential project, LM Studio, MCP/tool, memory, managed-continuity, telemetry, Settings, reopen/reattach, and scoped-maintenance workflow. Close and reopen once more to confirm its projects, settings, and memory persist. Verify the legacy `C:\Users\james\AppData\Local\Forge Conductor` files remain untouched. Record registration output, exact executable paths, timestamped receipts, hashes, and screenshots throughout.

These checks close R6-G1. Legacy schema-9/C008/C009 migration is owner-deferred, nonblocking backlog and is not implemented, tested, or passed by this lifecycle.

<!-- alpha-phase-review:start -->
Phase review: R6 internal Alpha completion continuation — 2026-09-13. Implementation and verification status: [Product status](STATUS.md).
Delivery/merge status is recorded by the linked phase pull request.
<!-- alpha-phase-review:end -->
