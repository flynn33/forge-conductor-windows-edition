# Troubleshooting

## Repeated view pulse in 1.3.5

The retained Manager diagnostic became visible in the shared page after the Actions frame was removed. Periodic telemetry changed it between one line and several lines, moving the content below it. Version 1.3.6 restores collapsed visibility and skips transient connecting text during observation refreshes while keeping metric polling active. The isolated WinUI regression observer measured no position changes after the fix; installed-screen observation was not completed because the native-control driver failed before initialization with `MODULE_NOT_FOUND`. See [Release 1.3.6](Release-1.3.6).

## Model connection

The configured model endpoint remains same-host unauthenticated loopback, normally `127.0.0.1:1234`. Keep LM Studio Require Authentication disabled; Forge does not add a token or login setup. In Settings use **Load effective settings**, confirm the endpoint/model values, and **Test LM Studio**. A reachable endpoint is not proof that tools or rollover worked.

## Manager or observations unavailable

Inspect the exact status/error in Rig and Settings. Missing or stale observations are not zero. **Restart Manager** remains a Settings action, but restarting services is not part of routine native rollover. Closing Forge's window does not cancel LM Studio chat work.

## MCP registration drift or timeout

Finish active MCP calls before changing registrations. Use **Install or repair all three plugins** for the existing Primary/Fallback/CLU integrations and inspect their health/presence. All three use exact integer `timeout: 180000`; Forge shell calls remain capped at 120 seconds. Fallback does not automatically replay a timed-out mutation. Inspect its actual effect before retrying.

## Bound project or packages are wrong

Read `get_forge_status`: project ID/root and binding source, ordered package rows, policy source, tool names/count, and agent count. The Forge home or current-directory startup default is not proof of an adopted live project. Select/register the intended project in Workspace, then install or repair all three integrations for that selection and verify their activated status. A policy absent from a different bound project is configuration evidence, not proof the intended project lost its policy. Call `instruction_package.read` using that status row's queue_row_id; follow entry cursor/next_offset rather than substituting another folder.

Package admission does not reject extension, encoding, file count/size/depth/name. Opaque entries and access/reparse failures remain inventory coverage details; they are not silently discarded or represented as parsed text.

## Incomplete native result or invalid JSON

Version 1.3.11 bounds policy/instruction/filesystem reads and fragments other large tool results before LM Studio's measured per-text-block truncation boundary. Follow `next_cursor` or `next_offset` until the read is complete. For `forge_tool_result_fragment`, concatenate every `part` in index order and parse once; do not repeat a mutation to recover its output. Missing, reordered, inconsistent, or invalid fragments are not successful semantic evidence. See [MCP Protocol](MCP-Protocol#bounded-native-result-delivery).

For `shell_exec`, supported forward and mixed Windows cwd separators now canonicalize before strict authorization. Unsafe, dot/parent, device, unauthorized UNC, and outside-workspace paths remain errors. Report the exact error rather than assuming an executed shell command from a UI success label.

## Policy findings

Choose a policy repository folder to bind it immediately. Use **Inspect findings** to see the records/open count and Activity for correlated correction history. Ordinary non-CLU tool results deliver pending findings through `clu_governance_notifications`. Bounded read results explicitly defer them to `clu.findings`, preserving pending evidence. Reload policies only when intentionally adopting changed folder content.

## Packet saved but no chat rollover

Manager lifetime does not grant saved authority or confirm an uncertain UI effect. Current project/provider/route validation, the OS-held single-writer lease and selected-native evidence still gate control dispatch. An empty or unrelated third chat does not establish the successor. Explicit route migration requires a fresh authorized Primary callback, matching actual native packet/recovery evidence and exact preservation of the old encrypted checkpoint. Uncertain New chat or Send is reconciled without automatic replay. Completion requires the actual handed message, successor `context_get` and a following successful Forge tool result. Installed automatic delivery remains unqualified.

In the superseded 1.3.22 run, the retained checkpoint reached an uncertain New chat effect and the selected new conversation had zero messages. The three CLI and bridge parents exited with code 1; the captured exit codes do not identify their exit cause. Do not classify that empty chat as delivered or complete. The 1.3.27 Manager-lifetime change still requires installed native verification.

A saved packet or local connection ID alone is not native successor proof. Inspect the selected native conversation state and continuity status. The product must verify the exact handed message and enabled integrations in a new native chat, then packet retrieval and a following Forge result. CLU is governance and cannot repair chat creation by itself.

### Historical native qualification through 1.3.19

The next two paragraphs record the earlier implementation and its measured limits; they do not describe the retained checkpoint source in the 1.3.27 candidate.

Auto Continuity was verified with a reserve-triggered pause, not physical context exhaustion. Rollover was verified while the primary MCP worker stayed alive. Interrupted handoff after idle-process eviction is not durable and is not claimed.

If the primary MCP worker was evicted during handoff, durable packet records may still exist but automatic recovery of the interrupted UI phase is not implemented. Do not describe that state as completed. Physical exhaustion and already-running agent reattachment were not exercised by the 1.3.5 reserve-pressure verification.

### Current 1.3.27 Manager observer candidate — installed recovery unverified

The source now checkpoints the native phase, exact packet revision, conversation identities and New chat/Send receipts. Inspect `get_forge_status.visible_chat_continuity.state` and `handoff_recovery` for `recovery_pending`, its recorded phase, reason, packet ID, predecessor/successor IDs and `automatic_replay: false`. That state retains unfinished work; it is not a completed rollover.

Confirm the intended current project/provider and Primary/Fallback/CLU routes, then select the recorded predecessor or exact successor identified by the recovery reason. Reconstruction compares fresh selected-chat message/tool evidence with the checkpoint. An uncertain New chat or Send is never automatically repeated. A different empty chat, a saved packet alone, or a UI success label cannot settle an ambiguous dispatch. Exact packet recovery through `context_get` followed by a successful Forge tool in the successor can provide reconciliation evidence.

Checkpoint schema, integrity, scope, ownership or storage failures defer native controls. Keep the reported error and retained packet; do not clear the checkpoint or resend an uncertain mutation as routine repair. Historical published 1.3.21 source fixtures passed the 118-group Infrastructure suite, including the private same-PID observer reconstruction regression and 20 durability cases. Installed observer/UI interruption or connector rollover recovery remains unverified; physical context exhaustion and already-running agent reattachment were not exercised. Historical 1.3.20 checkpoint-process primitive evidence retains its original source identity and is not a current installed qualification. See [Auto Continuity](Continuity) for storage bounds and the evidence contract.

## Recovered packet has no actionable goal

Implicit pickup is project-scoped. Retrieve a specific saved packet by explicit ID only when needed and retain authorized workspace checks. Budget checkpoints preserve the bound workspace and recent tool/file evidence; an old pathless packet without a recorded goal cannot reconstruct missing history. Inspect status and packet content before treating it as a task instruction.

## Saved data

Use Settings **Saved project records** selection and delete buttons, or Continuity packet delete-selection/clear. The old scope-reset/export/import UI scheme is removed. Do not delete the ordinary `%LOCALAPPDATA%\Forge Conductor` folder as a routine repair.

For package installation errors, use the published distribution's non-installing preflight and repository [installation guide](https://github.com/flynn33/forge-conductor-windows-edition/blob/main/docs/INSTALL.md). Report version, project/provider, exact error, and time; do not include credentials or raw private content.

## Review deadlines, evidence roots and nested writes

Inspect `reviewer_status.receive_timeout_sec`, `failure_category`, and `infrastructure_blocked`. For a slow local provider, set the receive wait explicitly up to 3,600 seconds and poll the owned run. Optional `text_only` omits the catalog but cannot inspect external evidence. A timeout/connection failure never approves a gate. Use inline `opening_message` when an opening file is not under an active authorized root.

Inspect configured and active roots separately. In workspace mode, activate an exact owner-configured directory with `workspace_authority_bind`; the retained native repair keeps that binding through recovery. Rebind after a connection restart. Owner-selected host mode uses ordinary available local volumes with native ACL/path checks. A deliberately removed permission remains denied in both modes. Working directories use the same local checks for synchronous shell, tracked shell, and process launch.

For native `filesystem_access_denied`, retain the reported operation/path/Win32 code. The repaired parent-creation route no longer requires `FILE_DELETE_CHILD`; actual OS denials remain errors. A 65,536-byte UTF-8 shell command is supported through stdin; larger scripts still exceed the explicit bound.

## Dedicated workflows and schedules in 1.3.19

Call `host_capabilities` and inspect runtime version/catalog before claiming a category is absent. Repair/reconnect the intended integrations after an upgrade. Dedicated Office writing and native PNG drawing do not require Python or Office; generative image providers and cloud accounts still need configured connections. A browser launch or desktop input receipt requires observing the resulting UI.

For schedules, inspect actual `latest_run`, `needs_attention`, `last_notification`, history and errors. Manager must remain running to fire tasks. An uncertain interrupted run blocks automatic replay; inspect its effects before explicitly authorizing run-now. A Windows toast submission receipt never confirms banner display. Disabled settings or missing installed identity remain notification failures, without changing the model run or Windows settings.

### Image drawing, analysis and generation

The superseded installed 1.3.22 Primary native status reported 112 tools, while complete 112/112/5 catalog qualification and integrated provider acceptance remained incomplete. The 1.3.27 source inventory is not proof of a current installed upgrade. `image_write` draws; `image_analyze` and `reviewer_status` inspect existing images. Generation and editing use the explicitly configured ComfyUI `image_generate`/`image_edit` jobs and `image_job_status`; local cancellation and exact-job resume have separate effect/authority contracts. A model claim or advertised tool count does not prove generation completion. See [provider contract](https://github.com/flynn33/forge-conductor-windows-edition/blob/main/docs/IMAGE-PROVIDER.md).
