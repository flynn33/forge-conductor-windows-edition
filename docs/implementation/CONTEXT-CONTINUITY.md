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
Phase review: R5 — 2026-09-12. Implementation and verification status: [Product status](../STATUS.md).
Delivery/merge status is recorded by the linked phase pull request.
<!-- alpha-phase-review:end -->
