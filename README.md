# Forge Conductor for Windows

Forge Conductor is a native Windows 11 workspace and MCP tool server for project work with local models in LM Studio.

Version **1.3.14** expands Primary and Fallback to **103 tools**, while retaining ten specialist playbooks and CLU's five governance tools. LM Studio models can use dedicated web search/fetch/HTTP, native DOCX/XLSX/PPTX creation, Windows desktop observation and input, PNG drawing and image previews, independent model workers, and persistent model-task schedules. Owner-selected host filesystem access covers ordinary local volumes while relative paths keep the selected project as their default. The narrower workspace mode remains selectable. See the [capability guide](docs/HOST-CAPABILITIES.md), [1.3.14 release notes](docs/releases/1.3.14.md), and [download 1.3.14](https://github.com/flynn33/forge-conductor-windows-edition/releases/tag/v1.3.14).

Native Office files and PNG drawings require neither Office nor Python. Scheduled model tasks require Manager to stay running; future schedules restore after restart, while interrupted runs with uncertain effects require explicit authorization before another attempt. Generative artwork and cloud email, calendar, or chat accounts require separately configured providers or authorized APIs. `host_capabilities` reports the implemented tools, current filesystem policy, and these connection requirements.

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

Release verification and its limits are recorded in the [1.3.14 release notes](docs/releases/1.3.14.md) and [capability verification record](docs/validation/HOST-CAPABILITIES-1.3.14.md). Each distribution records its exact commit, tree, MSIX hash and payload-manifest hash. Historical installed and native-chat evidence applies to the artifact named in each report.

The fresh x64 Release source test command passed **162/162 CTest entries in 58.10 seconds**, with all three static gates and the package-persistence contract passing. A disposable-profile direct MCP probe passed ten groups and 63 requests, including native Office/images, host continuity and complete worker/schedule/reconnect behavior, using an owned loopback provider. Signed-package installation and acceptance by the current Qwen conversation remain separate and pending.

Create the engineering distribution from committed release inputs with `./scripts/package.ps1 -DevelopmentSigning`. See [Install](docs/INSTALL.md), [Product status](docs/STATUS.md), [User guide](docs/USER-GUIDE.md), [Architecture](docs/ARCHITECTURE.md), and [Roadmap](docs/ROADMAP.md).

Historical Alpha plans and evidence remain under `.forge-alpha/` and `docs/implementation/alpha-recovery/`; they describe their own artifacts rather than the current release.
