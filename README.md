# Forge Conductor for Windows

Forge Conductor is a native Windows 11 workspace and MCP tool server for project work with local models in LM Studio.

The **1.3.6 release** fixes view movement during telemetry refresh by keeping the diagnostic status row collapsed and excluding background polls from command-progress text. It retains the 1.3.5 native LM Studio chat Auto Continuity, callable instruction-package reading, authoritative project disclosure, and direct CLU policy-folder binding. See the [release notes](docs/releases/1.3.6.md).

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

Version 1.3.6 passed the complete 153-entry x64 CTest matrix in 32.36 seconds, backend/App/package builds, and all three static gates. Actual WinUI geometry stayed stable across 16 injected telemetry polls while CPU values continued updating. The isolated native-chat evidence and its bounds are documented separately.

Create the engineering distribution from committed release inputs with `./scripts/package.ps1 -DevelopmentSigning`. See [Install](docs/INSTALL.md), [Product status](docs/STATUS.md), [User guide](docs/USER-GUIDE.md), [Architecture](docs/ARCHITECTURE.md), and [Roadmap](docs/ROADMAP.md).

Historical Alpha plans and evidence remain under `.forge-alpha/` and `docs/implementation/alpha-recovery/`; they describe their own artifacts rather than the current release.
