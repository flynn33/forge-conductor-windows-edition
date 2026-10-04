# Install Forge Conductor

Forge Conductor 1.3.6 targets Windows 11 x64. A distribution contains:

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

Preflight verifies the package hash, certificate hash, signer, package identity, trust, and update version without installing. Production certificates can validate through the normal Windows trust chain. Development-signed validation builds require an authorized administrator to trust the included public certificate in Local Machine Trusted People; the private key is never distributed.

Launch **Forge Conductor** from Start after installation. Ordinary use stores durable configuration, projects, memory, continuity, and view state under `%LOCALAPPDATA%\Forge Conductor`. Package removal does not purge that profile. Data deletion occurs only through explicitly confirmed maintenance actions.

LM Studio and a loaded tool-capable model are external prerequisites for inference features. The GUI, Manager, CLI, MCP server, local tools, project management, memory, settings, and telemetry are packaged together and do not require a development checkout.

After upgrading from 1.3.1 or earlier, finish or stop active MCP calls and use **Settings → LM Studio plugins → Install or repair all three plugins**. Repair updates Primary, Fallback, and CLU to the 180-second request deadline while preserving foreign MCP registrations and unknown fields. Then choose **Open LM Studio and connect plugins** and select the intended integration in the chat.

For upgrades, install a higher package version with the same stable identity and publisher. The optional `.appinstaller` file checks for updates on launch and in the background.

## 1.3.5 release construction boundary

The host's installed ForgeConductor.Windows_1.3.4.0 package was preserved during construction; 1.3.5 verification used a separate Debug home. The new release does not add a credential, login step, model-instruction file, or fourth plugin. Native Auto Continuity verification bounds are stated in [release notes](releases/1.3.5.md).

## 1.3.6 installed verification

The authorized local repair candidate installed `ForgeConductor.Windows_1.3.6.0_x64__wj2yg5ac9gadp`. App and Manager were running from that package; installed App, Manager, and CLI hashes matched that candidate payload. Final publication is rebuilt from the final release commit. Native-control initialization failed with MODULE_NOT_FOUND for kernel.js, so installed-window visual inspection is not claimed. See [1.3.6 release notes](releases/1.3.6.md).
