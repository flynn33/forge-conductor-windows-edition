# Native LM Studio chat Auto Continuity

## Current 1.3.22 snapshot boundary — qualification pending

The shared WindowsLMStudioChatControl native file reader now uses the same 64 MiB bound as the ordinary conversation reader. Its consumers include plugin cleanup, saved loaded-model acknowledgement and completed-native-tool-boundary pause corroboration. The previous shared helper used 32 MiB and described an oversized native snapshot as an integration-field update error. The current source reports a general snapshot-bound error and keeps the existing file identity/revision and cancellation/deadline checks.

A frozen large current chat exceeds 32 MiB, but its three current plugins do not establish which shared caller failed. The narrow Infrastructure entry passed 124/124 groups, including five real-file shared-reader/revision cases, in 21.94 seconds. It did not exercise actual UI model acknowledgement, plugin cleanup, completed-tool-boundary pause or concurrent mutation during the read. Installed current-chat recovery and full integrated qualification remain pending. This change does not prove visible rollover, physical context exhaustion or interrupted Send/New chat recovery. The 1.3.21 checkpoint measurements below retain their original artifact identity.

## Current 1.3.22 explicit route recovery

The existing authorized Primary session_handoff callback records recovery intent under its current full scope. WindowsLMStudioChatContinuity::recoverRouteFromNativeHandoff then requires the matching successful result in the selected predecessor's saved Primary native messages, a newer complete model packet and current project pointer. Fresh LMStudioConfigurationCodec inspection validates all three registrations against the running executable, home and workspace; current provider/scope and native/store/pointer evidence are checked again before publication. Saved/current scope may differ only in routing_sha256. There is no new MCP tool or schema.

Recovery is limited to an undispatched WaitingPacket with null effect, false request/repair/delivery acknowledgements, no new packet/successor/handed message/repair, and the retained dispatch failure. LMStudioChatCheckpoint::recoverRoute archives the original encrypted checkpoint byte-for-byte, retaining its original scope, revision and hash, before atomically publishing the current-scope planned Creating state. The old request acknowledgement remains false and the failed request is not resent. Planned Creating reconstruction requires the complete retained model packet with unchanged body/write sequence. Missing evidence, other scope changes and uncertain/confirmed effects retain recovery_pending without automatic replay.

The successful status exposes route_recovery archive_path, archive_sha256, previous_revision, previous_scope, native_request_id, packet_id, packet_write_sequence and old_request_acknowledged: false. Existing handoff_recovery continues to explain refusals. The final modified-file focused run passed 125/125 Infrastructure groups, including 37 route-recovery cases, and CMake/CTest: 54.53/9.91 seconds, 64.45 seconds total. This is private source-fixture evidence; final clean-source/full-suite and installed UI/current-chat recovery remain pending. The prior clean-source 165/166 failure is preserved in the 1.3.22 qualification record.

## Retained 1.3.21 source contract and historical checks

The Primary stdio MCP composition continues to own `WindowsLMStudioChatContinuity`; Fallback and CLU do not start competing native rollover workers. The current source adds durable phase reconstruction through `LMStudioChatCheckpoint` and New chat/Send effect receipts in `WindowsLMStudioChatControl`. The 118-group Infrastructure suite passed, including the private same-PID observer reconstruction regression and 20 durability cases. Installed observer/UI interruption or connector rollover recovery remains unverified; physical context exhaustion and already-running agent reattachment were not exercised. Historical 1.3.20 checkpoint-process primitive evidence retains its original source identity and is not a current installed qualification. The historical checks below do not qualify this new behavior.

### Bounded checkpoint and writer ownership

Each project checkpoint is stored under the routed Forge home at `continuity/lmstudio-visible-<project-id>.checkpoint`. The existing authorized atomic file store publishes a schema-versioned envelope with the source contract `selected-native-chat-effects-v1`, an incrementing revision, scope and state. Windows current-user DPAPI seals the content without a new credential or external service. Plaintext is limited to 8 MiB and stored content to 10 MiB; restored text and native evidence collections also have explicit bounds. This checkpoint is separate from the existing saved handoff packet.

An exclusive Windows file handle on `continuity/lmstudio-visible-writer.lock` allows one native UI writer across projects sharing the routed home. Worker exit releases the handle; a persisted lock filename or PID does not grant ownership. Checkpoint and lock access use fresh authorized paths, and native controls are deferred when ownership or checkpoint publication cannot be confirmed.

### Fresh scope and effect evidence

Restored state does not grant workspace authority. The current connector must confirm its authorized project/root, routed home, LM Studio root/executable, provider configuration and exact Primary/Fallback/CLU route hash. The worker reloads provider configuration and checks the current workspace and routing scope again before a control dispatch. A fresh unconfirmed workspace remains `awaiting_bound_workspace`; saved changed or unreconciled scope, malformed/schema-drifted state, failed DPAPI integrity or unavailable storage yields `recovery_pending` and defers controls.

The checkpoint retains the handoff phase, exact packet body/write sequence, predecessor and confirmed successor IDs, exact request/repair/delivery messages, relevant native evidence boundaries and effect receipts. Before New chat or Send dispatch, the worker durably records an `uncertain` effect; confirmation updates that receipt only after observed native evidence. Reconstruction reads the freshly selected native conversation and reconciles that evidence against the retained state. An uncertain control effect is not automatically replayed. An unrelated empty or third chat does not establish a successor.

For the waiting-packet phase, reconciliation requires the recorded predecessor and exact new native request/repair message evidence. Later phases check the saved packet revision and the exact successor/message, or matching native `context_get` packet recovery followed by a successful Forge tool result. Completion still depends on native packet recovery and following work, rather than checkpoint existence. Missing selection retains unfinished state and dispatches no control. Inspect `get_forge_status.visible_chat_continuity.handoff_recovery` for the pending phase, identities, reason and `automatic_replay: false`.

The relevant implementation is `src/Infrastructure/Windows/WindowsLMStudioChatContinuity.cpp`, `LMStudioChatCheckpoint.cpp` and `WindowsLMStudioChatControl.cpp`. Reconstruction regressions are in `tests/Infrastructure/WindowsLMStudioConversationReaderTests.cpp`. Source implementation and fixture coverage do not establish installed LM Studio restart, physical exhaustion or already-running agent reattachment results.

## Historical 1.3.5 implementation

This section preserves the original native implementation and qualification assertions. Its process-local limitation predates the current checkpoint source.

The primary stdio MCP composition owns `WindowsLMStudioChatContinuity`; Fallback and CLU do not start competing rollover workers. `WindowsLMStudioConversationReader` reads selected native conversation identity, actual generation stats, active-tool state, and correlated tool requests/results. `WindowsLMStudioChatControl` uses native Windows UI Automation for a completed-tool-boundary pause, New chat, loaded-model/integration retention, and Send.

After context reserve pressure, the worker requests a detailed model-written `session_handoff` packet. The adapter publishes `continuity/project/<project-id>` for shared connector initialization. The worker creates and confirms a fresh native conversation ID, sends the complete packet message, verifies all three integrations and the exact persisted message, then observes native `context_get` recovery and a following Forge tool result. A local connection ID or logger event alone is not treated as native chat proof.

Auto Continuity was verified with a reserve-triggered pause, not physical context exhaustion. Rollover was verified while the primary MCP worker stayed alive. Interrupted handoff after idle-process eviction is not durable and is not claimed.

The worker's in-flight phase is process-local. Durable packet storage and connector pickup exist, but they do not make interrupted native UI handoff recoverable after primary-worker eviction. The release notes identify native on-disk predecessor/successor evidence and unexercised cases.

## Historical provider-run implementation record

The following specification and Alpha observations describe earlier Manager-owned Responses execution. They are retained as history, not the 1.3.5 product path or native-chat durability claim.

# Real provider execution and context-only continuity
## Existing seams to change
Start in `src/Application/ContinuityAutomation.cpp` and its policy model/consumers.
The existing `triggerDecision` evaluates a context action, then falls back to progress/time rollover.
Remove that fallback and the corresponding product settings, serializers, validators, prompts and obsolete test expectations.
Search from `rolloverProgressInterval`, `rolloverIntervalSeconds`, `completedProgressUnits`, and any per-run call allowance.
Periodic durable autosave is acceptable, but it must never create a new session merely because time or a count elapsed.

Keep `ContinuityCoordinator`, durable handoff models, checksums and exact successor bindings.
Keep small per-request timeouts, output truncation, cancellation and concurrency capacities.
Do not confuse a protocol's required acknowledgment sequence with a quota on how many tools a run may use.
Stop ordinary work only on task completion, operator action, real nonrecoverable failure, or an actual context transition.

## One real LM Studio provider implementation
Implement a native C++/WinHTTP `LMStudioResponsesTransport` (name is proposed) at the existing provider seam.
Use `POST /v1/responses`. The Mac implementation in `Sources/ForgeNativeSessionHostPlugin/ForgeNativeSessionHostPlugin.swift`
already provides a behavioral example using that route. Do not port its entire release qualification framework.
The existing `/v1/forge/sessions` transport is a custom protocol and must not be presented as the LM Studio endpoint.
Production composition currently instantiates `LocalLogicalSessionTransport`; explicitly replace that binding for managed runs.
Keep the local transport only for deliberately selected fixtures/logical-only support, visibly not live continuity.

Implement one adapter, not multiple competing APIs. Ordinary completion uses the actual returned response `id` as
`previous_response_id` on follow-up requests. A fresh root omits that field entirely. Use function tools supplied by Forge;
Forge's manager executes their approved native handlers and returns function_call_output items correlated to call IDs.
Parse JSON arguments as JSON, never as executable code. A function call is not an action until its real handler succeeds.
Keep the authoritative tool catalog in one place; adapt schemas without duplicating implementation logic.

For Alpha, non-streaming Responses requests are acceptable first if UI cancellation/progress remain usable.
Streaming can follow through an incremental SSE parser handling split events and a terminal completed/failed response.
Do not wait for a fancy streaming renderer before wiring real inference. Parse Responses usage from the actual response
schema; do not mix Chat Completions `prompt_tokens` or native REST `stats` into a Responses parser without explicit mapping.
Add tolerant handling for benign extra provider fields; do not apply the custom transport's exact-field whitelist blindly.

Provider settings must save offline, then separately discover models and test connection. The selected loaded model's
context capacity is the relevant physical limit, not a marketing maximum. Preserve optional local bearer authentication.
This is a local LM Studio connection; no cloud account, cloud API key or additional SDK is required by this design.
Probe the deployed API/model's actual capabilities. A reachable /models response alone does not prove tool continuation.

## Context accounting and trigger
Expose an effective context capacity (in tokens) in Settings, limited to the confirmed loaded capacity.
The manager owns a per-session context monitor. Prefer actual provider observations. If exact usage is absent,
use a conservative serialized-input estimate and label the source/uncertainty in the UI; never label an estimate exact.
Include system instructions, tool schemas, retained input/output, tool results and relevant reasoning tokens.
Use the latest retained-context observation, not a sum of cumulative prompt counts that double-counts history.

Before another inference, calculate with saturating/checked arithmetic:
```
C = min(configured_effective_context_capacity, confirmed_loaded_context_capacity)
U = latest retained-context use for this provider session
I = incremental input not already included in U
R = next-response reserve + handoff reserve + estimation safety margin
rollover_required = (U + I + R >= C)
```
A reserve is part of context planning, not permission to call a fixed number of tools.
When exact serialized preflight is unavailable, avoid double-counting schemas already retained; conservative estimates
must be explicit. Do not set U to zero just because a local session ID was generated.
Validate that the fresh bootstrap itself fits within C with its response reserve. A too-small configured capacity is
an actionable settings error, not a loop that endlessly rolls over or discards the mission.
Provider overflow triggers emergency checkpoint/recovery from already durable work; do not require one more giant
summarization request to an exhausted context. Maintain incremental work state before reaching the threshold.

Large tool outputs should be persisted as artifacts, with useful excerpts and retrievable references in context.
Their byte limits prevent runaway memory; they do not ration the number of tool calls. Do not silently drop errors.
As soon as the threshold is reached, schedule the transition in the manager, not when the GUI next polls.

## Minimal durable handoff sequence
1. Quiesce new predecessor work at a completed tool boundary; retain the result of any already executed tool.
   Persist a handoff with project identity/generation, run/session IDs, mission, current files/work, decisions,
   verified results, pending effects and exact next action. Save atomically and identify its canonical digest.
2. Start a genuine fresh provider root with no `previous_response_id`. Bind it to the same project/run and a new session.
   Give it the exact handoff reference and make the context retrieval tool available. The exposed legacy name is
   `context_get` where that is the catalog contract; do not invent a `get_context` alias without implementing it.
3. Return the actual canonical handoff through the native retrieval handler. Then have the live model emit a typed
   acknowledgment carrying the handoff/run/project/session identity and digest. Validate the actual provider response
   and call correlation. A challenge/template generated by Forge is not an acknowledgment from the model.
4. Persist the acknowledgment, fence the predecessor from further tool mutations, activate the successor,
   and automatically dispatch the pending task work. Do not require an operator to copy/paste a continuity ID.
5. Preserve the durable state on failure. Retry/reconcile transient transport errors without creating a second live
   writer. Do not claim provider exactly-once inference or replay an uncertain shell effect after a crash.
   When external-effect completion is ambiguous, stop that effect for reconciliation and report it, not the whole product as completed.

Project generation/reset invalidates stale bindings. The protocol can reuse existing operation IDs/idempotency
records instead of inventing another ledger. Keep one focused test around duplicate transition observations.

## GUI truth and host boundary
Show model, actual active response/session, used/effective capacity, reserve and measurement source,
plus checkpoint/successor/acknowledged/resumed/error states. A real acknowledged-but-not-resumed state is not completed.
Ordinary LM Studio desktop chats that merely connect to the stdio MCP server are not automatically Forge-managed.
Do not reset tabs with GUI automation or undocumented endpoints. The Alpha guarantee applies to explicitly enrolled
Forge-managed runs whose context and provider requests the manager owns.

The R1 implementation now keeps ordinary run ownership in the Manager. It persists the current provider response ID,
pending function-call correlations, lifetime usage, retained context, run state, and errors; it routes function calls
through the authorized native MCP tool router and feeds retained usage to `ContinuityAutomation`. A completed handoff
returns the activated successor response ID to the same work loop. Recovered pending external work fails for explicit
reconciliation rather than being replayed blindly.

The R6 continuation preserves an admissible open session and active binding before a repository-backed run starts,
observes continuity only after completed native effects are persisted, and transfers bounded completed-work summaries
in the canonical handoff so a fresh successor can continue without repeating those effects. Manager and CLI session
ledgers live under the memory root after pre-SQLite migration of the legacy location. Real LM Studio operation
`786d0672-6231-4202-a46c-401732ade283` crossed the authoritative threshold and activated fresh successor
`7dd7aece-532a-459a-8489-ab9d62567466`; exact handoff retrieval, structured model acknowledgment, predecessor fencing,
and useful successor filesystem work passed.

The ordinary provider loop no longer has a turn limit. It continues until the task completes, the operator cancels,
a real failure occurs, or context-only continuity transfers ownership.

Manager-owned native calls carry protocol version `managed-run-v1`. `McpInvocationGuard` excludes that protocol from the legacy desktop-chat blocked-client, identical-call counter, and legacy handoff reservation because the Manager already owns context-only continuity. Project resolution, tool authorization, native invocation, and audit remain in the shared route. Ordinary MCP desktop-chat clients retain the legacy guard. Exact 0.9.4 run `34c078f8-6256-4b03-80e1-936e2e50f2f4` crossed the real threshold, completed the eight durable transition sequence to successor `4ddd93de-6cc6-413f-b5fd-90da72e074d8`, performed the post-successor effect, and returned terminal `DONE`. Focused coverage includes 12 identical managed-run calls and an 80-tool run.

## Configuration migration
Read old quota/count/time fields only long enough to ignore/drop them safely during migration.
Do not emit them in saved config, user documentation, CLI help, runtime tool descriptions or prompts.
Remove their code and obsolete settings controls, not just their labels. Keep compatibility for legitimate old
context-capacity settings and handoff formats. Inspect installed settings and both primary/fallback deployment templates.
The discovery helper flags candidates for semantic review; historical Git objects and this removal specification
are not runtime policy. Do not perform a global replacement of the word "budget".

## Evidence boundary
Required: a real context observation crosses the configured threshold; a new root is made; the model reads and
acknowledges the real handoff; a useful action continues automatically. Local IDs, fabricated acknowledgments,
fixtures, counters, and an enqueued command do not satisfy this requirement.

Official references: https://lmstudio.ai/docs/developer/openai-compat/responses and
https://lmstudio.ai/docs/developer/openai-compat/tools (checked September 10, 2026).

<!-- alpha-phase-review:start -->
Phase review: R2 telemetry parity follow-up — 2026-09-13. Implementation and verification status: [Product status](../STATUS.md).
Delivery/merge status is recorded by the linked phase pull request.
<!-- alpha-phase-review:end -->
