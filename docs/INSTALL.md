# Install Forge Conductor

The measurement block below records historical 1.3.19 qualification; it does not qualify the current 1.3.29 release.

<!-- measured-qualification-1.3.19 -->

Both native WinHTTP timeout diagnostics consistently report the selected receive budget, with unchanged timeout, deadline and cancellation behavior. The managed-context legacy-lease fix is retained. The exact 1.3.19 source passed 162/162 Release tests in 74.37 seconds, Product All, all three static gates and package persistence. Windows CI separately passed 162/162 in 126.04 seconds. Installed/staging/MSIX hashes matched all four executables, and actual catalogs returned 104/104/5. The existing selected Qwen conversation passed 9 bounded cases, including the formerly denied Host read and fresh sealed blind image analysis. The dedicated real-model managed recovery returned the exact seeded packet and unseen file in 2 captured native calls with verified sealed output. [Published release](https://github.com/flynn33/forge-conductor-windows-edition/releases/tag/v1.3.19) contains seven assets verified by size and SHA-256. [Windows package](https://github.com/flynn33/forge-conductor-windows-edition/releases/download/v1.3.19/ForgeConductor-1.3.19.0-x64.msix) and [installation bundle](https://github.com/flynn33/forge-conductor-windows-edition/releases/download/v1.3.19/ForgeConductor-1.3.19.0-x64.zip).

<!-- /measured-qualification-1.3.19 -->

The current Forge Conductor source identity is 1.3.29 (Windows package 1.3.29.0), targeting Windows 11 x64. Use the existing upgrade path to preserve the stable package identity and per-user data. Versioned build, signing, payload, installation and publication results are recorded in [release verification](validation/RELEASE-1.3.29.md); older installed observations below retain their original versions. A distribution contains:

- `ForgeConductor-<version>-x64.msix`;
- `distribution.json` and payload hashes;
- `Publisher.cer` containing the public certificate only;
- `Install.ps1`;
- `README.txt`;
- optionally `ForgeConductor.appinstaller` when an update base URI was supplied.

## Current 1.3.29 installation

The source/package inputs identify 1.3.29 / 1.3.29.0 with 125 Primary/Fallback descriptors. Read [release verification](validation/RELEASE-1.3.29.md) for actual signed-package, installation, catalog and preservation results. Configure the existing local ComfyUI installation through Settings after upgrade; [automation setup](COMFYUI-AUTOMATION.md) describes defaults and preparation limits. Historical package results do not establish acceptance of the new package.

The current [published 1.3.29 installation bundle](https://github.com/flynn33/forge-conductor-windows-edition/releases/download/v1.3.29/ForgeConductor-1.3.29.0-x64.zip) contains the signed 1.3.29.0 MSIX and existing installation helper/certificate. This host's installed package matches all 331 declared files, and Forge has returned to its normal **Production** data store with its three integrations bound to the existing repository project. The ComfyUI qualification data folder and its saved jobs remain separate and preserved. **Isolated profile** is the App's label for an explicitly selected alternate Forge data folder; the normal startup uses **Production**.

[Published 1.3.21](https://github.com/flynn33/forge-conductor-windows-edition/releases/tag/v1.3.21) has separately verified seven-asset publication readback. Its installation and native qualification below remain historical evidence. The retained 1.3.28 installation verification required owner-profile preservation, exact four-image payload hashes and actual 112/112/5 catalogs before native provider acceptance. See [1.3.28 release notes](releases/1.3.28.md) and [recorded 1.3.28 verification](validation/HOST-CAPABILITIES-1.3.28.md).

The optional legacy image provider is disabled by default. Plugin installation does not start a server, download a checkpoint or enable its six image tools. Explicit owner configuration and actual image_provider_status availability are required; see [legacy provider setup](IMAGE-PROVIDER.md). The general ComfyUI automation card has separate installation and automatic-setup settings described in [automation setup](COMFYUI-AUTOMATION.md).

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

## Historical published 1.3.21 implementation

The signed 1.3.21.0 package upgraded the unpublished 1.3.20.0 candidate. All four installed, staged and MSIX executable hashes matched, the complete owner profile and protected snapshots were preserved, and installed catalogs and the selected native Qwen cases passed against the recorded source. Publication readback is a separate check.

The published 1.3.21 source and packaging inputs are aligned to 1.3.21 / 1.3.21.0. Clean-source, signed-package, installed-catalog and measured native-model qualification passed; the 1.3.21 verification record states the exact scope and limits. Publication readback remains a separate check. See [implementation notes](releases/1.3.21.md) and [measured qualification](validation/HOST-CAPABILITIES-1.3.21.md). Download links for the historical published package above retain their original version.

## Historical published 1.3.19 distribution

The qualified 1.3.19.0 distribution binds its signed MSIX, public publisher certificate, installer, metadata and checksums to the source and installed verification below. The local engineering packages use the existing development publisher. Inspect actual verification and provenance for the committed source, executable hashes, signature and qualification scope. See [release notes](releases/1.3.19.md).

The installed, unpublished [1.3.18 investigation](validation/HOST-CAPABILITIES-1.3.18.md) passed its local 162-entry suite and exact four-payload/profile upgrade checks, but its Windows CI failed one timeout-message assertion (161/162). Eleven current-chat native calls succeeded; the complete nine-case acceptance and the first managed-recovery final-format check did not qualify the release. These observations remain bound to their original artifact. Its exact installed identity is historical; the final 1.3.19.0 higher-version upgrade and exact payload checks are recorded separately above.

## Earlier unpublished upgrade investigations

The paragraphs below retain their stage-local candidate references. Their earlier hashes do not identify the current package.

The installed, unpublished [1.3.17.0 investigation](validation/HOST-CAPABILITIES-1.3.17.md) passed its signed upgrade, exact four-payload/profile preservation and automatic three-route repair. It was superseded after the managed-run context-recovery lease defect was observed during current-chat testing. The new 1.3.18.0 candidate requires its own higher-version upgrade and exact source/payload/current-chat checks; previous successes are not reassigned.

The unpublished [1.3.16.0 candidate](validation/HOST-CAPABILITIES-1.3.16.md) installed successfully with exact four-executable and offline owner-profile preservation. Its subsequent explicit repair registered the three routes while preserving foreign MCP semantics and all 730 foreign plugin files. It was superseded before original/current-chat qualification or publication; the new 1.3.18.0 candidate requires its own higher-version upgrade and exact payload checks.

The unpublished 1.3.15.0 candidate installed successfully, but its MCP preservation guard refused a routing change written before installation by an isolated test Manager. It is superseded without a qualified current-chat acceptance or release tag. See [the investigation](validation/HOST-CAPABILITIES-1.3.15.md). A higher 1.3.18.0 package keeps the ordinary Windows upgrade path; previous checks do not qualify its new binaries.

## Historical 1.3.11 publication and installed qualification

Download the complete ZIP from the [1.3.11 release](https://github.com/flynn33/forge-conductor-windows-edition/releases/tag/v1.3.11), extract it, and run the preflight above. The ZIP includes the signed MSIX, public publisher certificate, installer, and immutable `distribution.json`. That manifest identifies the exact tagged source commit/tree and MSIX hash. Inside the MSIX, `release-provenance.json` records all four executable hashes, and `payload-manifest.json` records every packaged file hash. `bundle-sha256.txt` verifies the ZIP.

The accepted host repair had already installed a development-signed 1.3.11.0 package and completed the [native qualification](validation/LM-STUDIO-REPAIR-1.3.11.md). The public package is rebuilt from the published source commit. Publication leaves that existing installation and LM Studio conversation unchanged; prior native-chat evidence remains tied to its original artifact. The installer rejects downgrades and calls Windows `Add-AppxPackage` for installation. Replacement of an existing 1.3.11.0 installation by this different same-version rebuild is not qualified here; preflight checks do not prove that replacement will occur.

## 1.3.5 release construction boundary

The host's installed ForgeConductor.Windows_1.3.4.0 package was preserved during construction; 1.3.5 verification used a separate Debug home. The new release does not add a credential, login step, model-instruction file, or fourth plugin. Native Auto Continuity verification bounds are stated in [release notes](releases/1.3.5.md).

## 1.3.6 installed verification

The authorized local repair installed the original signed `ForgeConductor.Windows_1.3.6.0_x64__wj2yg5ac9gadp` package. App and Manager were running from it; installed App, Manager, and CLI hashes matched its payload. Publication preserves that exact immutable package from source commit `9912541debc92c1117cceab0f6322c4e62728ee4`; later documentation commits do not change product source or replace the package bytes. Native-control initialization failed with MODULE_NOT_FOUND for kernel.js, so installed-window visual inspection is not claimed. See [1.3.6 release notes](releases/1.3.6.md).
