# Installer and installation
**Current R6 continuation candidate:** x64 Release MSIX creation, development signing, full payload validation, extraction, and rehash succeeded on September 12, 2026. The package is `ForgeConductor-0.9.3.0-x64.msix`, SHA-256 `9d9b899e7133cb9b46ac3f6221df5673e0bb7f55f1f92ef979d08ab77324607f`, from commit `0cd18ae1e7fd160e2fcbbe690552ef5c61a4a203` and tree `0ff125b00f51f7ad9ece314edbcbde387c69fdc2`.

The 0.9.3 candidate replaces the earlier retained bytes after live-continuity repairs. Windows still requires the included development publisher certificate in Local Machine Trusted People. Current-user trust is insufficient and no package was registered, so exact unpacked-package execution is recorded separately from installed acceptance. The owner store remains untouched, and a newer unsupported store produces an actionable non-destructive error.

Build with `scripts/build.ps1 -Configuration Release -Product All`, then `scripts/package.ps1 -DevelopmentSigning`.
Each distribution under `out/dist/candidate-*` contains the signed MSIX, public certificate, checksum metadata,
README, and `Install-Engineering.ps1`. Extract the engineering ZIP and run the helper in Administrator PowerShell
with `-TrustDevelopmentPublisher` to trust this specific signing certificate and install. No private key is exported.

## Distribution contract
Produce the versioned x64 Alpha ZIP emitted by `scripts/package.ps1`, containing:
- Signed `ForgeConductor-<numeric-version>-x64.msix` with the actual native GUI, CLI, Manager, SessionHost and assets.
- Public development certificate `.cer` and explicit trust instructions for an internal test build; never a PFX/private key.
- Required Microsoft runtime dependencies, when not self-contained, with a working local install path.
- `Install-Engineering.ps1`, a concise operator README, package/bundle checksums, and distribution metadata.
- Package checksum and concise build provenance (commit/configuration), without automated-authorship metadata.

Prefer a self-contained Windows App SDK deployment where compatible with the selected WinUI project, to reduce
runtime setup surprises. Verify the VC++ runtime and any framework package dependencies separately: "self-contained"
is not permission to omit files blindly. Do not silently install a model or require Node/Python/Visual Studio to launch.
LM Studio and an available tool-capable model are external prerequisites for model-dependent features.

## Packaging implementation
`scripts/package.ps1` uses MakeAppx and SignTool. It refuses dirty candidate inputs or a mismatched staging commit/tree, then verifies the signed package by extracting it and comparing each payload hash. The candidate contains the self-contained App SDK, redistributable release CRT, executable hashes and real native binaries. Actual installed workflow acceptance remains open under R6-G1 until the explicit machine trust action succeeds.
The real app is `ForgeConductorApp.exe`; sibling programs are `forge-conductor.exe`,
`ForgeConductor.Manager.exe`, `ForgeConductor.SessionHost.exe`, the Forsetti manifest and agent/resources used at runtime.
Verify actual target output names rather than blindly adopting this proposed staging list.

Choose a stable package identity and publisher once; make manifest Publisher match the signing certificate exactly.
The current candidate uses product version `0.9.3` and numeric MSIX version `0.9.3.0`, incremented from the retained `0.9.1.0` candidate so a real update path is available. Runtime identity and package scripts validate this source. Avoid downgrading an installed build.
Replace every manifest placeholder and include the required logos/resources. The supplied PNG assets are sufficient
starter packaging assets, not a UI design project.

Ensure the `forge-conductor.exe` app-execution alias actually dispatches the CLI, including `serve`, not just launches
the GUI. Use a separate CLI application/extension or an explicit native dispatcher supported by the chosen manifest.
Test stdout protocol cleanliness from the installed command. Do not save version-specific WindowsApps source paths
or development `out/build` paths into LM Studio. Deployment must resolve the current installed native entry reliably
and refresh owned configuration when upgrades change its path. Keep foreign configuration intact.

## Development signing is a functional prerequisite
MSIX signing and trust are required for installation even when security hardening is out of scope.
Create/reuse a local development certificate with the correct publisher subject, code-signing usage and private key
stored outside Git. Sign with the certificate store or explicitly provided signing credentials; never commit a PFX/password.
Export only the public certificate. Reuse the identity/certificate for an update test rather than generating a new
publisher every build. Production certificate services/Store signing are not Alpha prerequisites.

Installation helpers must explain the certificate they ask the operator to trust, request elevation only where Windows
requires it, and use supported certificate/package deployment commands. Do not globally disable execution policy,
Defender, SmartScreen, firewall or UAC to make a build appear installable.

## Required operator steps to publish after validation
1. Verify/extract the distribution. Trust its specific internal test certificate as documented, then install dependencies/MSIX.
2. Launch Forge Conductor from Start. A dependency problem must display a useful error, not silently exit.
3. Configure LM Studio and select a project in the app. Optional Git/PowerShell dependencies are discovered visibly.
4. For updates, stop only Forge-owned processes as needed, preserve data/configuration and install the higher version.
5. Uninstall through Windows or the supplied helper. User project data is preserved unless explicitly purged separately.

The final README should identify the accepted internal installer artifact after acceptance. Creating a public
release is not required; internal delivery is enough. Do not claim repair/auto-update/Store readiness without implementing it.

References checked September 10, 2026:
https://learn.microsoft.com/en-us/windows/apps/package-and-deploy/self-contained-deploy/deploy-self-contained-apps
https://learn.microsoft.com/en-us/windows/msix/package/signing-package-overview

<!-- alpha-phase-review:start -->
Phase review: R6 continuation — 2026-09-12. Implementation and verification status: [Product status](STATUS.md).
Delivery/merge status is recorded by the linked phase pull request.
<!-- alpha-phase-review:end -->
