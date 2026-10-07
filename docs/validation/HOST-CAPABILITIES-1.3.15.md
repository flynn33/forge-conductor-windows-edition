# Unpublished 1.3.15 source/install investigation

This record is for the superseded product **1.3.15** / Windows MSIX **1.3.15.0** candidate, with 104 Primary/Fallback tools and five CLU tools. It was installed but was not tagged or published, and complete preservation/current-chat acceptance was not qualified. The current candidate is [1.3.16](HOST-CAPABILITIES-1.3.16.md), whose checks remain pending. Earlier [1.3.14 evidence](HOST-CAPABILITIES-1.3.14.md) retains its original source and installed identities.

## Executed source and package checks

Clean source `ce72c4fc8828864ba0af3f833198b620360cd34c`, tree `482d636a3884444f51599268574e19acf7dd16f7`, ran `scripts/test.ps1 -Configuration Release -Architecture x64 -Parallel 4`. Its log reports **162/162 CTest entries in 79.45 seconds**. All three static gates passed, the package-persistence contract reported production-profile exclusion, and Product All staged the native App and three sibling services. The exact logs are `capability-full-tests-1315-final-source.log`, `capability-static-gates-1315-final-source.log`, `capability-package-persistence-1315-final-source.log` and `capability-all-product-1315-final-source.log` under repository `out/verification/`. The source-bound [Windows CI run](https://github.com/flynn33/forge-conductor-windows-edition/actions/runs/37631065794) also completed successfully at 14:14:03Z. Green source/CI results did not establish absence of the external routing side effect discovered locally.

Distribution `release-1.3.15.0-20261007-134943` records that exact source/tree and a Valid development signature. MSIX SHA-256 is `fbbe559fc68d2a2432f654bd9f0bf7c66ed9904a9e527f4586aa3c1149028254`; ZIP SHA-256 is `49ce530db2ef1b1d36878b87b1e78538556c801409772a726e67c165a16b76fd`; payload-manifest SHA-256 is `7583a13f42efe697b3b12156ca8c83b75a461feb664e82d05baa6aac0822661a`. Its existing development signer thumbprint is `4B290FFFF895EAAC959DC343255F0DA043DC12A2`. `capability-package-1315-final-source.log` records the extracted-payload/provenance and signed distribution checks.

The install receipt records **2026-10-07 13:51:43Z**, upgrading 1.3.14.0 to 1.3.15.0 with the same identity and publisher. A subsequent read-only hash comparison of the four registered installed executable files matched the candidate's source-bound payload rows:

| Executable | Installed/payload SHA-256 |
|---|---|
| App | `50ff9783a04e4450f7585a0fa46fc6724a8bdb64f4326a7f7bafe0e592437a0b` |
| CLI | `0d4fdba9e4a4d90104f7564199da18576d3456ca30f80d6aeb5e8a9409f30dab` |
| Manager | `160225054fb9af0049f1ae5356fe37459a96bf1278fb1938d4b61d61d154e626` |
| SessionHost | `9ad05ed8c671496bf007286c88a30900c2e037569d0bf07fd3378bf91d0312ae` |

## Preservation refusal and routing side effect

The first verifier attempt stopped because its Python child inherited PowerShell 7 module paths and Windows PowerShell 5.1 failed to auto-load the signature module. An explicit import of the built-in Desktop Security module fixed that scratch capture path; the exact signature query then returned Valid with the matching signer/public certificate. No trust or package change was made for that repair.

The retry reached the protected-snapshot checks and refused: `Protected current/preupgrade snapshot bytes changed: mcp`. Actual readback found the original conversation, selected-chat configuration and owner Forge configuration byte-identical. The MCP file differed in exactly nine fields: the command, Forge home and deployment revision of each of the three Forge roles. All foreign server and top-level configuration semantics remained identical, and the foreign memory bridge matched its frozen registration.

The MCP file's modification time is **13:47:14Z**, before installation. A surviving disposable Manager profile log records `lmstudio_deploy_begin` at **13:47:10.647Z** and `lmstudio_deploy_complete` at **13:47:19.592Z**, with the exact new deployment revision found in those registrations. All nine files in the three generated Forge bridge directories were recreated at **13:47:25Z**, with bridge entries matching the changed MCP routes. These observations establish the isolated test Manager's production deployment during the suite; they do not attribute the change to the later MSIX installation. The previous native continuity and foreign plugin file timestamps showed no recent changes, but an exact previous bridge-tree byte inventory was unavailable.

The failure is retained rather than overridden by a passed installation or source suite. Correction of isolated Manager external-maintenance behavior and fresh production routing preservation are required for 1.3.16. Complete 1.3.15 current-chat workflow acceptance, blind-image analysis and release publication were not qualified.

## Subsequent authorized routing repair and limited status readback

The installed 1.3.15 App's ordinary repair-all-three action reported Committed at approximately 14:15:56Z. Actual before/after snapshots in `routing-repair-1315-retry1` retained identical conversation, selection and owner configuration bytes, all 730 foreign plugin file hashes and foreign MCP semantics. The before/after record SHA-256 values are `25f06a80bce2cccc87ab10eb8bf9a7460981cc6a8b5fddd5fce707e0e55e4ac2` and `373af49a5dd6458cd633707575a568818dff089eefec20810815a37778009a0b`. This explicit production repair is separate from the earlier unintended isolated deployment and does not erase that failure.

The existing Qwen chat subsequently returned actual Primary `get_forge_status` for version 1.3.15, 104 tools, the registered project, Host filesystem mode and an available durable Manager. Its transient CLI process was observed while alive at the installed path with the exact installed CLI hash and LM Studio Node parent. This single status read demonstrates restored routing; it does not establish the remaining workflow groups, blind-image recognition or qualification of the future 1.3.16 binaries.

## Earlier chat and preservation preparation

The actual existing Qwen conversation remains the acceptance target. Its original 1.3.13 status and denied external README read, subsequent 1.3.14 host access success, native Office exports, HTTP results, desktop observation, completed mutable worker and fresh read-only reviewer remain preserved in the private conversation history. The stock `image_read` description incorrectly reported a gray blank image, despite byte-identical PNG delivery. A fresh reviewer recognized the white background and the blue, red and green shapes, and an isolated direct image request independently recognized those pixels. Final acceptance must exercise the new dedicated analysis tool on a new blind fixture.

Before the final upgrade, the idle production App was closed normally. Two authenticated Manager inspections verified terminal sealed worker/reviewer/process receipts, no active operations and no future schedules. The supported shutdown request returned Manager exit code 0. An offline profile backup byte-verified 7,931 ordinary files totaling 385,900,828 bytes, with no reparse points; conversation, selection, owner configuration and MCP registration snapshots remained unchanged during that copy. These measurements qualify preservation preparation, not final installation.

Complete current-chat workflow acceptance was not qualified for this candidate. Final App native-chat/preference readback, all nine selected current-chat workflow groups, original reported shell/root/cwd cases, project-file preservation and release asset/readback checks must be measured separately for the new candidate. Each selected connector PID must have an actual observed creation time, executable path/hash and same-user parent process, bound to its native Primary trace. Raw chats and owner-profile data stay private; public qualification contains only explicitly selected facts/hashes/states.
