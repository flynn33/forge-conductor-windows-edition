# Version 1.3.21

Current source version is **1.3.21**, with Windows package identity **1.3.21.0** and **106 Primary/Fallback tools**. All previous tools, ten specialist playbooks and five CLU tools remain.

The installed, unpublished 1.3.20.0 candidate was superseded after repeated native CMake builds exposed an MSBuild `FileTracker.InitializeCommonApplicationDataPaths` failure before the requested target. The 1.3.21 source adds the bounded `SystemDrive` default and strengthens the native known-folder, repeated-build and intended-failure regressions. Final 1.3.21 qualification remains pending; retained 1.3.20 attempts keep their original source and package identities.

Package, installed-tool and live-model qualification are pending. The [source release notes](https://github.com/flynn33/forge-conductor-windows-edition/blob/main/docs/releases/1.3.21.md) describe durable CMake/CTest jobs, accessibility paging, optional higher-resolution bounded previews, exact filled rectangle dimensions and observation-error recovery. The [verification record](https://github.com/flynn33/forge-conductor-windows-edition/blob/main/docs/validation/HOST-CAPABILITIES-1.3.21.md) will bind executed checks to the actual source and artifacts.

[Version 1.3.19](Release-1.3.19) retains the published historical measurements. Its results are not relabeled as 1.3.21 qualification.
