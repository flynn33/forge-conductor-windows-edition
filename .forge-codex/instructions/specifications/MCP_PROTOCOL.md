# MCP protocol specification

The Windows server must preserve the macOS protocol behavior.

## Framing

- UTF-8, one JSON-RPC object per line.
- One compact response plus newline.
- Flush immediately.
- No `Content-Length`.
- No stdout diagnostics.
- Maximum input size and parse depth are bounded.
- Invalid JSON returns a protocol error without terminating the process unless framing cannot recover.

## Initialize

Return the role-specific server name, product version, protocol version negotiated according to the host request, and supported capabilities. Primary and fallback roles must have distinct identities.

## Tool list

`tools/list` returns exactly the canonical 53 tools when all standard features are enabled. Ordering must be deterministic. Tool schemas are ported from the Swift source rather than invented.

## Tool call

- Validate closed schemas where the source does.
- Enforce project/workspace authorization before dispatch.
- Enforce deadlines and cancellation.
- Audit sanitized arguments and result status.
- Convert exceptions to stable tool failures.
- Never leak secrets, native paths outside authorized scope, stack traces, or raw SQL errors.
- Continuity tools do not participate in identical-call loop blocking.
- Non-continuity progress tools update checkpoint/rollover budgets.

## Contract tests

Generate golden initialize, list, valid-call, invalid-call, timeout, cancellation, EOF, broken-pipe, oversized-input, malformed-JSON, and concurrent-process fixtures. Compare semantic JSON rather than whitespace.
