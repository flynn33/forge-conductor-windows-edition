# Current release 1.3.14

Use [Release 1.3.14](Release-1.3.14) and the [distribution](https://github.com/flynn33/forge-conductor-windows-edition/releases/tag/v1.3.14) for the current source, package identity, checksums, and actual qualification. Product version is 1.3.14 and MSIX version is 1.3.14.0. Current qualification is tied to the exact executed build, signed package and payload hashes; earlier installed evidence is retained below.

The records below describe historical artifacts and retain their original test counts and hashes.

# Packaging and updates

## Release 1.3.11

[Forge Conductor 1.3.11](https://github.com/flynn33/forge-conductor-windows-edition/releases/tag/v1.3.11) uses the stable `ForgeConductor.Windows` identity, product version `1.3.11`, and package version `1.3.11.0`. Use that release's distribution, provenance, checksum assets, and [release verification](https://github.com/flynn33/forge-conductor-windows-edition/releases/download/v1.3.11/release-verification.json) for its exact payload identity. The MSIX SHA-256 is `4022cd89978c1c0180c79f858ef37621a25eaa489f8d3518e77dfc4fef5edcb3`; the distribution ZIP SHA-256 is `dba790664e9c5f84c8e470c861f1d77594f2d61539adacbc30db4a564a336384`.

The publication is rebuilt after source [3a7306d947a248c421923c2eabc7e0dbc981a33f](https://github.com/flynn33/forge-conductor-windows-edition/commit/3a7306d947a248c421923c2eabc7e0dbc981a33f), tree `775961b3c6039beb2dc4da81495622c3449e244a`, was pushed to `main` with Jim Daley as author and committer. Product/runtime version, package version, and both application manifests are aligned; packaging rejects committed version drift and dirty product inputs before accepting staging. Release Product All, signature validation, exact payload/unpack checks, and the non-installing signed preflight passed. The fresh publication CLI passed 31 disposable feature calls and strict 58/58/5 role catalogs. The package was not installed by this publication workflow; the existing installed payload and original native conversation/configuration were preserved. Accepted native host checks identify the earlier installed clean-source artifact `5f938d93d2c80fb57fc39da65cac00a5f14b93c1`; those checks and hashes are not relabeled as new-package acceptance.

The native pipeline binds clean committed source inputs to four shipping executables, dependency closure, manifest identity, payload hashes, signature validation, and distribution metadata. The accepted installed executables had no PE `FileVersion` or `ProductVersion` resource; use runtime product version, Appx manifest identity, and source-bound hashes. Ordinary `%LOCALAPPDATA%\Forge Conductor` data remains separate from package-private state.

Historical [1.3.6](Release-1.3.6), [1.3.5](Release-1.3.5), and [1.3.5 Candidate](Release-1.3.5-Candidate) records identify their own artifacts. Their hashes and host observations do not identify the current publication package.

Native rollover qualification remains reserve-triggered with a live primary worker. The 1.3.11 repair checks do not add physical-exhaustion or interrupted UI-phase recovery qualification. See [Continuity](Continuity).

See [Release 1.3.11](Release-1.3.11), [Validation Gates](Validation-Gates), and the repository [installation instructions](https://github.com/flynn33/forge-conductor-windows-edition/blob/main/docs/INSTALL.md). The signed distribution includes a non-installing preflight; installation is a separate owner action. Same-version Windows package replacement was not exercised by publication verification.
