# P10-003: Typed Agent Semantics and the P10/P14 Boundary

Status: Accepted

Date: 2026-08-26

## Context

The macOS `AgentToolPack` combines aliases, dictionary parsing, response
envelopes, catalog calls, lifecycle calls, and tool registration. P10 requires
the underlying catalog and durable lifecycle, while P14 owns MCP transport,
router, schema, audit, and loop protection. Implementing wire dictionaries in
P10 would duplicate parsing and invert the intended dependency direction.

The P05 interfaces provide source-shaped session values but do not represent
the full start card, status reminder/binding, structured completion report, or
restart/reattach operations needed by later continuity and MCP phases.

## Decision

- P10 adds bounded transport-neutral request and outcome values for start,
  rich status, completion, ownership transfer, run projections, and recovery.
  Existing P05 methods and the exact capability-bearing `status` signature are
  retained while consumers migrate to the richer operations.
- P10 owns catalog selection, one-open-run-per-client behavior, active binding,
  idle calculation, stale pruning, report-schema validation, persistence
  transitions, and restart reconstruction.
- Status remains a mutation because it may transfer ownership and touch durable
  state. It requires the P05 write authority and immutable authorized-call
  capability, including caller, project, correlation, effect, and authority
  generation binding. The canonical call must contain exactly the requested
  top-level `session_id`. A run with a durable project requires an exact project
  match. A retained P05 run without a project is compatible only when it has a
  durable working directory accepted by the injected workspace-authority path
  resolver under an immutable trusted root; a lexical prefix check is not
  sufficient because Windows reparse points can redirect a path. Projectless
  and pathless runs cannot be transferred.
- Completion requires an existing run and an owning client or an explicitly
  trusted internal path. It never deletes another client's active pointer.
  Unknown agents and sessions use stable `agent_not_found` and
  `session_not_found` errors; ownership compare-and-swap failures use a stable
  conflict error.
- Completion checks required keys in declared schema order. Absent values and
  exact empty strings, arrays, or objects are missing; nonempty values, false,
  and zero are present. Missing keys produce a successful closed run with a
  warning outcome rather than preventing closure.
- Completion never trusts caller-supplied field metadata as evidence about the
  persisted report text. The Application service obtains an independent field
  projection through `IAgentCompletionReportInspector` and rejects any
  disagreement. The Windows implementation performs strict canonical JSON
  inspection with the approved nlohmann/json dependency below the Application
  layer, rejects duplicate object keys at every depth, and caps JSON nesting at
  64. P14 still owns wire parsing and schemas; the P10 inspection is a
  trust-boundary invariant check before durable mutation.
- Completion summary contains goal, report, and missing keys and is truncated
  at a UTF-8-safe 4,000-unit boundary. Goal, path, report, schema, list, and
  projection sizes are validated before serialization or database work.
- The in-process client-binding cache holds at most 128 entries. Inserting a
  new client at capacity evicts the lexicographically smallest other client,
  matching the deterministic source policy. Durable recovery remains possible
  for every evicted entry.
- P14 owns the exact seven agent MCP names, argument aliases, JSON Schemas,
  canonical JSON-RPC envelopes, error-to-wire mapping, request replay/loop
  protection, request-level redacted audit serialization, and registration.
  P10 exposes no MCP descriptor or transport code and makes no G14 claim.
- Catalog tool lists are constraints, never capability grants. An empty primary
  list is deny-all, and shell remains controlled by the separate global policy.

## Consequences

P11 can recover and transfer typed active bindings without downcasting a
concrete service. P14 can adapt one canonical parse into the exact wire format
without reproducing lifecycle rules. Retained P05 tests continue protecting
capability binding while G10 tests cover the richer behavior.

## Rejected alternatives

- Returning untyped JSON dictionaries from Application: rejected because it
  couples business behavior to the future MCP adapter.
- Treating status as read-only: rejected because touch and ownership transfer
  mutate durable state.
- Allowing completion by session identifier alone: rejected because that would
  preserve a source ownership weakness.
- Closing only schema-complete reports: rejected because the source deliberately
  closes abandoned work while warning about missing keys.

## Evidence basis

- `.forge-inputs/macos/Forge-Conductor-MacOS-main/Sources/ForgeConductorCore/Application/Tools/AgentToolPack.swift`
  — SHA-256 `df7e04af91142c8f9d1c9f6a27acc6b7f4ddb44d9c4d1009df98035805ed19bd`
- `.forge-inputs/macos/Forge-Conductor-MacOS-main/Sources/ForgeConductorCore/Application/AgentSessionService.swift`
  — SHA-256 `9b9dc37ee186e5d195484fbabd1d5cef25f701675d1e60e082e18d06f9d0cb00`
- `.forge-codex/state/decisions/P05-001-typed-results-operation-context-and-authority.md`
- `.forge-codex/state/decisions/P05-002-source-compatibility-and-windows-resource-bounds.md`
- `.forge-codex/instructions/plans/mcp-tool-parity.json`

