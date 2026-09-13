# Installed acceptance handoff

This handoff has two environments. Use `.\ForgeAlphaTest` or a dedicated Windows 11 x64 machine for the destructive lower-version/update/uninstall lifecycle. Use the owner's normal account for final 0.9.5 Start launch and everyday workflow with the fresh persistent `%LOCALAPPDATA%\Forge Conductor Internal Alpha` profile. Preserve `%LOCALAPPDATA%\Forge Conductor` and its schema-9 files untouched.

## Candidate identity

The current update candidate is the ignored local directory `out/dist/candidate-0.9.5.0-20260913-165014`:

- MSIX: `ForgeConductor-0.9.5.0-x64.msix`
- MSIX SHA-256: `3efd692af03e15b7d8e5dad95be8119563e08c158c48b6df1dbf26ad899578c9`
- engineering ZIP SHA-256: `bd098a2b672c528ce17233212980464e45dc628a5d1957b33800a5694e22c098`
- signed application source commit/tree: `dab23aa8555a37203ba11136c58bb7f799317356` / `a0b6b25eb93ca30495c0348b214c51f69cfedf78`
- distribution source commit/tree: `dab23aa8555a37203ba11136c58bb7f799317356` / `a0b6b25eb93ca30495c0348b214c51f69cfedf78`
- package identity/version: `ForgeConductor.Windows.Alpha` / `0.9.5.0`
- publisher: `CN=Forge Conductor Alpha Development`
- signer/public-certificate thumbprint: `0AC803FF3292A2C736B1CBB31AFF88418983B992`
- public certificate SHA-256: `d8eef97d802a198d9e0fe65da6adcb6df71812e0b0947f756bf12c401340eec3`
- install helper SHA-256: `671c491b0cb9636652d15de6e0906ae1bd5436d7ec38375792179626c85aabbb`
- `README.txt` SHA-256: `ec7e09f4109ef3d22b7e6ba6d7b41d6ee10e967a764eddcec4e16d4972a18f7d`
- `distribution.json` SHA-256: `d80022fc999f3817576db1d9a0674a5ce112004f1ad1cdabaaf2a854dfdb6736`

The MSIX contains 324 verified payload files, the persistent-profile application change, and the narrow manifest exclusion that keeps only `%LOCALAPPDATA%\Forge Conductor Internal Alpha` outside removable package-private storage. Package creation, signing, unpacking, and every payload rehash passed. The current application change required a full rebuild. The package was then signed, unpacked, and verified against every recorded payload hash; schema-2 metadata binds the application and distribution to the committed source above. `Publisher.cer` contains no private key. No PFX or private signing material belongs in the handoff.

The retained lower candidate for the update test is `out/dist/candidate-0.9.4.0-20260913-115325/ForgeConductor-0.9.4.0-x64.msix`, SHA-256 `937e503c3198d829907aff2a349067ad8f21c7cec54df071d668a531eb65c586`. Its engineering ZIP SHA-256 is `2e0598163d582999f18d26f373cb49f23334b17425ac2277f1eee50912ed0403`.

## Publisher trust and current-host result

Open PowerShell **as Administrator** and run this exact command:

```powershell
Import-Certificate -FilePath 'D:\GitHub\Forge-Conductor-Windows-Edition\out\dist\candidate-0.9.5.0-20260913-165014\Publisher.cer' -CertStoreLocation 'Cert:\LocalMachine\TrustedPeople'
```

The owner performed this action. Fresh verification found thumbprint `0AC803FF3292A2C736B1CBB31AFF88418983B992` in Local Machine Trusted People and confirmed that the public certificate bytes match the candidate. Ordinary read-only preflight passed, the current package installed, and Start launched `ForgeConductorApp.exe` from `C:\Program Files\WindowsApps\ForgeConductor.Windows.Alpha_0.9.5.0_x64__m6xqq279pd8q6`. Its packaged Manager runs from the same payload with `--alpha-root "C:\Users\james\AppData\Local\Forge Conductor Internal Alpha"`.

The registered 53-tool smoke passed two-project isolation, filesystem read/write, search, Git, shell, legacy-memory restart, and project-memory restart. A same-version uninstall/reinstall preserved `projects\registry.json` byte-for-byte. The final package did not create `Packages\ForgeConductor.Windows.Alpha_m6xqq279pd8q6\LocalCache\Local\Forge Conductor Internal Alpha`. These results prove the current normal-account install and durable storage behavior; they do not replace the lower-version upgrade lifecycle below. The legacy `%LOCALAPPDATA%\Forge Conductor` profile remained untouched.

The final registered visual telemetry, Settings/reset, and managed-continuity interaction remains open because this session's sanctioned Computer Use surface exposed no native Windows applications. Complete that walkthrough against the currently registered package when native-app control is available. Prior exact-version native page, keyboard, High Contrast, 150% text, disconnect/reconnect, and live-provider evidence remains applicable; Start activation or process identity alone does not replace the final interaction record.

## Remaining disposable lifecycle

1. An authorized administrator provisions the verified public publisher certificate and designates `.\ForgeAlphaTest`, another disposable account, or a dedicated Windows 11 x64 test machine.
2. Sign in to the designated environment and install retained candidate `out/dist/candidate-0.9.4.0-20260913-115325/ForgeConductor-0.9.4.0-x64.msix` first. Confirm package registration, Start launch, and that GUI, Manager, CLI, and SessionHost resolve to its registered payload.
3. In installed 0.9.4, register a disposable project; set a non-default setting; write and read a project-memory marker; add a persistence marker and foreign MCP entry; then close and reopen the GUI and confirm the Manager, project, settings, memory, and foreign entry survive.
4. From current candidate `candidate-0.9.5.0-20260913-165014`, run `Install-Engineering.ps1 -PreflightOnly` without the trust switch. Confirm `machine_trust: true`, `prior_version: 0.9.4.0`, and `ready_for_install: true`. Run the helper without either switch and confirm its timestamped receipt records `prior_version` `0.9.4.0` and registered version `0.9.5.0`.
5. Launch updated 0.9.5 and exercise the native pages, Manager attachment, real telemetry, LM Studio MCP, representative filesystem/Git/shell tools, project memory, Settings and confirmed reset, and managed continuity. Verify all declared markers and the foreign MCP entry survived the upgrade. Uninstall in the test account, confirm registration and Start entry are gone while the disposable profile remains, reinstall 0.9.5, and verify the declared project, settings, memory, and foreign-entry state is recovered.

Do not install current 0.9.5 before the retained lower candidate in that environment, and do not downgrade the owner's normal account. These checks close the remaining disposable portion of R6-G1. Legacy schema-9/C008/C009 migration is owner-deferred, nonblocking backlog and is not implemented, tested, or passed by this lifecycle.

<!-- alpha-phase-review:start -->
Phase review: R6 internal Alpha completion continuation — 2026-09-13. Implementation and verification status: [Product status](STATUS.md).
Delivery/merge status is recorded by the linked phase pull request.
<!-- alpha-phase-review:end -->
