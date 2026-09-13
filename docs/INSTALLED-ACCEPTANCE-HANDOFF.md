# Installed acceptance handoff

This handoff is for an authorized Windows administrator or a dedicated Windows 11 x64 test machine. Use the dedicated local test account `.\ForgeAlphaTest`, or another account whose Forge profile is explicitly disposable. Do not use the owner profile under `C:\Users\james\AppData\Local\Forge Conductor`.

## Candidate identity

The current update candidate is the ignored local directory `out/dist/candidate-0.9.5.0-20260913-135703`:

- MSIX: `ForgeConductor-0.9.5.0-x64.msix`
- MSIX SHA-256: `78683d3cef440a190932b8f8cb0fe53b39a6a1ac2940a80c7e4da9e4309a8f22`
- engineering ZIP SHA-256: `6bb46f8992599a4d7da5729570011c76cf6cd0657857505ffbb19ca4737b8e91`
- source commit: `27c26e1401da72241cff68012ece2a8645f4e752`
- source tree: `e9995ac4ca6de0043fb1f293091f23720a434acd`
- package identity/version: `ForgeConductor.Windows.Alpha` / `0.9.5.0`
- publisher: `CN=Forge Conductor Alpha Development`
- signer/public-certificate thumbprint: `0AC803FF3292A2C736B1CBB31AFF88418983B992`
- public certificate SHA-256: `d8eef97d802a198d9e0fe65da6adcb6df71812e0b0947f756bf12c401340eec3`
- install helper SHA-256: `4d57e1044481e26187d69f55cc897779fc626127cd69c1860019b86c95d5ed3e`
- `distribution.json` SHA-256: `77907474952b77f76fa67ad266870668e1108e9f4767c162dfad47dcaf8f5d47`

The candidate contains 324 payload files. Package creation, unpacking, payload rehash, executable provenance, signer matching, and the exact CLI version check passed. `Publisher.cer` contains no private key. No PFX or private signing material belongs in the handoff.

The retained lower candidate for the update test is `out/dist/candidate-0.9.4.0-20260913-115325/ForgeConductor-0.9.4.0-x64.msix`, SHA-256 `937e503c3198d829907aff2a349067ad8f21c7cec54df071d668a531eb65c586`. Its engineering ZIP SHA-256 is `2e0598163d582999f18d26f373cb49f23334b17425ac2277f1eee50912ed0403`.

## Administrator prerequisite

Review the named development publisher and hashes, then import only the bundled public `Publisher.cer` into `Cert:\LocalMachine\TrustedPeople` through the organization-approved administrator process. The current non-elevated session already established that Current User trust is insufficient and Windows returns `0x800B0109`; do not use an elevation workaround.

After trust is provisioned, sign in as `.\ForgeAlphaTest` and run `Install-Engineering.ps1` from the candidate directory without `-TrustDevelopmentPublisher`. Preserve each generated `install-result-*.json` file with the acceptance evidence.

## Remaining installed checks

1. Install the retained 0.9.4.0 candidate into the clean test account. Confirm package registration, Start-menu launch, and that the running GUI, Manager, CLI, and SessionHost resolve to the registered package payload.
2. In the installed GUI, visit all native pages, start or attach to the Manager, register a disposable project, inspect real telemetry, deploy/inspect LM Studio MCP, invoke representative filesystem/Git/shell tools, write and read project memory, and exercise a confirmed Settings reset.
3. Add a disposable persistence marker and a foreign MCP entry. Close and reopen the GUI and verify the Manager/project/memory state survives.
4. Run the 0.9.5.0 helper from `candidate-0.9.5.0-20260913-135703` without the trust switch. Confirm the helper records `prior_version` `0.9.4.0` and registered version `0.9.5.0`; a same-version reinstall does not satisfy the update gate.
5. Launch the updated package and repeat the essential Manager, project, memory, MCP, tool, Settings, and continuity checks. Verify the persistence marker and foreign MCP entry remain intact.
6. Uninstall with `Remove-AppxPackage` in the test account. Confirm package registration and Start entry are gone while `%LOCALAPPDATA%\Forge Conductor` remains.
7. Reinstall 0.9.5.0 and confirm the retained project, memory marker, settings, and foreign MCP state are recovered.
8. Record package registration output, exact executable paths, install-result JSON, hashes, and screenshots for install, Start launch, update, uninstall, retained data, and reinstall.

These checks close the machine-trust portion of R6-G1. Authentic central C008/C009 migration history is a separate dependency; do not point the candidate at the owner schema-9 database or claim schema compatibility from the disposable installed lifecycle.

<!-- alpha-phase-review:start -->
Phase review: R2 telemetry parity follow-up — 2026-09-13. Implementation and verification status: [Product status](STATUS.md).
Delivery/merge status is recorded by the linked phase pull request.
<!-- alpha-phase-review:end -->
