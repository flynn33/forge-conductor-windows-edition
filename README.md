# Forge Conductor for Windows

Forge Conductor is a native Windows 11 workspace and MCP tool server for project work with local models in LM Studio.

The current candidate is **1.3.18**, with **104 tools** in Primary and Fallback, ten specialist playbooks and CLU's five governance tools. LM Studio models can use dedicated web search/fetch/HTTP, native DOCX/XLSX/PPTX creation, Windows desktop observation and input, PNG drawing, image previews and independent image analysis, independent model workers, and persistent model-task schedules. Owner-selected host filesystem access covers ordinary local volumes while relative paths keep the selected project as their default. The narrower workspace mode remains selectable. See the [capability guide](docs/HOST-CAPABILITIES.md) and [1.3.18 candidate notes](docs/releases/1.3.18.md). Publication is pending.

Native Office files and PNG drawings require neither Office nor Python. Scheduled model tasks require Manager to stay running; future schedules restore after restart, while interrupted runs with uncertain effects require explicit authorization before another attempt. Generative artwork and cloud email, calendar, or chat accounts require separately configured providers or authorized APIs. `host_capabilities` reports the implemented tools, current filesystem policy, and these connection requirements.

`image_analyze` starts an authorized independent read-only vision run from a local image. Poll `reviewer_status` for its actual output, errors and token usage. The existing `image_read` preview remains available; LM Studio 0.4.25's stock MCP tool result stores image metadata rather than a model image file part. The analysis route uses Forge's working Responses image path and retains the three existing integrations.

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

Release verification and its limits are recorded in the [1.3.18 release notes](docs/releases/1.3.18.md) and [capability verification record](docs/validation/HOST-CAPABILITIES-1.3.18.md). Each distribution records its exact commit, tree, MSIX hash and payload-manifest hash. Historical installed and native-chat evidence applies to the artifact named in each report.

Final 1.3.18 source/build, installation and current-chat acceptance are pending. The installed, unpublished [1.3.17 investigation](docs/validation/HOST-CAPABILITIES-1.3.17.md) passed its exact source suite and CI, signed upgrade and automatic three-route repair. Current-chat testing then exposed the managed-run context-recovery lease defect; its attempts remain retained without final release qualification, a tag or publication. The unpublished [1.3.16 candidate](docs/validation/HOST-CAPABILITIES-1.3.16.md) passed its 162-entry source suite and Windows CI, installed with exact four-payload and owner-profile preservation, and completed explicit three-route repair. It was superseded for a CLI Manager-startup reporting fix before original/current-chat workflow qualification, tagging or publication. Those measurements retain their exact 1.3.16 source and package identity. The unpublished [1.3.15 investigation](docs/validation/HOST-CAPABILITIES-1.3.15.md) records its actual source tests, signed installation and a preservation refusal: an isolated test Manager changed production LM Studio routing before installation. The earlier [1.3.14 investigation](docs/validation/HOST-CAPABILITIES-1.3.14.md) retains its own source, package and current-chat results. None of the earlier candidates establishes 1.3.18 current-chat qualification.

Create the engineering distribution from committed release inputs with `./scripts/package.ps1 -DevelopmentSigning`. See [Install](docs/INSTALL.md), [Product status](docs/STATUS.md), [User guide](docs/USER-GUIDE.md), [Architecture](docs/ARCHITECTURE.md), and [Roadmap](docs/ROADMAP.md).

Historical Alpha plans and evidence remain under `.forge-alpha/` and `docs/implementation/alpha-recovery/`; they describe their own artifacts rather than the current release.
