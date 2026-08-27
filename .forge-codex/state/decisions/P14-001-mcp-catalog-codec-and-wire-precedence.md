# P14-001: MCP Catalog, JSON Codec, and Wire Precedence

Status: Accepted

Date: 2026-08-26

## Context

P14 must turn the existing MCP contracts and all previously qualified typed
services into the product's real stdio protocol surface. The macOS 0.9.0 source
is the behavioral source for tool names, descriptions, schemas, aliases,
defaults, role identities, envelopes, and errors. Higher-precedence Windows
plans replace its optional `Content-Length` framing and 4 MiB default with one
bounded newline-delimited protocol.

The existing `ForgeConductor.Mcp` target is an interface placeholder. The
repository has no production catalog, JSON codec, server, transport, router,
tool-pack adapter, audit repository, or `serve` composition. The 53 underlying
typed service operations are already present and remain outside the protocol
layer.

## Decision

- P14 begins with an immutable application-owned catalog and deterministic JSON
  codec. The catalog is injected through `IToolCatalog`; protocol code does not
  discover handlers or services through a locator.
- The advertised catalog contains exactly 53 unique names in ordinal
  alphabetical order. Primary and fallback roles share the same catalog and
  behavior. Only their process identity and server name differ.
- Tool effects follow source behavior: `agent_run_status` and
  `project_memory.export` are writes even though the initial parity plan labeled
  them reads. The resulting inventory is one status, 22 reads, and 30 writes.
- Schemas preserve the macOS descriptor shapes and openness. Ten legacy tools
  retain their explicit permissive object schema; other legacy schemas omit
  `additionalProperties`; project-memory and lifecycle schemas are closed.
- User-visible descriptions remain source-compatible. The shell descriptor says
  PowerShell rather than bash because the Windows product's typed shell adapter
  executes bounded PowerShell; advertising bash would be observably false.
- The Windows `project_memory.search` and
  `project_memory.list_recent` `kinds` arrays advertise `maxItems: 100`, as
  already decided by P05-002 and enforced by the typed Windows boundary.
- JSON is parsed and emitted only through nlohmann/json from the pinned vcpkg
  dependency. Compact output uses deterministic ordinal key ordering and strict
  UTF-8 conversion. Duplicate object keys, excessive nesting, non-object
  request roots, embedded NUL, and records over 1,048,576 bytes fail closed.
- The stdio transport accepts exactly one UTF-8 JSON object per line. Output is
  one compact object followed by LF and is flushed immediately. `Content-Length`
  is rejected and diagnostics never use stdout.
- A malformed JSON line produces JSON-RPC parse error `-32700` and the server
  resumes at the next newline when framing remains recoverable. Clean EOF is
  distinct from a malformed or empty frame and starts deterministic shutdown.
- Supported protocol versions, newest first, are `2025-11-25`, `2025-06-18`,
  `2025-03-26`, and `2024-11-05`. A supported request is echoed; a missing,
  blank, or unknown version selects `2025-11-25`.
- The catalog and codec are a coherent build-and-test slice. Tool adapters,
  explicit per-call authority resolution, routing, loop protection, audit,
  concurrent request ownership, Win32 stdio, and CLI composition follow as
  separate slices without weakening these wire contracts.

## Consequences

The Windows server deliberately rejects macOS-compatible `Content-Length`
input. This resolves the behavioral conflict in favor of the direct Windows
protocol specification and P05-002; it is not an omitted feature.

Catalog tests can prove exact names, ordering, effects, descriptions, and
schemas without invoking a service or starting a process. Codec tests can prove
canonicalization, negotiated versions, hostile-input rejection, and bounds
without coupling JSON to Domain or infrastructure resources.

Passing this slice does not pass G14. G14 also requires typed tool dispatch,
authority, audit, loop/cancellation behavior, real newline stdio, CLI `serve`,
and independent primary/fallback process evidence.

## Evidence

- `.forge-codex/instructions/specifications/MCP_PROTOCOL.md`
- `.forge-codex/instructions/architecture/MCP_AND_LM_STUDIO.md`
- `.forge-codex/instructions/plans/mcp-tool-parity.json`
- `.forge-codex/instructions/plans/resource-budgets.json`
- `.forge-codex/state/baseline/p02-mcp-semantic-inventory.json`
- `.forge-codex/state/baseline/p02-schema-inventory.json`
- `.forge-codex/state/decisions/P05-002-source-compatibility-and-windows-resource-bounds.md`
- macOS `MCPServer.swift`, `ToolRouter.swift`, `ProjectMemoryToolPack.swift`, and
  `ContinuityLifecycleToolPack.swift`
