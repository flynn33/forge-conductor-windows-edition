# User guide

Launch Forge Conductor from Start. Normal operation has four destinations: **Workspace**, **Rig**, **Activity**, and **Settings**.

## Prepare a workspace

1. Open **Workspace**, register or select the project folder, discover the models currently loaded in LM Studio, and select the saved provider profile and loaded LLM instance.
2. Add zero or more instruction-package folders. Each selected folder becomes a project-scoped queue row. Use **Move up** and **Move down** to define the order supplied to new work.
3. Optionally bind a local or remote development-policy source under **CLU governance**, then inspect its immutable revision and coverage.
4. Set **Automatic continuity** independently for this project/provider pair.
5. Review readiness. A missing optional policy or disabled continuity does not block ordinary work.

Forge Conductor remembers the selected project/provider preference and queue. Closing the window does not cancel Manager-owned work.

## Browse project memory

Select a project on **Workspace**. Enter text and choose **Search** for a bounded result, or choose **Display All** to load every active record. Display All follows Manager cursors in 100-record pages and stops with an explicit error if the record set changes, a cursor repeats, or the reported count is inconsistent. Select a visible row to inspect or edit that exact record.

Browse and Display All operate on durable project-memory records. They do not inspect, start, or prove an automatic-continuity rollover.

## Instruction-package folders

A package is a folder, not a special manifest format. Intake walks the complete directory tree and does not reject a package because of file extension, encoding, NUL bytes, path depth, file count, individual size, aggregate size, or filename. Content is hashed incrementally rather than loaded wholesale.

The queue records:

- files and directories in deterministic order;
- reparse points without silently following them;
- metadata and SHA-256 content identities;
- inaccessible or changed entries as explicit failures;
- bounded derived UTF-8 text when safe, otherwise an opaque-content classification.

Open a package to page through its inventory or bounded content. Cursors are tied to the package revision so a changed revision cannot be resumed accidentally. Retry a failed row after correcting its source. Removing a queued row affects future work; active execution retains the revision it already received.

New managed work receives the ordered package identities and available bounded text through a durable agent-goal envelope of up to 128 KiB. Nothing is silently described as “ignored” or “unsupported”; entries that cannot be interpreted as text remain recorded and retrievable by identity. If an interpreted entry cannot fit beside the requested task and package headers, the run does not start, the queue cursor does not advance, and the error directs you to shorten the task or remove/reorder that package. Admission records the exact cursor plan with the run before any provider or tool work starts. Cursor advances for several packages commit together; after an interruption, retrying the exact same run reconciles that durable plan before dispatch and returns the original assignment without consuming packages activated later.

## CLU governance

CLU governs development-policy compliance only. It never starts, stops, or reports continuity.

Bind a policy source from the Workspace governance card. The binding records its source identity, immutable revision, entry inventory, interpretation state, and coverage gaps. Use the document reader to inspect bounded pages. Refresh deliberately when the source changes.

During development, CLU can:

- evaluate structured evidence against the bound policy;
- create deduplicated findings and correction requests;
- expose pending notification receipts;
- accept correction evidence and resolve findings;
- export a redacted governance history.

Governance is nonblocking by design. No bound policy produces an inactive state rather than preventing work. A finding informs the operator and requests correction; it does not impersonate the independent runtime-continuity system.

The dedicated `clu` MCP role exposes `clu.evaluate`, `clu.export_log`, `clu.findings`, `clu.resolve`, and `project_policy.read`.

## LM Studio MCP roles

Forge installs three independent LM Studio integrations: **Primary**, **Fallback**, and **CLU**. Each registration uses an exact 180-second LM Studio request timeout. Forge `shell_exec` requests remain capped at 120 seconds, leaving time for Forge to return the result before LM Studio closes the request.

When a role connects, Forge's MCP initialization instructions identify the exact registered project folder, the ordered instruction-package folders, and the active development-policy source and revision. `forge_status` returns the same facts as structured `workspace`, `instruction_packages`, and `development_policy` fields. The `home` field remains Forge's application-data directory and is not the project folder. An empty handoff `paths` object or empty continuity `implicit_roots` list describes that continuity record only; it does not override the authoritative workspace binding reported by `forge_status`.

The local LM Studio model endpoint is an unauthenticated same-host loopback contract. Keep LM Studio **Require Authentication** disabled. Work Space discovery reads `/api/v1/models` and lists only loaded LLM instance IDs; downloaded-but-unloaded models and embedding models are not selectable. **Test model discovery** reads that same inventory without sending a prompt, while the separate Responses contract probe sends a disposable provider request. If authentication is enabled, LM Studio can show connected while discovery, the Responses probe, or automatic model preparation reports an authentication failure. Forge does not require a local LM Studio token or provide bearer-auth setup controls.

Use `project_policy.read` to inspect the bounded policy document after locating its source through the initialization instructions or `forge_status`.

Fallback is a separately selected integration and health role. A timeout does not automatically replay the call through Fallback or switch the chat back to Primary. Before retrying a timed-out command that can change files, processes, or external state, inspect the relevant state because the original call may have completed after the client stopped waiting.

If a tool reports `MCP error -32001 Request timed out` near 60 seconds, finish or stop active calls, open **Settings → LM Studio plugins**, choose **Install or repair all three plugins**, then choose **Open LM Studio and connect plugins**. Select the intended Forge integration in the chat before retrying. Repair all three roles together; do not hand-edit only one registration. LM Studio reloads MCP configuration during repair, so an in-flight call can disconnect.

## Automatic continuity

The **Automatic continuity** toggle is saved per selected project/provider profile and its effective value is persisted with the admitted run, including after Manager restart. When enabled, a Manager-owned run may create and bootstrap a successor at the context boundary, require its structured acknowledgement, fence the predecessor, and resume productive work. Repeated rollovers in one run use distinct sequenced operation and handoff identities; completed work added after an earlier checkpoint is refreshed before successor creation with an exact prior-digest comparison. Successors receive a bounded UTF-8-safe restatement of the original task and latest completed work. If a run ends or rollover fails before a successor consumes the checkpoint, Forge durably abandons it rather than allowing later maintenance to resume finished work. On the next Manager start, a pre-ingress pass protected by the process-wide instance lease abandons checkpoint-only work left by an earlier process; periodic maintenance leaves checkpoints owned by a live process stable and resumes only durable successor intent. When disabled, the run skips automatic continuity observations and remains otherwise unchanged.

LM Studio desktop chats and Forge-managed runs are separate modes. Connecting MCP does not retroactively enroll a desktop chat in Manager-owned continuity.

A fresh post-fix live continuity check is still required. Until it passes, the 1.3.5 source candidate does not claim a qualified Manager-owned successor chain. A completed chain, once qualified, is not expected to remain reported as an active operation.

## Rig, Activity, and Settings

- **Rig** shows live resource, provider, storage, process, context, continuity, and workflow health. Missing observations are displayed as unavailable, never as zero.
- **Activity** presents operational outcomes and is the normal place to correlate work with CLU findings, corrections, notifications, and exported evidence.
- **Settings** contains persistent runtime controls, explicitly scoped maintenance actions, and Doctor. With a project selected, Doctor checks package queue/cursor integrity, CLU repository/notification/export state, project memory, continuity provider binding, migrations, and schema alignment.

Under **Settings → Data maintenance**, the selected project's memory appears in a scrollable list. Use **Delete record** on a row, or select multiple rows with the normal Windows Ctrl/Shift selection gestures and choose **Delete selected**. One destructive confirmation is shown, each selected memory key is sent through `project_memory.forget`, and the list refreshes afterward. **Reset project continuity** is a separate wipe action; it is unrelated to Browse/Display All or record deletion. The older exact-confirmation reset remains separate for project memory, project continuity, combined project data, or all registered project data.

Version 1.3.5 remains a source candidate; no 1.3.5 MSIX is claimed by this guide until the committed release pipeline emits and verifies one. Manager and live-provider checks do not replace installed interaction and operator acceptance of these controls.

Ordinary data lives at `%LOCALAPPDATA%\Forge Conductor`. For disposable validation only, launch `ForgeConductorApp.exe --alpha-root <absolute-empty-folder>`.
