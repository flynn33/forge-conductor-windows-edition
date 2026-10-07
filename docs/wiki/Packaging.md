# Current candidate 1.3.15

Product/package identities are 1.3.15 / 1.3.15.0. Final source/build, signed payload, installed App, current-chat qualification and publication remain pending until their actual checks execute. See [Release 1.3.15](Release-1.3.15) and the [verification record](https://github.com/flynn33/forge-conductor-windows-edition/blob/main/docs/validation/HOST-CAPABILITIES-1.3.15.md). Historical checks below apply only to the named earlier binaries; no result is reassigned.

## Historical 1.3.14 engineering evidence

The unpublished 1.3.14 engineering candidates retain product version 1.3.14 and MSIX version 1.3.14.0. Their [investigation record](Release-1.3.14) identifies the actual source, package checksums and qualification scope. They were not tagged or published as a release, and their installed evidence does not qualify the final 1.3.15 package.

Candidate `release-1.3.14.0-20261007-123639` is development-signed with signature status Valid and clean product source `7b3e154d58a2cc21d074541341e9ad4aaa0df914`, tree `0358d85e8281fb3b81032f1348b6fe789da41a20`. Product All and extracted-payload/provenance checks passed. MSIX SHA-256 is `a7019c34e88aeddbf6615fc0f8823ed9cb336e386700d06ea985a63a2c6160c6`; ZIP SHA-256 is `c6b555268c9d435a89107ba7b8e05912cecbcfe6a1a13e4348048b7308c95b37`. The App hash changed for the honest continuity wording; CLI, Manager and SessionHost hashes match the earlier source-767284a candidate. Installation/readback of the new App is pending, and earlier backend acceptance is kept separate from final package/UI qualification.

The records below describe historical artifacts and retain their original test counts and hashes.

# Packaging and updates

## Release 1.3.11

[Forge Conductor 1.3.11](https://github.com/flynn33/forge-conductor-windows-edition/releases/tag/v1.3.11) uses the stable `ForgeConductor.Windows` identity, product version `1.3.11`, and package version `1.3.11.0`. Use that release's distribution, provenance, checksum assets, and [release verification](https://github.com/flynn33/forge-conductor-windows-edition/releases/download/v1.3.11/release-verification.json) for its exact payload identity. The MSIX SHA-256 is `4022cd89978c1c0180c79f858ef37621a25eaa489f8d3518e77dfc4fef5edcb3`; the distribution ZIP SHA-256 is `dba790664e9c5f84c8e470c861f1d77594f2d61539adacbc30db4a564a336384`.

The publication is rebuilt after source [3a7306d947a248c421923c2eabc7e0dbc981a33f](https://github.com/flynn33/forge-conductor-windows-edition/commit/3a7306d947a248c421923c2eabc7e0dbc981a33f), tree `775961b3c6039beb2dc4da81495622c3449e244a`, was pushed to `main` with Jim Daley as author and committer. Product/runtime version, package version, and both application manifests are aligned; packaging rejects committed version drift and dirty product inputs before accepting staging. Release Product All, signature validation, exact payload/unpack checks, and the non-installing signed preflight passed. The fresh publication CLI passed 31 disposable feature calls and strict 58/58/5 role catalogs. The package was not installed by this publication workflow; the existing installed payload and original native conversation/configuration were preserved. Accepted native host checks identify the earlier installed clean-source artifact `5f938d93d2c80fb57fc39da65cac00a5f14b93c1`; those checks and hashes are not relabeled as new-package acceptance.

The native pipeline binds clean committed source inputs to four shipping executables, dependency closure, manifest identity, payload hashes, signature validation, and distribution metadata. The accepted installed executables had no PE `FileVersion` or `ProductVersion` resource; use runtime product version, Appx manifest identity, and source-bound hashes. Ordinary `%LOCALAPPDATA%\Forge Conductor` data remains separate from package-private state.

Historical [1.3.6](Release-1.3.6), [1.3.5](Release-1.3.5), and [1.3.5 Candidate](Release-1.3.5-Candidate) records identify their own artifacts. Their hashes and host observations do not identify the current publication package.

Native rollover qualification remains reserve-triggered with a live primary worker. The 1.3.11 repair checks do not add physical-exhaustion or interrupted UI-phase recovery qualification. See [Continuity](Continuity).

See [Release 1.3.11](Release-1.3.11), [Validation Gates](Validation-Gates), and the repository [installation instructions](https://github.com/flynn33/forge-conductor-windows-edition/blob/main/docs/INSTALL.md). The signed distribution includes a non-installing preflight; installation is a separate owner action. Same-version Windows package replacement was not exercised by publication verification.
