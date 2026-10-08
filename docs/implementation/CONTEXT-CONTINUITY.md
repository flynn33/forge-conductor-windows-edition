# Native LM Studio chat Auto Continuity

## Current 1.3.26 Manager observer candidate — qualification pending

The 1.3.26 candidate uses `ManagerVisibleChatContinuity` as an `IManagerTransitionWorker`. `ManagerProcessWorkerGroup` starts it after controller initialization and joins it before memory, configuration and repository services are destroyed. Internal `visible_chat.observe` and `visible_chat.status` use the existing authenticated same-Windows-user nonce pipe and 2 MiB frame bound; JSON object bodies are limited to 1 MiB with closed-field, duplicate-field and depth checks. The Primary CLI callback carries the authorized project/root, tool name, success and adapter canonical result before client-local guard annotations. Fallback and CLU do not submit competing observations. Route validation uses the verified sibling CLI from `ManagerProcessEnvironment`, rather than treating Manager.exe as the deployed MCP executable. The public catalog, schemas and 112/112/5 source inventory are unchanged.

Manager lifetime does not grant saved authority or confirm an uncertain UI effect. Current project/provider/route validation, the OS-held single-writer lease and selected-native evidence still gate control dispatch. An empty or unrelated third chat does not establish the successor. Explicit route migration requires a fresh authorized Primary callback, matching actual native packet/recovery evidence and exact preservation of the old encrypted checkpoint. Uncertain New chat or Send is reconciled without automatic replay. Completion requires the actual handed message, successor `context_get` and a following successful Forge tool result. Installed automatic delivery remains unqualified.

See [1.3.26 candidate notes](../releases/1.3.26.md) and [pending qualification](../validation/HOST-CAPABILITIES-1.3.26.md).

The 1.3.26 source candidate addresses the delivered `context_get` result comparison at native successor route recovery. The Manager callback precedes `McpInvocationGuard::afterInvoke`, which adds the client-local Boolean `context_budget_cleared`. The comparison accepts that Boolean annotation when absent from the callback and keeps every callback-owned field exact. A non-Boolean annotation, changed packet, extra unrelated field or conflicting callback-owned value remains invalid. The separately recorded 1.3.24 working-source Boolean-comparison regression passed; this historical result does not qualify the 1.3.26 candidate. Current complete release and installed native qualification remain pending.

## Superseded 1.3.23 recovery observation

The installed, unpublished 1.3.23 candidate at source `b42a80df7db637337ea60737ef8e1049159cf448`, tree `a475d6d04e5a70b32bb72390afc7347d2d97ff65`, passed Product All, three static gates, persistence and **167/167 Release tests in 95.75 seconds**. Its signed 1.3.23.0 package matched all four executable images and all **323 payload files**; prelaunch comparison preserved **8,471 files / 412,433,227 bytes** and four protected snapshots. Two actual Primary calls in the selected Qwen response returned the saved packet and 1.3.23 status, and generation reached `eosFound`. Manager still reported `recovery_pending`: the delivered `context_get` result had only the later `context_budget_cleared:false` annotation absent from its callback. No new 1.3.23 route-recovery archive or completed route reconciliation was captured. Automatic New chat/Send, full installed catalogs, integrated public/native image acceptance, CI and release qualification remained incomplete. No 1.3.23 release was published.

## Superseded 1.3.22 native observation

The superseded installed 1.3.22 candidate at source `9178abdf12998af56df4860a56b25dccfcc30e10`, tree `b5e9fdedc9cb6b7ea0f97f9d93ef2e2d2f313d64`, passed Product All, all three static gates, package persistence and **166/166 Release tests in 97.71 seconds**. The signed 1.3.22.0 installation matched all four executable payloads and preserved **8,431 files / 410,168,202 bytes** and the four protected snapshots before any launch. Actual selected Qwen Primary results reported version 1.3.22 with 112 tools and saved a successful model-written handoff at sequence 8. The original encrypted revision-2 checkpoint was preserved exactly before revision 3 entered Creating. The later revision 4 retained an uncertain New chat effect; the selected new chat had zero messages and no handed packet. All three CLI handles and their three native bridge parents subsequently reported exit code 1. Automatic packet delivery and rollover did not complete. A later manually seeded Primary context_get/get_forge_status attempt advanced the checkpoint to revision 6/resumed with the same packet, confirmed New chat, delivery/context-recovery flags true and the old request acknowledgement still false. That assisted readback does not establish automatic packet Send, and the watch did not capture revision 5. Complete installed 112/112/5 catalog qualification and integrated public/native image-provider acceptance were not established. These results do not qualify 1.3.24.

The shared native snapshot reader retained the 64 MiB bound. The final source suite passed, but captured stdio and bridge exit codes do not establish why those processes exited. The source lifetime was tied to Primary server.run returning; the 1.3.26 candidate changes that ownership boundary. The native effect guard remains required even when the observer survives a client.

Exact automatic delivery was not captured: checkpoint revision 4 was observed at 2026-10-08T05:32:58.591238Z with an uncertain delivery/new_chat effect; the selected conversation 1791437581203 had zero messages. The completed predecessor contained 84 messages, final eosFound, and its complete original 78-message prefix was preserved. A 30-minute passive watch copied three checkpoint versions with zero read errors. See [retained evidence](../validation/HOST-CAPABILITIES-1.3.22.md).

The next sections retain the earlier 1.3.21 ownership and fixture records. They describe their named artifacts; current Manager lifetime and installed recovery require separate 1.3.26 evidence.

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

## 1.3.26 cached prompt pressure candidate

The cached `tokenCount` projects LM Studio's complete rendered prompt. Continuity evaluates the larger of admitted cached count and actual latest provider usage at initial observation and again after a confirmed pause. `cached_rendered_prompt_tokens`, `pressure_tokens`, `pressure_source` and `pressure_headroom_tokens` remain separate from actual `tokens_used` and `headroom_tokens`. Cache admission requires the current selected generation/model identifier and equal loaded capacity; unknown, malformed or mismatched evidence is not admitted. LM Studio refreshes this cache around its outer prediction and does not attach a generation timestamp. Physical overflow remains distinct. Final source checks and installed automatic New/Send/full packet read/following Primary proof are pending.
