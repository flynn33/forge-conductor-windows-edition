# Forge Conductor for Windows

The measurement block below records historical 1.3.19 qualification; it does not qualify the current 1.3.21 implementation.

<!-- measured-qualification-1.3.19 -->

Both native WinHTTP timeout diagnostics consistently report the selected receive budget, with unchanged timeout, deadline and cancellation behavior. The managed-context legacy-lease fix is retained. The exact 1.3.19 source passed 162/162 Release tests in 74.37 seconds, Product All, all three static gates and package persistence. Windows CI separately passed 162/162 in 126.04 seconds. Installed/staging/MSIX hashes matched all four executables, and actual catalogs returned 104/104/5. The existing selected Qwen conversation passed 9 bounded cases, including the formerly denied Host read and fresh sealed blind image analysis. The dedicated real-model managed recovery returned the exact seeded packet and unseen file in 2 captured native calls with verified sealed output. [Published release](https://github.com/flynn33/forge-conductor-windows-edition/releases/tag/v1.3.19) contains seven assets verified by size and SHA-256. [Windows package](https://github.com/flynn33/forge-conductor-windows-edition/releases/download/v1.3.19/ForgeConductor-1.3.19.0-x64.msix) and [installation bundle](https://github.com/flynn33/forge-conductor-windows-edition/releases/download/v1.3.19/ForgeConductor-1.3.19.0-x64.zip).

<!-- /measured-qualification-1.3.19 -->

## Measured 1.3.21 qualification

Clean source `39adf553df320120d214bd90cc49223359054ede`, tree `c1b295cae2c0a01ce334526d99af6bc3e7b3a243`, passed **163/163 Release CTest entries in 67.14 seconds**, Product All, all three static gates and package persistence. The signed 1.3.21.0 package matched all four installed/staged/MSIX executable hashes; the offline owner-profile comparison preserved **8,375 files / 407,592,939 bytes** and the four protected snapshots. Installed Primary/Fallback/CLU catalogs returned **106/106/5** tools. The existing selected Qwen conversation supplied **13 verified native cases** across pixel receipts, CMake/CTest, desktop paging, independent image analysis and expected native errors. The separate original authority probe verified **7 cases / 63 actual requests** without model inference.

The complete source suite also exercised the native CMake fixture's Windows PowerShell 5.1 known-folder check, repeated build on the initialized tree and exact marker before the intentional failed target. These source-fixture checks are separate from the selected installed/native-model cases.

Continuity source fixtures passed **118 Infrastructure groups**, including the observer reconstruction regression and **20 durability cases**. Observer reconstruction was exercised within one PID using private native conversation files and injected control receipts. These results do not qualify installed observer/UI interruption, an installed connector rollover, physical context exhaustion or already-running agent reattachment. The historical 1.3.20 cross-process checkpoint primitive evidence is not asserted as a current 1.3.21 check.

Stock LM Studio MCP image metadata remains distinct from model image input and independent analysis. Higher-resolution previews and native RGBA samples do not establish exact OCR or change that stock bridge. Generative image providers and cloud accounts require separately configured services. Windows CI passed for the recorded 1.3.21 source, as linked in the verification record. Publication readback is a separate check.



Forge Conductor is a native Windows 11 workspace and MCP tool server for project work with local models in LM Studio.

The installed, unpublished 1.3.20.0 candidate was superseded after repeated native CMake builds exposed an MSBuild `FileTracker.InitializeCommonApplicationDataPaths` failure before the requested target. The 1.3.21 source adds the bounded `SystemDrive` default and strengthens the native known-folder, repeated-build and intended-failure regressions. Measured 1.3.21 source, package, installed-catalog and native-model checks passed; retained 1.3.20 attempts keep their original source and package identities.

The current source version is **1.3.21**, with **106 tools** in Primary and Fallback, ten specialist playbooks and CLU's five governance tools. LM Studio models can use dedicated web search/fetch/HTTP, native DOCX/XLSX/PPTX creation, Windows desktop observation and input, PNG drawing, image previews and independent image analysis, native CMake/CTest jobs, independent model workers, and persistent model-task schedules. Owner-selected host filesystem access covers ordinary local volumes while relative paths keep the selected project as their default. The narrower workspace mode remains selectable. Clean-source, signed-package, installed-catalog and measured native-model qualification passed; the 1.3.21 verification record states the exact scope and limits. See the [capability guide](docs/HOST-CAPABILITIES.md), [1.3.21 release notes](docs/releases/1.3.21.md) and [measured verification record](docs/validation/HOST-CAPABILITIES-1.3.21.md). Published 1.3.19 evidence remains historical above.

Native Office files and PNG drawings require neither Office nor Python. Scheduled model tasks require Manager to stay running; future schedules restore after restart, while interrupted runs with uncertain effects require explicit authorization before another attempt. Generative artwork and cloud email, calendar, or chat accounts require separately configured providers or authorized APIs. `host_capabilities` reports the implemented tools, current filesystem policy, and these connection requirements.

`image_analyze` starts an authorized independent read-only vision run from a local image. Poll `reviewer_status` for its actual output, errors and token usage. `image_read`, `image_write`, `desktop_capture` and `image_analyze` accept optional `preview_max_dimension` 128–2,048, default 256; adaptive resizing retains the 512 KiB base64-encoded preview bound and reports final dimensions and reduction metadata. Invalid preview parameters are rejected before write/capture effects. LM Studio 0.4.25's stock MCP result stores image metadata rather than a model image file part. The independent analysis route uses Forge's Responses image path and retains the three existing integrations; larger previews do not change that stock bridge boundary or guarantee exact OCR.

`desktop_read` supports zero-based `offset` and `next_offset`/`has_more` paging, with observed row indices from each fresh accessibility tree and a 64 KiB encoded native response bound. `cmake_test_run` and `cmake_test_status` use an explicit initialized build tree and the existing durable job/log/wait/kill lifecycle. Inspect separate phase outcomes and nullable validated JUnit counts; a successful status read is not test success. See the [CMake/CTest guide](docs/CMAKE-CTEST.md) for request examples, shared deadlines, report integrity and crash recovery.

Schedule lists and mutation receipts provide bounded summaries. Retrieve complete task text, frozen scope, history, logs and notification receipts with `schedule_list` using `schedule_id`; concatenate its UTF-8 JSON pages and carry the returned `revision` on continuation requests. A changed revision requires starting the read again.

## Product surfaces

- **Workspace:** project/provider selection, ordered instruction packages, CLU development-policy governance, Auto Continuity, and readiness.
- **Rig:** live system, selected LM Studio chat context, model, storage, process, and workflow observations; **Ensure manager**, **Restart service** and **Stop service** controls in Runtime configuration.
- **Continuity:** saved packet list, selected packet details, delete-selection, and clear actions.
- **Activity:** operational outcomes, governance findings, corrections, notifications, and exported evidence.
- **Settings:** effective configuration, context capacity and reserves, and saved-record actions. Load, save/readback, revert, Test LM Studio, and Restart Manager remain available.

Sessions are LM Studio chats. There is no replacement run manager. Removed controls and callable actions include Managed Run/readback, Export selected project, Import verify first, the Actions frame and Invoke Tool, Advanced Canonical Catalog, Scope Test, Reset Scope, Apply and Verify, and the previous Data Maintenance scheme. Saved project records remain selectable and deletable through buttons.

`get_forge_status` reports the authoritative project ID/folder and binding source, instruction-package paths in execution order, development-policy source, tool names/count, and agent count. `instruction_package.read` retrieves a selected queue row with paged content. Packages remain project-scoped; Forge's application-data home is not a substitute for the bound project.

Choose **Choose policy folder...** under CLU to bind the selected repository immediately. The remaining controls name their actions: reload policies, inspect status/findings, read a document/next part, and export the CLU log. The model is instructed to read and follow the bound policy. Findings are returned as model tool-result notifications and shown through the CLU findings UI and Activity.

Auto Continuity observes the selected LM Studio conversation's actual generation usage. At reserve pressure it waits for a completed tool boundary, requests a detailed model-written packet, creates a new native LM Studio chat through Windows UI Automation, sends the packet, and verifies packet retrieval plus a following Forge tool result. Primary owns rollover; Fallback retains the general catalog and CLU retains governance tools. Install or repair remains one action for `forge-conductor`, `forge-conductor-fallback`, and `forge-conductor-clu`; no fourth plugin is added. No Forge credential or login step is introduced.

Auto Continuity was verified with a reserve-triggered pause, not physical context exhaustion. Rollover was verified while the primary MCP worker stayed alive. Interrupted handoff after idle-process eviction is not durable and is not claimed.

Ordinary launches use `%LOCALAPPDATA%\Forge Conductor`; disposable verification uses a separate home. The historical `--alpha-root <absolute-path>` option remains available for isolated profiles.

## Build and verify

Requirements and commands are in [Build](docs/BUILD.md) and [Testing](docs/TESTING.md):

```powershell
./scripts/build.ps1 -Configuration Release -Architecture x64 -Product All
./scripts/build.ps1 -Configuration Release -Architecture x64 -Product Backend
./scripts/test.ps1 -Configuration Release -Architecture x64
./scripts/Run-Static-Gates.ps1
```

Current implementation and qualification limits are recorded in the [1.3.21 notes](docs/releases/1.3.21.md) and [measured capability verification record](docs/validation/HOST-CAPABILITIES-1.3.21.md). Clean-source, signed-package, installed-catalog and measured native-model qualification passed; the 1.3.21 verification record states the exact scope and limits. Each qualified distribution records its exact commit, tree, MSIX hash and payload-manifest hash. The [published 1.3.19 release notes](docs/releases/1.3.19.md) and [historical verification record](docs/validation/HOST-CAPABILITIES-1.3.19.md) retain their own artifact identity.

Historical 1.3.19 checks are recorded separately in the measured qualification above. The installed, unpublished [1.3.18 investigation](docs/validation/HOST-CAPABILITIES-1.3.18.md) passed its local 162-entry suite and exact four-payload/profile upgrade checks, but its Windows CI failed one timeout-message assertion (161/162). Eleven current-chat native calls succeeded; the complete nine-case acceptance and the first managed-recovery final-format check did not qualify the release. These observations remain bound to their original artifact.

The installed, unpublished [1.3.17 investigation](docs/validation/HOST-CAPABILITIES-1.3.17.md) passed its exact source suite and CI, signed upgrade and automatic three-route repair. Current-chat testing then exposed the managed-run context-recovery lease defect; its attempts remain retained without final release qualification, a tag or publication. The unpublished [1.3.16 candidate](docs/validation/HOST-CAPABILITIES-1.3.16.md) passed its 162-entry source suite and Windows CI, installed with exact four-payload and owner-profile preservation, and completed explicit three-route repair. It was superseded for a CLI Manager-startup reporting fix before original/current-chat workflow qualification, tagging or publication. Those measurements retain their exact 1.3.16 source and package identity. The unpublished [1.3.15 investigation](docs/validation/HOST-CAPABILITIES-1.3.15.md) records its actual source tests, signed installation and a preservation refusal: an isolated test Manager changed production LM Studio routing before installation. The earlier [1.3.14 investigation](docs/validation/HOST-CAPABILITIES-1.3.14.md) retains its own source, package and current-chat results. None of the earlier candidates establishes 1.3.19 current-chat qualification.

Create the engineering distribution from committed release inputs with `./scripts/package.ps1 -DevelopmentSigning`. See [Install](docs/INSTALL.md), [Product status](docs/STATUS.md), [User guide](docs/USER-GUIDE.md), [Architecture](docs/ARCHITECTURE.md), and [Roadmap](docs/ROADMAP.md).

Historical Alpha plans and evidence remain under `.forge-alpha/` and `docs/implementation/alpha-recovery/`; they describe their own artifacts rather than the current release.
