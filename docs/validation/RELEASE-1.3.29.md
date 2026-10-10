# Forge Conductor 1.3.29 release verification

The release version is **1.3.29**, with Windows package version **1.3.29.0**. This record separates the versioned release build and installation from the earlier feature qualification candidate.

## Registered-CLI cold-start correction

The replacement desktop bootstrap launches the current registered CLI with the fixed internal `--internal-launch-manager` command, selected profile mode and canonical home. That independent CLI creates only its matching Manager sibling with no inherited standard handles and closes its process handles. The connector retains its existing exact-home/version readiness checks. No operator settings, authentication method, tool schemas or generation paths change.

The focused Manager Windows infrastructure and real MCP process snapshot entries passed, in 5.55 and 83.61 seconds respectively (89.20 seconds total). New cases reject missing, extra and invalid launch arguments and wrong/nonexistent profiles, retain the previous stdin command's rejection contract, and verify the actual matching private-profile Manager survives its detached launcher. The existing connector kill-on-close job regression now exercises Shell → independent CLI → Manager and passed. This source correction still requires the complete source suite, replacement package and actual installed cold-start acceptance before publication.

At committed correction source `e6e97d3bbcac18712306954722b26edc31824ff3`, tree `be704dc1a9e2191c26da6728cfeae28e73d962e6`, Product All passed in 45.5114626 seconds. Its complete parallel-4 Release run passed 176/177 entries but the monolithic MCP process snapshot entry hit its unchanged 90-second CTest limit at 90.02 seconds. Total CTest time was 234.41 seconds. The test printed no intermediate output, so the interrupted case and exact cause remain unknown. Packaging did not run and this source was not installed.

An ignored diagnostic copy added only flushed function timing markers, retaining all 40,475 assertions and the 90-second limit. It passed in 80.26 seconds (80.28 seconds wall): core/catalog/context work and four core regressions reached 27.630 seconds, the five Manager regressions took 40.357 seconds, and final cleanup/scope destruction took 12.228 seconds. The new detached launch case took 1.057 seconds. This measures the aggregate cost without establishing which phase exceeded the limit in the earlier run. The original failure log and diagnostic source/log seals remain retained privately; timing instrumentation is excluded from tracked source and release artifacts.

The test registration now partitions the four core regressions and five Manager regressions into `ForgeConductor.Mcp.ServeProcessSnapshotTests --suite core` and `ForgeConductor.Mcp.ManagerLifecycleProcessTests --suite manager`. Shared catalog/context, owner-profile snapshots, synthetic fixtures and preservation checks remain unconditional in both. Each entry retains its 90-second limit, all assertions and individual deadlines; invoking the executable without a selector still runs the complete original case set. This produces 178 configured Release entries. The partition changes only CMake test registration and the test runner.

The two registrations passed together at parallel 4: core in 33.16 seconds with 37,806 assertions, Manager lifecycle in 60.18 seconds with 10,472 assertions, and 60.24 seconds wall time. The default complete invocation also passed under the same 90-second bound, in 80.36 seconds with the unchanged 40,475 assertions. The split totals include duplicated shared preservation checks. Four malformed selector/count cases failed before any assertions; the CTest inventory verified 178 entries and identical labels and bounds. The sealed suite-partition evidence has SHA-256 `ca745e5f524ae47cb3322cb5dbeaea882cb120226636aa345e954851446a2001`.

## Verified partition candidate and hosted absence-contract investigation

Clean source `7c639d25a880d11f82baea7aa5444a3739f45890`, tree `fbc742b49731d0dfdfb5f37a6aacbd6a21300c9a`, passed Product All in 35.0320929 seconds, all 178 Release entries in 223.12 seconds (245.8211992 seconds including the test script's build), three static gates in 9.3664953 seconds and package persistence in 0.5309187 seconds. Signing completed in 13.9883124 seconds. The independent distribution audit passed all 331 declared payload files, 332 physical files, 336 MSIX entries, five ZIP entries, four AMD64 products and eight ComfyUI resources. The audit SHA-256 is `f42d88ef1ab58d49b79c50a045fa9b14df76b018d55fc7d5d9a6caa20fd88699`; the signed MSIX is `0b33e56a01e887bcda6395df4e46bebb0afe2a9f3df8247245993bb5c5fb815a` and ZIP is `606cabeff6e9ec64279a7506ff49e5ca22a73f3980ace47fce3e1740839381e5`. This candidate was not installed or published while a separate hosted-test contract was investigated.

The earlier ed101 tag CI passed 176/177 entries, failing the connector-job-close test's `health.error().code==LimitExceeded` assertion at 72.88 seconds after 39,920 assertions. Its main CI passed 174/177: that assertion failed at 79.31 seconds, the native CMake known-folder probe exceeded its 15-second limit, and the ComfyUI service entry exceeded its 120-second limit. The health error and interrupted ComfyUI case were not printed, so their exact hosted causes remain unknown.

A disposable empty-profile native Manager probe separately reproduced `host_capability_unavailable` from `ManagerCompositionRoot::lmStudioUnavailableReason` through `UnavailableLmStudioDeploymentService`. Its corrected diagnostic passed all 1,698 assertions in 3.0622616 seconds, including exact error/message/retryability, matching Manager identity, detached launcher exit, cooperative shutdown and profile preservation. The first diagnostic expected a different downstream absence message and failed; both results remain retained. The existing job-close test permitted only a temporary limit error before its intended missing-desktop-configuration branch. That expectation contradicts the observed absence contract. The correction is confined to tests: assert the actual absence result when the desktop configuration is absent, retain strict health/path assertions when it exists, and retain every process identity, Job Object, survival, cleanup and profile-preservation assertion and deadline. A sixth Manager regression reuses the detached-launch fixture with a disposable empty profile, exercising this contract on configured hosts too. Unexpected test errors now print their actual code/message. These observations establish a test-contract defect; they do not identify the unprinted hosted error.

The corrected process registrations passed at parallel 4 under their unchanged 90-second limits: core in 31.55 seconds with 37,806 assertions and Manager lifecycle in 62.22 seconds with 10,607 assertions, 62.23 seconds total. The complete invocation and new clean-source Release pipeline are checked separately before installation/publication.

## Hash-fix candidate and packaged cold-start finding

The hash-fix signed candidate identifies clean committed source `ed101d77cd5adbbf809e7f36c7174d060613e6c4`, tree `0fd417128a05fd5bbe5e4c8aa47d4840a24726f7`. Its build, installed App path and completed-artifact recovery passed the checks below. Publication was withheld when a separate cold connector startup check failed. These measurements describe that retained candidate, not a subsequently published replacement. Author, committer and tagger are Jim Daley; publication uses `flynn33`.

| Corrected check | Observed result |
| --- | --- |
| Product All, Release x64, parallel 4, with excluded host-qualification target | Passed in 38.77025 seconds; all four product executables staged |
| Normal Release, parallel 4 | 177/177 passed in 222.94 seconds; all 177 individual result lines passed |
| Static gates | No-Python, native-stack and no-attribution passed |
| Package persistence | Stable production profile exclusion and package capability contract passed |
| Development-signed package | Passed in 13.6768192 seconds; signature `Valid` with the existing publisher |
| Independent offline payload audit | All 331 declared files matched size and SHA-256 on disk and inside streamed MSIX entries |
| Resource and executable coverage | Four AMD64 product executables matched staging/provenance; eight ComfyUI resources matched source, stages and committed content; qualification executables excluded |
| Distribution ZIP | Exactly five entries matched the distribution copies, including the corrected signed MSIX |

The corrected audit has SHA-256 `90a0f4b29998fb57d0e02e214fe1d31a42f28c9e4ad53f714c0a3421687942ae`. It also verifies 332 physical payload files including the manifest, 336 MSIX entries including four package metadata entries, Appx identity/version/architecture, publisher certificate and installer digests. The corrected Manager SHA-256 is `5d80bb4daa9700ae42f8486e888ea97fbb7478c86e01768fe9f09d70a75897a4`. Six subsequent wiki documentation edits were observed separately from the clean packaged source.

| Corrected artifact | SHA-256 |
| --- | --- |
| `ForgeConductor-1.3.29.0-x64.msix` | `0f62340d47a38aa89c959e4a81157a33e3aba30c5655f2d1dd82ba18811e8d00` |
| `ForgeConductor-1.3.29.0-x64.zip` | `edaf7b46740bd660c6ee6ffc3ca897c88b4fa3044fd9dbb931d6061d3cdedb43` |
| `payload-manifest.json` | `c45a8eb21311a420548bb10cd6ec000f18db94a15e5e4b96b4b8b758d3ad2180` |
| `release-provenance.json` | `9eb330935660396734b79bc877234faf50b08a5cec6506d508494a80cbd490f7` |

The existing publisher is `CN=Forge Conductor Development`, certificate thumbprint `4B290FFFF895EAAC959DC343255F0DA043DC12A2`. Signature trust is observed on this Windows host. The public certificate is bundled; the private key is not distributed.

The installed App-started acceptance verified all 331 payload files and eight ComfyUI resources, actual Primary/Fallback/CLU tool counts 125/125/5, and all 18 existing jobs through pages of 6, 9 and 3 records. Two completed-final status requests returned `completed` and two artifacts each without another generation POST. Manager remained alive and the provider log was byte-for-byte unchanged with handler count 10. Live Settings and Rig showed the managed runtime ready on the RTX 4090, with 18.3 GiB available at that observation. The compact installed acceptance evidence has SHA-256 `e0f3499a47b06ed2903826b506d9d03e2a3a5bdcd4449c9327ea672e22b071a1`. These checks submitted no generation and did not change saved approvals.

The separate cold installed MCP connection reported `durable_manager.available=false` after its desktop bootstrap helper exceeded the 10-second deadline. Actual helper dumps map the blocked call to `IShellDispatch2::ShellExecute` targeting the installed Manager executable. An unpackaged native caller reproduced the same target failure, while the byte-identical unpackaged CLI helper returned successfully in 0.131 seconds. Desktop Shell dispatch to the registered installed CLI succeeded in 63 ms, and its verified current-user execution alias succeeded in 47 ms. The internal Windows Shell reason remains unknown; caller package identity is not required to reproduce the failure. The bootstrap report has SHA-256 `c449d7a8c1c9bf8bd5d903095ca3e5b8bf591288fd0908eb6a602b2d02b5cc89`; its 34-file evidence manifest has SHA-256 `2877b978d7d565a59a8e6c285a6cb0c50a7e4cd434c237415556db3e8a4cf07d`. Existing Manager, ComfyUI, configuration and receipt seals were preserved. The release remains a draft while the registered-CLI bootstrap replacement is implemented and qualified.

## Rejected initial signed package

The initial signed 1.3.29.0 package from source `57c8c2a5e7496b66ee1cec420ae4fa396addec0e` was rejected before release publication. Its installed Manager crashed in bcrypt.dll during a completed-video status request. Investigation found that ComfyUI artifact hashing freed its caller-owned hash buffer before destroying the hash handle. The corrected source keeps the buffer alive through cleanup. The measurements below retain the rejected candidate identity and do not describe the corrected release artifacts.

The retained actual minidump records `bcrypt!BCryptDestroyHash+0x82`, exception `0xc0000005`, matching Application Error 1000's bcrypt offset `0x4f82`. A private relink using the original inputs has an identical complete `.text` section, allowing the first Manager return RVA `0x1b16f4` to be mapped to `ComfyDetail::fileFacts(HANDLE, OperationContext)+0xac4`. Its original cleanup calls sized `operator delete` before `BCryptDestroyHash`; subsequent stack frames pass through backend inspection and service snapshot/execute. The original caller-owned buffer lifetime violated [Microsoft's BCryptCreateHash contract](https://learn.microsoft.com/en-us/windows/win32/api/bcrypt/nf-bcrypt-bcryptcreatehash). The private dump-analysis evidence manifest has SHA-256 `d6f5bbb6e9312993fb7e564545e849ac6e65833c13379e36c91c9b0161811e24`.

The native regression guards only the calling thread's first exact CNG object-buffer allocation. Its isolated original-helper invocation reproduced access violation exit `-1073741819` (`0xc0000005`). The same test with the corrected helper passed empty-file and multi-chunk digests/byte counts plus cancellation cleanup with the unchanged `cancelled` result. This guard exists only in the backend test executable. The first compile attempt used a nonexistent cancellation constant in the new fixture; correcting it to the existing `ErrorCodes::Cancelled` contract enabled the before/after comparison. Neither the guarded regression nor dump analysis submits provider generation or changes a saved approval.

The rejected candidate's source was commit `57c8c2a5e7496b66ee1cec420ae4fa396addec0e`, tree `4b6a7e6be963ae722e6ffe8eec2727f8b81867b5`. Its product and packaging inputs were clean and committed before building. The annotated tag was updated to corrected source before publication while the GitHub release remained a draft; the prior tag object and all initial package/crash evidence were retained privately.

| Rejected candidate check | Observed result |
| --- | --- |
| Product All, Release x64, parallel 4, with the excluded host-qualification target | Passed in 117.8555904 seconds; all four product executables staged |
| Normal Release, parallel 4 | 177/177 passed in 216.55 seconds; all 177 individual result lines passed |
| Static gates | No-Python, native-stack and no-attribution passed |
| Package persistence | Stable production profile exclusion and package capability contract passed |
| CLI version and native self-test | `Forge Conductor 1.3.29 (Windows native)`; self-test passed |
| Development-signed package | Passed in 13.9219843 seconds; signature `Valid` with the existing publisher |
| Independent offline payload audit | All 331 declared files matched by size and SHA-256 in both the physical payload and streamed MSIX entries |
| Resource and executable coverage | Four AMD64 product executables matched staging/provenance; all eight ComfyUI resources matched source and both stages; qualification/test executables absent |
| Distribution ZIP | Exactly five bundled files, including the same signed MSIX, matched their outer distribution copies |

The commands were `scripts/build.ps1 -Configuration Release -Architecture x64 -Product All -Target ForgeConductor.NativeTools.ComfyUiHostQualification -Parallel 4`, `scripts/test.ps1 -Configuration Release -Architecture x64 -Parallel 4`, `scripts/Run-Static-Gates.ps1`, `scripts/validation/Test-PackagePersistenceContract.ps1` and `scripts/package.ps1 -DevelopmentSigning`. The qualification target is excluded from normal CTest and the shipped payload. CTest now printed `100% tests passed out of 177`; an initial evidence-capture parser expected its older comma-separated wording. Reading the completed log and counting all 177 passed result lines corrected the capture without rerunning tests.

The independent distribution audit is a separate read-only check, rather than a restatement of the packaging script. It verified all declared file sizes/digests, streamed MSIX and ZIP contents, clean source/tree identity, embedded provenance, Appx identity/version/architecture, the actual signature, public certificate, installer and README digests, committed resource content and starter output-node references. Its evidence SHA-256 is `12320d13891c049e927aef56d60456225600605a70b35238f09d2f712f71636c`.

| Rejected candidate artifact | SHA-256 |
| --- | --- |
| `ForgeConductor-1.3.29.0-x64.msix` | `47d7120cfcb4e204d08d9eb7fbcc83706f8ebc63433498685f67d5c84460077e` |
| `ForgeConductor-1.3.29.0-x64.zip` | `ce6de99c5c468e579aead8e1914b8bd557312565eba466d4b5f7084e86bd2e61` |
| `payload-manifest.json` | `963951cd54f90dee5a583202ed748cab1babf579271d412a01a29fb034810baa` |

The independent audit observed 332 physical payload files including the manifest itself, and 336 signed-MSIX entries including four package metadata entries. The release payload manifest declares 331 content files. Those distinct counts are not interchangeable. The existing platform `RestartAgent.exe` is the sole additional packaged executable beyond the four Forge product executables.

## Installed upgrade and publication

The signed distribution's existing Windows PowerShell installation preflight passed with `ready_for_install:true`, prior package `1.3.28.0` and existing publisher trust. The hash-fix candidate's actual installed upgrade and live Settings/Rig readback passed as recorded above. Its cold connector failure prevents publication; final replacement installation and seven-asset public release readback remain pending at this documentation snapshot.

The ed101 predecessor's [tag Windows CI run 38051004918](https://github.com/flynn33/forge-conductor-windows-edition/actions/runs/38051004918) and [main run 38051001350](https://github.com/flynn33/forge-conductor-windows-edition/actions/runs/38051001350) failed with the actual results recorded above. Initial-source [tag CI 38049643594](https://github.com/flynn33/forge-conductor-windows-edition/actions/runs/38049643594) and [main CI 38049620393](https://github.com/flynn33/forge-conductor-windows-edition/actions/runs/38049620393) were cancelled when replacement runs entered their concurrency groups; neither is recorded as a completed test pass.

## Feature qualification boundary

The earlier feature candidate passed Product All, all 177 configured Release tests, static gates and package persistence. Actual host observations include image, text-to-video and image-to-video previews, Registry custom-node installation, exact-job recovery after Manager interruption, and one approved final H.264 video. The final was 1280×704, 121 frames at 24 fps, approximately 5.04 seconds, with a measured provider execution time of 214.968 seconds. Its exact graph was submitted once. Independent sampled visual review and controlled playback completed; operator final quality acceptance remains unobserved. The separate longer preview was approximately 8.06 seconds. The image and image-to-video final approval paths have no actual operator-approved final result in this record.

See the [release notes](../releases/1.3.29.md), [ComfyUI automation guide](../COMFYUI-AUTOMATION.md) and [complete host qualification chronology](../COMFYUI_HOST_QUALIFICATION.md) for the contracts, measured cases, failed attempts and limits.

The existing hosted signing workflow requires repository secrets `FORGE_SIGNING_PFX_BASE64` and `FORGE_SIGNING_PASSWORD`. Inspection found neither configured. Corrected-source [Signed Windows Release run 38051004136](https://github.com/flynn33/forge-conductor-windows-edition/actions/runs/38051004136) failed at **Require signing secrets**, before build, tests, packaging or upload, as did the initial-source run. The distributed package was built and signed locally with the already installed development publisher certificate, whose public certificate is trusted on this host. No private key is distributed. This follows the preceding release's local signing path without adding an operator authentication method.
