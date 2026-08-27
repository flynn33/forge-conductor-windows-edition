# P14-002: Bounded MCP Server and Windows Stdio Lifetimes

Status: Accepted

Date: 2026-08-26

## Context

P14 must expose the catalog and codec from P14-001 through a real JSON-RPC
server and native Windows stdio transport without introducing unbounded work,
ambiguous workspace authority, or teardown races. Requests can arrive while a
tool call is active, cancellations can precede their calls, and synchronous
Windows handles can remain blocked even after cancellation is requested.

The protocol boundary must also remain usable after a malformed or oversized
frame when the newline boundary is recoverable. Router implementations are
separate injected components, so their returned payloads cannot be trusted to
fit the MCP wire limits merely because the tool call itself succeeded.

## Decision

- The MCP server owns one receive loop and one serial tool-call worker. Its
  pending-call queue is capped at 64 entries. Pre-cancellation state is FIFO
  bounded to 256 identifiers; external request identifiers are capped at 256
  UTF-8 bytes, and JSON-RPC method and tool names are each capped at 128 UTF-8
  bytes.
- Every tool call passes through an explicitly injected per-call authority
  resolver before reaching the injected router. Cancellation and deadline
  state are checked at the resolver/router boundaries, and neither component
  is discovered through a service locator.
- Incoming records are validated by the codec before protocol dispatch. The
  shared limits remain 1,048,576 bytes and 64 nesting levels. Malformed,
  invalid, and oversized records produce bounded protocol errors and processing
  resumes with the next recoverable newline-delimited frame.
- Tool-call parameters, tool names, catalog membership, and `project_id` type
  are validated before routing. Router output is codec-validated and
  canonicalized as a JSON object within the shared document and nesting limits.
  Invalid or oversized router output is replaced with a stable bounded error
  envelope; response serialization has a second bounded failure fallback rather
  than echoing attacker-controlled names or payloads.
- `WindowsStdioMcpTransport` accepts only Win32 pipe or disk handles and uses
  exactly one UTF-8 JSON record per LF-terminated frame. It neither implements
  nor accepts `Content-Length` framing, flushes each response, and reserves
  stdout exclusively for protocol responses rather than diagnostics.
- Each potentially blocking read or write runs in a temporary owned worker with
  heap-owned operation state. Normal completion, cancellation, deadlines, and
  shutdown signal the operation and join that worker. If a broken synchronous
  handle provider still does not return within a one-second cancellation grace,
  the transport is poisoned, rejects further admission, and the worker detaches
  while retaining the implementation that owns all referenced state. This is a
  bounded fail-forward path: at most one read and one write can be outstanding,
  and the future `serve` composition must create only one transport per process.
- A failed or interrupted write poisons the transport because the boundary
  cannot prove that zero bytes reached stdout. Reusing that stream could emit a
  corrupt JSON record, so subsequent sends fail closed.
- Server and transport destructors request shutdown and drain their active
  owned operations. The sole exception is the already-poisoned retained-self
  path for a provider that violates the bounded cancellation grace; its detached
  worker owns its complete lifetime until the provider returns and cannot admit
  more work.

The source-description delta for the Windows PowerShell-backed `shell_exec`
tool is governed by P14-001 and is not reopened by this lifetime decision.

## Consequences

The protocol layer can accept cancellations while one tool call is executing,
reject excess calls deterministically, recover after independently framed bad
input, and shut down without leaving ordinary server or transport work owned by
destroyed stack objects. A provider that fails to release a synchronous Win32
operation cannot block process teardown indefinitely or permit further writes
to an indeterminate stream.

This slice does not complete P14 or pass G14. Typed tool-pack adapters, the
production router, audit persistence, loop prevention, CLI and process
composition, and real-process stdio fixtures remain required. Their absence
must not be presented as end-to-end MCP parity.

## Evidence

- `include/ForgeConductor/Mcp/McpServer.h`
- `src/Mcp/McpServer.cpp`
- `include/ForgeConductor/Mcp/WindowsStdioMcpTransport.h`
- `src/Mcp/WindowsStdioMcpTransport.cpp`
- `tests/Mcp/McpProtocolServerTests.cpp`
- `tests/Mcp/McpStdioTransportTests.cpp`
- `.forge-codex/state/decisions/P14-001-mcp-catalog-codec-and-wire-precedence.md`
- `.forge-codex/state/commands/20260827T024149435Z-0ccadfb5.json`
- `.forge-codex/state/evidence/P14/mcp-server-stdio-checkpoint.json`
- `.forge-codex/instructions/specifications/MCP_PROTOCOL.md`
- `.forge-codex/instructions/plans/resource-budgets.json`
- `.forge-codex/instructions/plans/gates.json`
