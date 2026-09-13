# Installed acceptance handoff

This handoff is for an authorized Windows administrator or a dedicated Windows 11 x64 test machine. Use the dedicated local test account `.\ForgeAlphaTest`, or another account whose Forge profile is explicitly disposable. Do not use the owner profile under `C:\Users\james\AppData\Local\Forge Conductor`.

## Candidate identity

The current update candidate is the ignored local directory `out/dist/candidate-0.9.4.0-20260913-115325`:

- MSIX: `ForgeConductor-0.9.4.0-x64.msix`
- MSIX SHA-256: `937e503c3198d829907aff2a349067ad8f21c7cec54df071d668a531eb65c586`
- engineering ZIP SHA-256: `2e0598163d582999f18d26f373cb49f23334b17425ac2277f1eee50912ed0403`
- source commit: `3fb70143ef2db9704c8a395b49a29857bd087976`
- source tree: `fad39f10bdb2638a74a0b28deb0c0ecc1e88dede`
- package identity/version: `ForgeConductor.Windows.Alpha` / `0.9.4.0`
- publisher: `CN=Forge Conductor Alpha Development`
- signer/public-certificate thumbprint: `0AC803FF3292A2C736B1CBB31AFF88418983B992`
- public certificate SHA-256: `d8eef97d802a198d9e0fe65da6adcb6df71812e0b0947f756bf12c401340eec3`
- install helper SHA-256: `4d57e1044481e26187d69f55cc897779fc626127cd69c1860019b86c95d5ed3e`
- `distribution.json` SHA-256: `3b450890bf78f8193ad6cbadd014e7627db2c52b168685df085192730ac84bf4`

The candidate contains 324 payload files. Package creation, unpacking, payload rehash, executable provenance, signer matching, and the exact CLI version check passed. `Publisher.cer` contains no private key. No PFX or private signing material belongs in the handoff.

The retained lower candidate for the update test is `out/dist/candidate-0.9.3.0-20260913-033258/ForgeConductor-0.9.3.0-x64.msix`, SHA-256 `9d9b899e7133cb9b46ac3f6221df5673e0bb7f55f1f92ef979d08ab77324607f`. Its engineering ZIP SHA-256 is `dbef5788b6bd2bc91d03ee9228db02e7a9605bfdadbc66ed9e55e58329aa2721`.

## Administrator prerequisite

Review the named development publisher and hashes, then import only the bundled public `Publisher.cer` into `Cert:\LocalMachine\TrustedPeople` through the organization-approved administrator process. The current non-elevated session already established that Current User trust is insufficient and Windows returns `0x800B0109`; do not use an elevation workaround.

After trust is provisioned, sign in as `.\ForgeAlphaTest` and run `Install-Engineering.ps1` from the candidate directory without `-TrustDevelopmentPublisher`. Preserve each generated `install-result-*.json` file with the acceptance evidence.

## Remaining installed checks

1. Install the retained 0.9.3.0 candidate into the clean test account. Confirm package registration, Start-menu launch, and that the running GUI, Manager, CLI, and SessionHost resolve to the registered package payload.
2. In the installed GUI, visit all native pages, start or attach to the Manager, register a disposable project, inspect real telemetry, deploy/inspect LM Studio MCP, invoke representative filesystem/Git/shell tools, write and read project memory, and exercise a confirmed Settings reset.
3. Add a disposable persistence marker and a foreign MCP entry. Close and reopen the GUI and verify the Manager/project/memory state survives.
4. Run the 0.9.4.0 helper from `candidate-0.9.4.0-20260913-115325` without the trust switch. Confirm the helper records `prior_version` `0.9.3.0` and registered version `0.9.4.0`; a same-version reinstall does not satisfy the update gate.
5. Launch the updated package and repeat the essential Manager, project, memory, MCP, tool, Settings, and continuity checks. Verify the persistence marker and foreign MCP entry remain intact.
6. Uninstall with `Remove-AppxPackage` in the test account. Confirm package registration and Start entry are gone while `%LOCALAPPDATA%\Forge Conductor` remains.
7. Reinstall 0.9.4.0 and confirm the retained project, memory marker, settings, and foreign MCP state are recovered.
8. Record package registration output, exact executable paths, install-result JSON, hashes, and screenshots for install, Start launch, update, uninstall, retained data, and reinstall.

These checks close the machine-trust portion of R6-G1. Authentic central C008/C009 migration history is a separate dependency; do not point the candidate at the owner schema-9 database or claim schema compatibility from the disposable installed lifecycle.

<!-- alpha-phase-review:start -->
Phase review: R6 acceptance closeout — 2026-09-13. Implementation and verification status: [Product status](STATUS.md).
Delivery/merge status is recorded by the linked phase pull request.
<!-- alpha-phase-review:end -->
