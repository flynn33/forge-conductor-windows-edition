# Install Forge Conductor

Forge Conductor 1.3.16 (Windows package 1.3.16.0) targets Windows 11 x64. A distribution contains:

- `ForgeConductor-<version>-x64.msix`;
- `distribution.json` and payload hashes;
- `Publisher.cer` containing the public certificate only;
- `Install.ps1`;
- `README.txt`;
- optionally `ForgeConductor.appinstaller` when an update base URI was supplied.

## Verify and install

Open PowerShell in the extracted distribution:

```powershell
./Install.ps1 -PreflightOnly
./Install.ps1
```

Preflight checks the distribution's package and certificate hashes, signer, trust, and declared package identity/version against the currently installed version without installing. It rejects downgrades; it does not unpack the MSIX to independently inspect its manifest. Installation then verifies that Windows registered the declared package identity, version and publisher. Production certificates can validate through the normal Windows trust chain. Development-signed validation builds require an authorized administrator to trust the included public certificate in Local Machine Trusted People; the private key is never distributed.

Launch **Forge Conductor** from Start after installation. Ordinary use stores durable configuration, projects, memory, continuity, and view state under `%LOCALAPPDATA%\Forge Conductor`. Package removal does not purge that profile. Data deletion occurs only through explicitly confirmed maintenance actions.

LM Studio and a loaded tool-capable model are external prerequisites for inference features. The GUI, Manager, CLI, MCP server, local tools, project management, memory, settings, and telemetry are packaged together and do not require a development checkout.

After upgrading, select the intended registered project in Workspace, finish or stop active MCP calls and use **Settings → LM Studio plugins → Install or repair all three plugins**. Repair updates Primary, Fallback, and CLU to the selected project, current binary, shared data home, and 180-second request deadline while preserving foreign MCP registrations and unknown fields. Start a new model bootstrap or reconnect the integrations to receive current initialization instructions. Then choose **Open LM Studio and connect plugins** and select the intended integration in the chat.

Before upgrading or restarting the owning Manager, wait for tracked work to finish or request cancellation and confirm terminal status. Manager-backed jobs survive an MCP reconnect while their Manager remains running; reconnect and verify retained jobs with process_list/process_adopt. Standalone work remains connector-owned and stops on its owner shutdown. Receipts do not convert an interrupted unknown exit status into success.

For upgrades, install a higher package version with the same stable identity and publisher. The optional `.appinstaller` file checks for updates on launch and in the background.

## Current 1.3.16 distribution

The 1.3.16 candidate distribution and publication are pending. Its complete ZIP must contain the signed 1.3.16.0 MSIX, public publisher certificate, installer, metadata and checksums before installation is qualified. The local engineering packages use the existing development publisher. Inspect actual verification and provenance for the committed source, executable hashes, signature and qualification scope. See [candidate notes](releases/1.3.16.md).

The unpublished 1.3.15.0 candidate installed successfully, but its MCP preservation guard refused a routing change written before installation by an isolated test Manager. It is superseded without a qualified current-chat acceptance or release tag. See [the investigation](validation/HOST-CAPABILITIES-1.3.15.md). A higher 1.3.16.0 package keeps the ordinary Windows upgrade path; previous checks do not qualify its new binaries.

## Historical 1.3.11 publication and installed qualification

Download the complete ZIP from the [1.3.11 release](https://github.com/flynn33/forge-conductor-windows-edition/releases/tag/v1.3.11), extract it, and run the preflight above. The ZIP includes the signed MSIX, public publisher certificate, installer, and immutable `distribution.json`. That manifest identifies the exact tagged source commit/tree and MSIX hash. Inside the MSIX, `release-provenance.json` records all four executable hashes, and `payload-manifest.json` records every packaged file hash. `bundle-sha256.txt` verifies the ZIP.

The accepted host repair had already installed a development-signed 1.3.11.0 package and completed the [native qualification](validation/LM-STUDIO-REPAIR-1.3.11.md). The public package is rebuilt from the published source commit. Publication leaves that existing installation and LM Studio conversation unchanged; prior native-chat evidence remains tied to its original artifact. The installer rejects downgrades and calls Windows `Add-AppxPackage` for installation. Replacement of an existing 1.3.11.0 installation by this different same-version rebuild is not qualified here; preflight checks do not prove that replacement will occur.

## 1.3.5 release construction boundary

The host's installed ForgeConductor.Windows_1.3.4.0 package was preserved during construction; 1.3.5 verification used a separate Debug home. The new release does not add a credential, login step, model-instruction file, or fourth plugin. Native Auto Continuity verification bounds are stated in [release notes](releases/1.3.5.md).

## 1.3.6 installed verification

The authorized local repair installed the original signed `ForgeConductor.Windows_1.3.6.0_x64__wj2yg5ac9gadp` package. App and Manager were running from it; installed App, Manager, and CLI hashes matched its payload. Publication preserves that exact immutable package from source commit `9912541debc92c1117cceab0f6322c4e62728ee4`; later documentation commits do not change product source or replace the package bytes. Native-control initialization failed with MODULE_NOT_FOUND for kernel.js, so installed-window visual inspection is not claimed. See [1.3.6 release notes](releases/1.3.6.md).
