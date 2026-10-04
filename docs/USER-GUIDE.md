# User guide

Launch Forge Conductor from Start. The destinations are **Workspace**, **Rig**, **Continuity**, **Activity**, and **Settings**. Work happens in LM Studio chats.

## Prepare a workspace

1. Register or select the project folder in Workspace and choose its provider profile.
2. Add instruction-package folders. Move queue rows up or down to define execution order.
3. Under CLU governance, choose **Choose policy folder...**. Selecting a repository folder binds it immediately; there is no second Bind Source action.
4. Set **Auto Continuity** for the selected project/provider and review readiness.
5. Use the loaded model in LM Studio with the existing Forge integrations.

The Manager persists project access, packages, policy bindings, memory, preferences, and telemetry. The primary MCP process owns the native chat rollover worker. Closing Forge's window does not cancel LM Studio chat work.

## Instruction packages and status

A package is a folder. Intake inventories entries and streams hashes without extension, encoding, file-count, file-size, aggregate-size, depth, or name-based admission filters. Files, directories, reparse points, and read failures remain explicit. Safe derived text is bounded and content pages retain revision identity.

The model can call `get_forge_status` to obtain `workspace.project_id`, `workspace.project_root`, binding-source fields, `instruction_packages.packages` in execution order, the development-policy source/revision, tool names/count, and agent count. `home` is application data, not the project folder. An empty continuity path does not erase the registered workspace binding.

Call `instruction_package.read` with the selected `queue_row_id` from status. Read all cursor pages; an incomplete text entry is continued with its `path` and `next_offset`. The response includes the selected package, returned entries and content, completion markers, and `next_cursor`. Opaque or inaccessible entries retain coverage information. Packages from another project are not substituted.

## CLU policy repository

CLU evaluates development-policy evidence. It does not create chats or control continuity. The folder picker selects a repository, not one policy file, and binds it on selection.

The remaining controls are **Reload policies from folder**, **Inspect status**, **Inspect findings**, **Export CLU log**, **Policy documents**, **Read document**, **Next part**, and **Selected policy document**. Reload intentionally adopts changed folder content. The model receives the bound source and the instruction to read and follow it through `project_policy.read`.

After a non-CLU tool operation, the adapter evaluates structured evidence and adds pending findings to the returned `clu_governance_notifications`. The loaded model therefore receives correction guidance. **Inspect findings** displays the findings and open count; Activity retains correlated evidence and correction history. Missing policy is an inactive state; findings remain nonblocking governance evidence.

The CLU role exposes `clu.evaluate`, `clu.export_log`, `clu.findings`, `clu.resolve`, and `project_policy.read`.

## LM Studio integrations

**Install or repair all three plugins** is one action for `forge-conductor`, `forge-conductor-fallback`, and `forge-conductor-clu`. Foreign integrations and unknown configuration fields are preserved. Each Forge registration uses the exact 180-second outer request deadline; Forge `shell_exec` remains capped at 120 seconds.

Primary owns automatic chat rollover. Fallback is an independent general-catalog integration, not automatic replay of an ambiguous timed-out mutation. CLU supplies governance tools and receives project packet context through shared initialization; recover packets through Primary or Fallback. No continuity plugin or new Forge credential is required. Same-host model traffic retains the existing unauthenticated loopback contract.

## Auto Continuity and packets

The Workspace/dashboard **Auto Continuity** toggle is persisted for the selected project/provider. Settings exposes effective capacity, next-response reserve, handoff reserve, and safety margin.

The primary worker reads the selected native LM Studio conversation's actual generation usage and loaded capacity. At context reserve pressure it stops generation at a completed tool boundary without cutting off an active Forge tool. It requests a detailed model-written packet containing goal, verified work, decisions and constraints, files, blockers, agent sessions, and ordered next actions. `session_handoff` saves that packet and publishes project-scoped pickup.

The product opens **New chat** through Windows UI Automation, retains the same three integrations and loaded model, sends the packet, and checks the exact successor conversation ID and persisted handed message. The successor calls `context_get` and then another Forge tool. Packet storage or an MCP connection ID alone is not credited as native chat rollover.

Auto Continuity was verified with a reserve-triggered pause, not physical context exhaustion. Rollover was verified while the primary MCP worker stayed alive. Interrupted handoff after idle-process eviction is not durable and is not claimed.

The **Continuity** view shows saved packets and selected packet detail. Use **Refresh packets**, **Delete selected packet**, or **Clear all packets**. These actions remove saved records, not policy folders or project source files.

## Memory and Settings

Workspace **Search** reads a bounded page. **Display All** follows Manager cursors in 100-record pages and rejects inconsistent counts, repeated cursors, or changed record sets. Select a record to inspect or edit it.

Settings retains **Load effective settings**, **Save and read back**, **Revert pending edits**, **Test LM Studio**, and **Restart Manager**. Under **Saved project records**, select one or more records and press **Delete selected**, or use a row's **Delete record**. **Refresh records** reloads authoritative state. Source folders are not deleted. The old scope-reset scheme and Apply and Verify are removed.

Managed Run, its readback, Export selected project, Import verify first, the Actions frame and Invoke Tool, Advanced Canonical Catalog, Scope Test, and Reset Scope are removed. `agent_run_start`, `agent_run_status`, and `agent_run_complete` remain baseline specialist-session tools.

Version 1.3.6 keeps Rig layout stable during background telemetry refresh without disabling updates. Rig shows unavailable or stale observations explicitly. Activity displays operational and governance outcomes. Ordinary data lives at `%LOCALAPPDATA%\Forge Conductor`; disposable validation uses a separate home.
