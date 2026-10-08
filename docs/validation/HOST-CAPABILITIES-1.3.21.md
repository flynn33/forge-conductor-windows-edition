# Forge Conductor 1.3.21 verification

**Qualification pending.** Current source identity is 1.3.21 / Windows package 1.3.21.0, with a 106-tool Primary/Fallback catalog. No new 1.3.21 package has been installed or published yet.

The installed, unpublished 1.3.20.0 candidate was superseded after repeated native CMake builds exposed an MSBuild `FileTracker.InitializeCommonApplicationDataPaths` failure before the requested target. The 1.3.21 source adds the bounded `SystemDrive` default and strengthens the native known-folder, repeated-build and intended-failure regressions. Final 1.3.21 qualification remains pending; retained 1.3.20 attempts keep their original source and package identities.

Focused MCP catalog, adapter and retained original contract-matrix checks have passed during development. Native CMake/CTest, preview and continuity regressions are still being qualified. A development check does not establish final committed-source, installed or model acceptance.

The final record must bind the actual source commit/tree, complete source checks, four executable payload hashes, package signature and checksums, owner-state preservation, actual installed catalog, current-session native tool results and release readback. Failed and superseded development attempts remain retained under their original identities.

Published [1.3.19 verification](HOST-CAPABILITIES-1.3.19.md) remains historical evidence for its own source and artifacts. It does not substitute for 1.3.21 acceptance.
