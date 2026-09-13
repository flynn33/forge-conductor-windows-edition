# Installed acceptance handoff

This handoff is for an authorized Windows administrator or a dedicated Windows 11 x64 test machine. Use the dedicated local test account `.\ForgeAlphaTest`, or another account whose Forge profile is explicitly disposable. Do not use the owner profile under `C:\Users\james\AppData\Local\Forge Conductor`.

## Candidate identity

The current update candidate is the ignored local directory `out/dist/candidate-0.9.5.0-20260913-144427`:

- MSIX: `ForgeConductor-0.9.5.0-x64.msix`
- MSIX SHA-256: `9c772fcc9646f1e876f83c59c9e59a189f6f6881bcd603eb290dd589d5129a74`
- engineering ZIP SHA-256: `2874a1cdf51d6861e780a32b599172f9ca0d84fe9ad9a9bb5eb92b55e3bb8dd4`
- source commit: `c77c45386b25d7b76270c3685b79c172f41526c8`
- source tree: `2bb16d60c7616f3d6f31ec94c85192dfe30db349`
- package identity/version: `ForgeConductor.Windows.Alpha` / `0.9.5.0`
- publisher: `CN=Forge Conductor Alpha Development`
- signer/public-certificate thumbprint: `0AC803FF3292A2C736B1CBB31AFF88418983B992`
- public certificate SHA-256: `d8eef97d802a198d9e0fe65da6adcb6df71812e0b0947f756bf12c401340eec3`
- install helper SHA-256: `6a556425b5f559c722bd472a82b44767432efb8d7e2c3f453be945353efcef89`
- `distribution.json` SHA-256: `a4200ef80f49bb33ff0a8700e182dff1e4e141fffef489bf732d70475b14fa04`

The candidate contains 324 payload files. Package creation, unpacking, payload rehash, executable provenance, signer matching, and the exact CLI version check passed. `Publisher.cer` contains no private key. No PFX or private signing material belongs in the handoff.

The retained lower candidate for the update test is `out/dist/candidate-0.9.4.0-20260913-115325/ForgeConductor-0.9.4.0-x64.msix`, SHA-256 `937e503c3198d829907aff2a349067ad8f21c7cec54df071d668a531eb65c586`. Its engineering ZIP SHA-256 is `2e0598163d582999f18d26f373cb49f23334b17425ac2277f1eee50912ed0403`.

## Administrator prerequisite

Review the named development publisher and hashes, then import only the bundled public `Publisher.cer` into `Cert:\LocalMachine\TrustedPeople` through the organization-approved administrator process. The current non-elevated session established on September 13 that thumbprint `0AC803FF3292A2C736B1CBB31AFF88418983B992` is absent at machine scope, the current account has no Forge package registration, an all-users package query is access denied, and local account `.\ForgeAlphaTest` does not exist. Current User trust is insufficient.

After trust is provisioned and an explicitly disposable account is created or designated, sign in to that account and run `Install-Engineering.ps1 -PreflightOnly` from the candidate directory. Confirm `machine_trust: true`, the expected identity and version, and `ready_for_install: true`. Then run the helper without `-PreflightOnly` or `-TrustDevelopmentPublisher`. Preserve each generated `install-result-*.json` file with the acceptance evidence.

## Remaining installed checks

1. Install the retained 0.9.4.0 candidate into the clean test account. Confirm package registration, Start-menu launch, and that the running GUI, Manager, CLI, and SessionHost resolve to the registered package payload.
2. In the installed GUI, visit all native pages, start or attach to the Manager, register a disposable project, inspect real telemetry, deploy/inspect LM Studio MCP, invoke representative filesystem/Git/shell tools, write and read project memory, and exercise a confirmed Settings reset.
3. Add a disposable persistence marker and a foreign MCP entry. Close and reopen the GUI and verify the Manager/project/memory state survives.
4. Run the 0.9.5.0 helper from `candidate-0.9.5.0-20260913-144427` first with `-PreflightOnly` and then without either switch. Confirm the timestamped receipt records `prior_version` `0.9.4.0` and registered version `0.9.5.0`; a same-version reinstall does not satisfy the update gate.
5. Launch the updated package and repeat the essential Manager, project, memory, MCP, tool, Settings, and continuity checks. Verify the persistence marker and foreign MCP entry remain intact.
6. Uninstall with `Remove-AppxPackage` in the test account. Confirm package registration and Start entry are gone while `%LOCALAPPDATA%\Forge Conductor` remains.
7. Reinstall 0.9.5.0 and confirm the retained project, memory marker, settings, and foreign MCP state are recovered.
8. Record package registration output, exact executable paths, install-result JSON, hashes, and screenshots for install, Start launch, update, uninstall, retained data, and reinstall.

These checks close the machine-trust portion of R6-G1. Authentic central C008/C009 migration history is a separate dependency; do not point the candidate at the owner schema-9 database or claim schema compatibility from the disposable installed lifecycle.

<!-- alpha-phase-review:start -->
Phase review: R6 internal Alpha completion continuation — 2026-09-13. Implementation and verification status: [Product status](STATUS.md).
Delivery/merge status is recorded by the linked phase pull request.
<!-- alpha-phase-review:end -->
