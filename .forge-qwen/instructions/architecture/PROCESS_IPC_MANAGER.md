# Process, IPC, and manager

## GUI

Owns windows/presentation and attaches to the manager. It does not compete for the dashboard port.

## CLI/MCP

`forge-conductor.exe serve` initializes no WinUI, reads bounded newline-delimited JSON from stdin, reserves stdout for protocol frames, uses stderr/ETW for diagnostics, and shuts down on EOF/broken pipe/cancellation/parent loss.

## Manager

One instance per canonical home/current-user SID, enforced by named mutex, lease record, authenticated control handshake, and listener ownership. It owns telemetry, dashboard, deployment reconciliation, and recovery. Startup uses packaged `StartupTask` or justified per-user Task Scheduler fallback.

## IPC

Current-user ACL named pipe, 4-byte length-prefixed UTF-8 JSON, version, request/correlation ID, deadline, nonce/token, frame/concurrency/time limits. Loopback HTTP/SSE binds only `127.0.0.1`/`::1`, uses DPAPI-protected token and same-origin/anti-CSRF checks, and never exposes privileged MCP writes.

## Child processes

`CreateProcessW`, `STARTUPINFOEX` inherited-handle allowlist, Unicode environment, redirected bounded overlapped streams, kill-on-close Job Object, deadline/cancellation, deterministic drain/wait/close.
