# Process model and IPC

## Processes

### GUI

The GUI never starts a second dashboard listener. It connects to the manager and renders snapshots.

### Manager

A per-user manager process owns:

- dashboard listener;
- telemetry sampling;
- LM Studio deployment reconciliation;
- durable continuity recovery;
- bounded maintenance;
- health/status publication.

Enforce one instance per user with a mutex whose name includes a stable hash of the current SID.

### MCP server

`forge-conductor.exe serve` runs one unbuffered newline-delimited stdio JSON-RPC server. It writes no banners or logs to stdout. Diagnostics go to ETW/file logs.

### Session host

Owns logical model sessions and autonomous rollover. It communicates with the manager through a current-user named pipe and with model providers through supported HTTP APIs.

## Named pipes

- Create with an ACL limited to the current user SID and LocalSystem when needed.
- Use message framing: 4-byte little-endian length followed by UTF-8 JSON.
- Cap request and response frames.
- Require protocol version, request ID, correlation ID, deadline, and authentication nonce.
- Reject unknown fields where contracts require closed schemas.
- Support cancellation and server shutdown.
- Never deserialize directly into executable commands.

## Loopback dashboard

Bind only to `127.0.0.1` and `::1`. Generate a DPAPI-protected bearer token. Enforce request limits, timeouts, header/body size limits, connection limits, and idle expiration.

## Startup

The installer registers the manager through a per-user Task Scheduler logon task or an approved packaged startup mechanism. Registration is idempotent and reversible. The Manager settings page can enable, disable, repair, restart, and inspect startup state.
