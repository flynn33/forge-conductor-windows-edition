# Windows package inputs

`scripts/package.ps1` creates the Forge Conductor 1.0 Windows 11 x64 distribution from a clean, source-bound Release staging manifest.

The stable package identity is `ForgeConductor.Windows`. The package contains the self-contained WinUI application, CLI, Manager, SessionHost, Windows App SDK runtime, release Visual C++ runtime, resources, Forsetti manifest, third-party notices, embedded provenance, and a complete payload hash manifest.

Development signing is available only when `-DevelopmentSigning` is explicit. Production packaging requires a PFX supplied through parameters or the `FORGE_SIGNING_PFX` and `FORGE_SIGNING_PASSWORD` environment variables. No private key is copied into the distribution.

Supplying `-UpdateBaseUri <absolute-uri>` adds a versioned `ForgeConductor.appinstaller` that references the generated MSIX using the stable identity, publisher, architecture, and package version.

The production LocalAppData profile is narrowly excluded from package write virtualization so configuration, projects, memory, and continuity survive package updates and removal. `scripts/validation/Test-PackagePersistenceContract.ps1` enforces that manifest contract.
