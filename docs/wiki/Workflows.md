# Workflows

These figures are the operator paths. The receipts they depend on stay on [Release 1.3.29](Release-1.3.29) and [Qualification](Qualification).

## Prepare the workspace

<p align="center"><img src="images/diagram-workflow.png" alt="Seven operator steps from Workspace through project selection, instruction order, CLU, Auto Continuity, the three plugins, and get_forge_status." width="100%"></p>

Details: [Workspace guide](Guided-Setup), [Projects and instructions](Project-Instructions), [CLU governance](Setup-and-Governance), [LM Studio plugins](MCP-Protocol).

## Preview, then approve

<p align="center"><img src="images/diagram-comfy.png" alt="ComfyUI path from Settings through one sealed preview and a saved reply of approved, yes, or render final. Legacy sd1 tools stay separate." width="100%"></p>

Details: [ComfyUI automation](ComfyUI-Automation). The legacy provider contract is the [image-provider guide](https://github.com/flynn33/forge-conductor-windows-edition/blob/main/docs/IMAGE-PROVIDER.md).

## Continue in one successor chat

<p align="center"><img src="images/diagram-continuity.png" alt="Reserve pressure, a finished tool boundary, a DPAPI packet, New chat and Send, then context_get and a following Forge call." width="100%"></p>

Details: [Continuity](Continuity). The 1.3.28 installed delivery used reserve pressure. It did not fill the context window.

## Ownership

<p align="center"><img src="images/diagram-architecture.png" alt="LM Studio, three plugins, the per-user Manager, the WinUI app, and external ComfyUI." width="100%"></p>

<p align="center"><img src="images/diagram-surfaces.png" alt="Workspace, Rig, Continuity, Activity, and Settings." width="100%"></p>

Details: [Architecture](Architecture), [Process model](Process-Model), [Product surfaces](Product-Surfaces).

## Tool families

<p align="center"><img src="images/diagram-tools.png" alt="Families inside the 125-tool catalog, including eleven ComfyUI tools and six separate legacy sd1 tools." width="100%"></p>

The names are on [Tool catalog](Tool-Catalog). `host_capabilities` reports what this host can run. A listed tool is not a measured run.
