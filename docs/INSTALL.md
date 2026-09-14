# Install Forge Conductor

Forge Conductor 1.0 targets Windows 11 x64. A distribution contains:

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

For upgrades, install a higher package version with the same stable identity and publisher. The optional `.appinstaller` file checks for updates on launch and in the background.
