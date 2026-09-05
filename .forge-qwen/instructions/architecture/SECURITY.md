# Security

Default deny. Trusted workspace roots derive only from explicit configuration/initialization; session/memory/handoff payloads cannot expand authority. Canonicalize via handles/final paths and reject traversal, device/UNC/ADS/reparse/junction/symlink/race escapes unless final target independently authorized.

Tool authorization evaluates client, operation, tool, roots/repository, shell policy, deadline, risk, and configured grants before side effects. Shell disabled by default, 120-second hard max, bounded streams, Job Object.

DPAPI CurrentUser protects tokens/secrets. CNG supplies random/hash. Redact authorization headers, keys, environment secrets, prompts/document bodies, private paths/content before logs, ETW, memory, handoffs, exports, and crash evidence.

Test malformed/oversized/deep JSON, SQL/cursor/import tampering, HTTP smuggling/CSRF/SSE exhaustion, named-pipe impersonation, PID reuse, untrusted Git config/hooks, command quoting, secret scanning, package capabilities, and binary analysis.
