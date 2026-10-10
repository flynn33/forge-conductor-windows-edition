# Windows package inputs

Current source and packaging inputs identify Forge Conductor 1.3.29 / Windows package 1.3.29.0 x64. `scripts/package.ps1` constructs the distribution from a clean, source-bound Release staging manifest. Current versioned build, signing, payload, installation and publication results belong to [release verification](../docs/validation/RELEASE-1.3.29.md); see [release notes](../docs/releases/1.3.29.md). Earlier package measurements retain their artifact identities.

The stable package identity is `ForgeConductor.Windows`. The package contains the self-contained WinUI application, CLI, Manager, SessionHost, Windows App SDK runtime, release Visual C++ runtime, resources, Forsetti manifest, third-party notices, embedded provenance, and a complete payload hash manifest.

Development signing is available only when `-DevelopmentSigning` is explicit. Production packaging requires a PFX supplied through parameters or the `FORGE_SIGNING_PFX` and `FORGE_SIGNING_PASSWORD` environment variables. No private key is copied into the distribution.

Supplying `-UpdateBaseUri <absolute-uri>` adds a versioned `ForgeConductor.appinstaller` that references the generated MSIX using the stable identity, publisher, architecture, and package version.

The production LocalAppData profile is narrowly excluded from package write virtualization so configuration, projects, memory, and continuity survive package updates and removal. `scripts/validation/Test-PackagePersistenceContract.ps1` enforces that manifest contract.

The historical public 1.3.11 package is rebuilt from its tagged clean source commit and all four staged product hashes. VERSION, BUILD, CMake, the runtime identity, and both application manifests must agree before packaging. The publication verification record attached to the release is separate from the earlier accepted installed/native qualification. Publication does not install this new package. See [1.3.11 release notes](../docs/releases/1.3.11.md).

The original signed 1.3.6 package was constructed successfully and installed App/Manager/CLI hashes matched its payload. Publication preserves that exact immutable artifact from source commit `9912541debc92c1117cceab0f6322c4e62728ee4`, tree `be9e31450ac2792bb76cbaf409d90e7779f3046e`, with `v1.3.6` pointing to that commit. Later documentation commits remain on main without changing product source. The differently hashed later same-version rebuild is not the published package. These checks establish package identity, not installed-window visual acceptance. See [release notes](../docs/releases/1.3.6.md).

The published [1.3.29 release](https://github.com/flynn33/forge-conductor-windows-edition/releases/tag/v1.3.29) has seven assets verified by size and SHA-256, with clean packaged source `2e77c5fda1bad7309dd1e7f5a4048f230cc17579`. Its installed readback matches all 331 declared files. The [installation bundle](https://github.com/flynn33/forge-conductor-windows-edition/releases/download/v1.3.29/ForgeConductor-1.3.29.0-x64.zip) retains the existing public publisher certificate and installation helper. Later documentation commits retain that immutable tagged package source. Published 1.3.21 and recorded 1.3.28 results keep their historical identities; productive ComfyUI inference has a separate [host report](../docs/COMFYUI_HOST_QUALIFICATION.md).
