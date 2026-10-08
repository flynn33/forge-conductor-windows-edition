# Auto Continuity

The primary `forge-conductor` MCP worker owns native visible LM Studio chat rollover. Workspace saves **Auto Continuity** for the exact project/provider pair. CLU remains governance; Fallback remains an independent general-catalog integration. No fourth plugin or Forge credential is introduced.

## Current 1.3.21 checkpoint source

The Primary worker now persists the interrupted native handoff phase alongside the existing saved packet. Final reconstruction regression and installed interrupted-connector restart qualification are pending; the historical 1.3.5/1.3.11 checks below do not qualify the new reconstruction path.

A current-user DPAPI checkpoint lives at `continuity/lmstudio-visible-<project-id>.checkpoint` under the routed Forge home. The existing authorized atomic store publishes its schema, source contract, revision, scope and bounded state; plaintext is limited to 8 MiB and stored content to 10 MiB. One exclusive Windows file handle owns native UI writing across projects on that home and is released on worker exit. A saved checkpoint, lock filename or PID does not grant current workspace authority.

Reconstruction requires fresh authorized project/root, home, LM Studio root/executable, provider and Primary/Fallback/CLU routing confirmation. The retained phase includes predecessor/successor identities, exact packet body/write sequence, messages and native effect receipts. Before New chat or Send, the worker checkpoints an `uncertain` receipt and records confirmation only after native evidence. Uncertain effects are not automatically replayed.

Read `get_forge_status.visible_chat_continuity.state` and `handoff_recovery`. `recovery_pending` carries the recorded phase, identities, reason and `automatic_replay: false`; it retains unfinished work and is not completion. Reconciliation uses the freshly selected recorded chat and exact new message evidence, or matching successor `context_get` packet recovery followed by a successful Forge tool result. Another empty or third chat is insufficient. Changed scope, malformed/integrity-invalid checkpoints, unavailable ownership/storage and missing selected chat defer controls. See [Troubleshooting](Troubleshooting#current-1320-checkpoint-recovery).

## Project-scoped recovery

Implicit `context_get` uses the active client/project binding. A global fallback is eligible only for its matching authorized workspace. Explicit packet-ID lookup remains available; adoption requires a registered authorized project root. Empty paths in a legacy packet do not erase the live binding.

Budget checkpoints retain the authorized workspace and recent file/tool evidence. Successful `session_checkpoint` and `session_handoff` writes refresh the recovered packet ID; failed writes preserve the prior recovery state. Explicit seed and persisted run receipts survive updates. An automatic packet with no actionable historical goal does not invent one.

Large native tool results are reassembled only from a complete, consistent fragment sequence for the same call before continuity consumes their evidence. Raw conversation evidence remains available. Malformed or incomplete results cannot establish a completed tool boundary. See [MCP Protocol](MCP-Protocol#bounded-native-result-delivery).

## Lifecycle

1. Read actual generation usage and capacity from the selected native conversation.
2. Detect context reserve pressure and pause generation only at a completed tool boundary. An active Forge tool is allowed to finish.
3. Ask the loaded model to save a detailed `session_handoff` packet with goal, verified work, constraints/decisions, files, blockers, agent sessions, and ordered next actions.
4. Publish `continuity/project/<project-id>` for shared connector pickup.
5. Use product Windows UI Automation to create **New chat**, retain the loaded model and three existing integrations, and send the complete packet.
6. Verify native successor identity and the exact on-disk handed message. Observe successor `context_get` recovery and another successful Forge tool call before reporting `resumed`.

## Historical native qualification

The original reserve-triggered native predecessor/successor evidence is recorded in [Release 1.3.5](Release-1.3.5). It used a live primary MCP worker and does not qualify physical context exhaustion.

The accepted 1.3.11 installed checks verify enabled, unblocked native observing state and project-scoped pickup. They did not run a new reserve-triggered rollover. Specialist persistence/reattachment regression coverage does not establish reattachment of an already-running native agent during a chat rollover.

## Historical durability limits through 1.3.19

The next paragraph preserves the earlier in-process limitation; it predates the current 1.3.21 checkpoint implementation. Physical exhaustion and already-running agent reattachment remain separate qualification work.

The packet is persisted, but the in-flight UI handoff phase is process-local. Recovery of that interrupted phase after MCP worker eviction/restart is unfinished. Historical rollover qualification did not exercise physical exhaustion or reattachment of an already-running agent. A fresh CLU initialize was not exercised in that historical native rollover; its packet context uses the shared bootstrap path, and Primary/Fallback provide recovery tools.

## Packet controls

Open **Continuity** to inspect the saved packet list and selected details. **Refresh packets**, **Delete selected packet**, and **Clear all packets** operate on packet records. Settings retains separate saved-project-record selection and delete buttons.

See [Release 1.3.11](Release-1.3.11), historical [Release 1.3.6](Release-1.3.6), and [Architecture](Architecture).
