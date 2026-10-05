# Forge Conductor for Windows

Forge Conductor is a native Windows 11 workspace and MCP tool server for project work with local models in LM Studio.

The **1.3.11 release** accepts supported Windows path separators at the request authorization boundary, pages policy inventory, bounds serialized text reads, and preserves large tool results through native LM Studio result limits. It repairs legacy instruction-package visibility in MCP bootstrap, filesystem tool schemas, automatic checkpoint metadata, and project-scoped recovery. Install/repair binds all three LM Studio integrations to the selected registered project. The public release includes the repairs developed locally as 1.3.7 through 1.3.10; those intermediate versions were not GitHub releases. See the [release notes](docs/releases/1.3.11.md) and [download 1.3.11](https://github.com/flynn33/forge-conductor-windows-edition/releases/tag/v1.3.11).

## Product surfaces

- **Workspace:** project/provider selection, ordered instruction packages, CLU development-policy governance, Auto Continuity, and readiness.
- **Rig:** live system, selected LM Studio chat context, model, storage, process, and workflow observations.
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

Release verification and its limits are recorded in the [1.3.11 release notes](docs/releases/1.3.11.md). The public package is rebuilt from the commit tagged `v1.3.11`; its `distribution.json` records that commit, tree, MSIX hash and payload-manifest hash. Historical installed and native-chat evidence applies to the artifact named in each report. The publication build is qualified separately without replacing the running installation.

Create the engineering distribution from committed release inputs with `./scripts/package.ps1 -DevelopmentSigning`. See [Install](docs/INSTALL.md), [Product status](docs/STATUS.md), [User guide](docs/USER-GUIDE.md), [Architecture](docs/ARCHITECTURE.md), and [Roadmap](docs/ROADMAP.md).

Historical Alpha plans and evidence remain under `.forge-alpha/` and `docs/implementation/alpha-recovery/`; they describe their own artifacts rather than the current release.
