# Forge Conductor 1.3.29 release verification

The release version is **1.3.29**, with Windows package version **1.3.29.0**. This record separates the versioned release build and installation from the earlier feature qualification candidate.

## Rejected initial signed package

The initial signed 1.3.29.0 package from source `57c8c2a5e7496b66ee1cec420ae4fa396addec0e` was rejected before release publication. Its installed Manager crashed in bcrypt.dll during a completed-video status request. Investigation found that ComfyUI artifact hashing freed its caller-owned hash buffer before destroying the hash handle. The corrected source keeps the buffer alive through cleanup. The measurements below retain that initial candidate identity; corrected build, package, installation and publication results are pending.

The retained actual minidump records `bcrypt!BCryptDestroyHash+0x82`, exception `0xc0000005`, matching Application Error 1000's bcrypt offset `0x4f82`. A private relink using the original inputs has an identical complete `.text` section, allowing the first Manager return RVA `0x1b16f4` to be mapped to `ComfyDetail::fileFacts(HANDLE, OperationContext)+0xac4`. Its original cleanup calls sized `operator delete` before `BCryptDestroyHash`; subsequent stack frames pass through backend inspection and service snapshot/execute. The original caller-owned buffer lifetime violated [Microsoft's BCryptCreateHash contract](https://learn.microsoft.com/en-us/windows/win32/api/bcrypt/nf-bcrypt-bcryptcreatehash). The private dump-analysis evidence manifest has SHA-256 `d6f5bbb6e9312993fb7e564545e849ac6e65833c13379e36c91c9b0161811e24`.

The native regression guards only the calling thread's first exact CNG object-buffer allocation. Its isolated original-helper invocation reproduced access violation exit `-1073741819` (`0xc0000005`). The same test with the corrected helper passed empty-file and multi-chunk digests/byte counts plus cancellation cleanup with the unchanged `cancelled` result. This guard exists only in the backend test executable. The first compile attempt used a nonexistent cancellation constant in the new fixture; correcting it to the existing `ErrorCodes::Cancelled` contract enabled the before/after comparison. Neither the guarded regression nor dump analysis submits provider generation or changes a saved approval.

The versioned source is commit `57c8c2a5e7496b66ee1cec420ae4fa396addec0e`, tree `4b6a7e6be963ae722e6ffe8eec2727f8b81867b5`. Product and packaging inputs were clean and committed before building. The source, annotated `v1.3.29` tag, author, committer and tagger identify Jim Daley. This source was fast-forwarded into the primary local checkout and published to `main` through `flynn33`.

| Versioned check | Observed result |
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

| Release artifact | SHA-256 |
| --- | --- |
| `ForgeConductor-1.3.29.0-x64.msix` | `47d7120cfcb4e204d08d9eb7fbcc83706f8ebc63433498685f67d5c84460077e` |
| `ForgeConductor-1.3.29.0-x64.zip` | `ce6de99c5c468e579aead8e1914b8bd557312565eba466d4b5f7084e86bd2e61` |
| `payload-manifest.json` | `963951cd54f90dee5a583202ed748cab1babf579271d412a01a29fb034810baa` |

The independent audit observed 332 physical payload files including the manifest itself, and 336 signed-MSIX entries including four package metadata entries. The release payload manifest declares 331 content files. Those distinct counts are not interchangeable. The existing platform `RestartAgent.exe` is the sole additional packaged executable beyond the four Forge product executables.

## Installed upgrade and publication

The signed distribution's existing Windows PowerShell installation preflight passed with `ready_for_install:true`, prior package `1.3.28.0` and existing publisher trust. The actual installed upgrade, current live Settings/Rig readback and final seven-asset release readback are pending at this documentation snapshot.

The exact packaged source has a separate [Windows CI run 38049643594](https://github.com/flynn33/forge-conductor-windows-edition/actions/runs/38049643594), dispatched against `v1.3.29`; its final result is pending. The initial `main` push has [run 38049620393](https://github.com/flynn33/forge-conductor-windows-edition/actions/runs/38049620393). Documentation-only descendants do not change the packaged source identity.

## Feature qualification boundary

The earlier feature candidate passed Product All, all 177 configured Release tests, static gates and package persistence. Actual host observations include image, text-to-video and image-to-video previews, Registry custom-node installation, exact-job recovery after Manager interruption, and one approved final H.264 video. The final was 1280×704, 121 frames at 24 fps, approximately 5.04 seconds, with a measured provider execution time of 214.968 seconds. Its exact graph was submitted once. Independent sampled visual review and controlled playback completed; operator final quality acceptance remains unobserved. The separate longer preview was approximately 8.06 seconds. The image and image-to-video final approval paths have no actual operator-approved final result in this record.

See the [release notes](../releases/1.3.29.md), [ComfyUI automation guide](../COMFYUI-AUTOMATION.md) and [complete host qualification chronology](../COMFYUI_HOST_QUALIFICATION.md) for the contracts, measured cases, failed attempts and limits.

The existing hosted signing workflow requires repository secrets `FORGE_SIGNING_PFX_BASE64` and `FORGE_SIGNING_PASSWORD`. Inspection found neither configured. The tag-triggered [Signed Windows Release run 38049643395](https://github.com/flynn33/forge-conductor-windows-edition/actions/runs/38049643395) failed at **Require signing secrets**, before build, tests, packaging or upload. The distributed package was built and signed locally with the already installed development publisher certificate, whose public certificate is trusted on this host. No private key is distributed. This follows the preceding release's local signing path without adding an operator authentication method.
