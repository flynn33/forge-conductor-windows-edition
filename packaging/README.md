# Windows package inputs

Current source and packaging inputs identify Forge Conductor 1.3.27 / Windows package 1.3.27.0 x64. `scripts/package.ps1` constructs a distribution from a clean, source-bound Release staging manifest. Integrated 1.3.27 build, complete tests, signed-package, installed-catalog and native LM Studio qualification are pending; isolated provider checks do not qualify these binaries. Publication readback remains a separate check; see [implementation notes](../docs/releases/1.3.27.md) and [measured verification](../docs/validation/HOST-CAPABILITIES-1.3.27.md). Published [1.3.19 evidence](../docs/releases/1.3.19.md) retains its own artifact identity.

The stable package identity is `ForgeConductor.Windows`. The package contains the self-contained WinUI application, CLI, Manager, SessionHost, Windows App SDK runtime, release Visual C++ runtime, resources, Forsetti manifest, third-party notices, embedded provenance, and a complete payload hash manifest.

Development signing is available only when `-DevelopmentSigning` is explicit. Production packaging requires a PFX supplied through parameters or the `FORGE_SIGNING_PFX` and `FORGE_SIGNING_PASSWORD` environment variables. No private key is copied into the distribution.

Supplying `-UpdateBaseUri <absolute-uri>` adds a versioned `ForgeConductor.appinstaller` that references the generated MSIX using the stable identity, publisher, architecture, and package version.

The production LocalAppData profile is narrowly excluded from package write virtualization so configuration, projects, memory, and continuity survive package updates and removal. `scripts/validation/Test-PackagePersistenceContract.ps1` enforces that manifest contract.

The historical public 1.3.11 package is rebuilt from its tagged clean source commit and all four staged product hashes. VERSION, BUILD, CMake, the runtime identity, and both application manifests must agree before packaging. The publication verification record attached to the release is separate from the earlier accepted installed/native qualification. Publication does not install this new package. See [1.3.11 release notes](../docs/releases/1.3.11.md).

The original signed 1.3.6 package was constructed successfully and installed App/Manager/CLI hashes matched its payload. Publication preserves that exact immutable artifact from source commit `9912541debc92c1117cceab0f6322c4e62728ee4`, tree `be9e31450ac2792bb76cbaf409d90e7779f3046e`, with `v1.3.6` pointing to that commit. Later documentation commits remain on main without changing product source. The differently hashed later same-version rebuild is not the published package. These checks establish package identity, not installed-window visual acceptance. See [release notes](../docs/releases/1.3.6.md).

The published 1.3.21 seven-asset readback passed separately. New 1.3.27 packaging remains pending and must be rebuilt from its final clean committed source and exact staging manifest; the isolated native provider smoke does not qualify that distribution.
