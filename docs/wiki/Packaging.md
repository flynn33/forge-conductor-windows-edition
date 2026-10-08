# Current source version 1.3.24

Product version 1.3.24 and package version 1.3.24.0 are the current source targets. The 112-tool implementation is pending integrated build, signature, package/install, native-model, CI and release qualification. No 1.3.24 download or publication is asserted. The optional image provider is disabled by default and connects only to an explicitly configured existing local service; packaging does not install/start ComfyUI or download models. See [1.3.24 source notes](Release-1.3.24) and [local image-provider guide](https://github.com/flynn33/forge-conductor-windows-edition/blob/main/docs/IMAGE-PROVIDER.md).

## Superseded installed 1.3.22

The signed 1.3.22.0 installation matched all four executable hashes and preserved 8,431 files / 410,168,202 bytes plus the four protected snapshots before launch. Native automatic delivery remained incomplete and no 1.3.22 release was published. These checks do not qualify a 1.3.24 package. See [retained results](Release-1.3.22).

## Historical published 1.3.21

The signed 1.3.21.0 upgrade matched all four installed, staged and MSIX payload hashes and preserved the complete 8,375-file owner profile. Source tests, installed catalogs and selected native Qwen cases passed. [Version 1.3.21](https://github.com/flynn33/forge-conductor-windows-edition/releases/tag/v1.3.21) has been published with separate asset readback; these historical results remain bound to its original artifact. See [the measured 1.3.21 record](Release-1.3.21) for exact source and qualification limits.

## Historical published 1.3.19

Both native WinHTTP timeout diagnostics consistently report the selected receive budget, with unchanged timeout, deadline and cancellation behavior. The managed-context legacy-lease fix is retained. The exact 1.3.19 source passed 162/162 Release tests in 74.37 seconds, Product All, all three static gates and package persistence. Windows CI separately passed 162/162 in 126.04 seconds. Installed/staging/MSIX hashes matched all four executables, and actual catalogs returned 104/104/5. The existing selected Qwen conversation passed 9 bounded cases, including the formerly denied Host read and fresh sealed blind image analysis. The dedicated real-model managed recovery returned the exact seeded packet and unseen file in 2 captured native calls with verified sealed output. [Published release](https://github.com/flynn33/forge-conductor-windows-edition/releases/tag/v1.3.19) contains seven assets verified by size and SHA-256. [Windows package](https://github.com/flynn33/forge-conductor-windows-edition/releases/download/v1.3.19/ForgeConductor-1.3.19.0-x64.msix) and [installation bundle](https://github.com/flynn33/forge-conductor-windows-edition/releases/download/v1.3.19/ForgeConductor-1.3.19.0-x64.zip).

The development signer and exact four-payload source binding are in [the measured record](Release-1.3.19).

## Historical unpublished 1.3.18 candidate

Source `ffd10dce9125bf700ad5efbfd1228475704cc792`, tree `2781ec95847694ff4225471a8655fa64632bb11e`, passed **162/162 local Release tests in 78.61 seconds**, Product All, all three static gates and package persistence. [Windows CI 37657943054](https://github.com/flynn33/forge-conductor-windows-edition/actions/runs/37657943054) failed **161/162 in 115.37 seconds**: the sole failing WinHttp transport assertion required `receive_timeout_ms=1000`. CI did not print the returned timeout message, so its exact error branch remains unknown. Twenty unchanged-source isolated repetitions passed locally and do not override the CI failure.

The signed 1.3.18.0 upgrade matched all four installed/staging/MSIX payloads and preserved the complete **8,256-file, 401,730,716-byte** owner profile and protected snapshots. Eleven actual current-chat native calls succeeded across status, Host read, owned directory/write/readback, native Office and desktop observation/capture. The complete nine-case acceptance remained unfinished. A separate actual managed recovery returned the exact `found:true` packet and file contents, but the first probe failed its final narrative-format assertion and remains unqualified.

A final preservation readback verified all **1,199 original workspace files / 12,663,710 bytes**, Git status, the complete prior conversation prefix, owner configuration and selection, foreign MCP semantics and all **730 foreign plugin files**. Project-policy differences were limited to verified appended evaluation history; no project gate was executed or approved. The incomplete observations and failed assertions remain retained. No final release qualification, tag or publication followed.

See [the historical investigation](Release-1.3.18). These results do not qualify 1.3.19.

## Historical unpublished 1.3.17 candidate

Source `34e6e4c3f418c52522e4db33bf1d1dbb917abc46`, tree `c9a899ed0318af0d25d87c35f5c984c5b13eaa17`, passed 162/162 local Release tests in 84.41 seconds and [Windows CI 37646332085](https://github.com/flynn33/forge-conductor-windows-edition/actions/runs/37646332085) passed 162/162 in 91.69 seconds. Product All, all three static gates and package persistence passed. The signed 1.3.17.0 installation matched all four payloads and preserved 8,206 owner-profile files; automatic three-role repair completed in 8.019 seconds while preserving protected state and all 730 foreign plugin files. Managed-run context recovery then exposed a continuity-state lease defect. Current-chat attempts and the first image literal-check refusal remain retained, without final release qualification, a tag or publication. See [the historical investigation](Release-1.3.17). No result is reassigned to 1.3.18.

## Historical unpublished 1.3.16 candidate

Source `25d15f922744c464fdf660fd991f1cdfe1542323`, tree `1b23dfa072ea60e7da0f02dd4bd20f7028ac4dc9`, passed 162/162 local CTest entries in 78.19 seconds, all three static gates, package persistence and Product All. [Windows CI 37640187548](https://github.com/flynn33/forge-conductor-windows-edition/actions/runs/37640187548) completed successfully with 162/162 tests in 92.20 seconds and all three static gates. The 1.3.16.0 upgrade matched all four payload hashes and preserved the complete 7,991-file owner profile and protected snapshots. Explicit three-route repair completed in 62.857 seconds, with foreign MCP semantics and 730 foreign plugin files preserved. Original/current-chat workflow qualification and publication did not follow; a verified cold-child startup reporting issue requires the higher 1.3.17.0 candidate. See [the historical record](Release-1.3.16). These results are not reassigned to 1.3.17.

## Unpublished 1.3.15 investigation

The superseded 1.3.15.0 candidate from source `ce72c4fc8828864ba0af3f833198b620360cd34c`, tree `482d636a3884444f51599268574e19acf7dd16f7`, passed source/build/package checks and installed at 2026-10-07 13:51:43Z. Its MSIX SHA-256 is `fbbe559fc68d2a2432f654bd9f0bf7c66ed9904a9e527f4586aa3c1149028254`; ZIP SHA-256 is `49ce530db2ef1b1d36878b87b1e78538556c801409772a726e67c165a16b76fd`. Complete preservation was refused because an isolated test Manager changed production MCP routes before installation. It was not tagged or published and has no complete current-chat workflow qualification. See [the historical investigation](Release-1.3.15); none of these hashes identify the pending 1.3.17 package.

## Historical 1.3.14 engineering evidence

The unpublished 1.3.14 engineering candidates retain product version 1.3.14 and MSIX version 1.3.14.0. Their [investigation record](Release-1.3.14) identifies the actual source, package checksums and qualification scope. They were not tagged or published as a release, and their installed evidence does not qualify the final 1.3.17 package.

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
