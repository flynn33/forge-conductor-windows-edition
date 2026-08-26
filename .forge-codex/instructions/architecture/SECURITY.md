# Security architecture

## Workspace authority

Authorize filesystem, Git, search, shell, and document operations against explicit project roots. Canonicalize through opened handles and reject reparse-point escapes, alternate data streams where unsafe, device paths, UNC paths unless approved, and path normalization mismatches.

## Process execution

Use `CreateProcessW` with:

- explicit executable and argument vector;
- no implicit shell unless the shell tool is selected;
- inherited-handle allowlist through `STARTUPINFOEX`;
- redirected overlapped pipes;
- Job Object with kill-on-close and resource limits;
- timeout and cancellation;
- bounded stdout/stderr;
- deterministic process-tree termination;
- sanitized environment.

Shell execution is disabled by default. When enabled, prefer PowerShell 7 if explicitly configured; otherwise use Windows PowerShell. Cap at 120 seconds.

## Secrets

Use DPAPI CurrentUser scope. Never store secrets in config JSON, project memory, logs, ETW payloads, crash dumps, or handoffs. Apply redaction before persistence.

## IPC

Named-pipe ACLs permit the current user only. Loopback dashboard requires a DPAPI-protected token, origin checks where relevant, bounded requests, and no remote binding.

## Database/import

Set SQLite defensive options where available, use parameterized statements, validate schemas and checksums, cap sizes, reject path traversal, and quarantine corrupt imports.

## Installer

Use signed packages. Development signing is per-user and isolated. Do not disable SmartScreen, UAC, Defender, or certificate validation. The setup bootstrapper verifies all package hashes and publisher identity before deployment.
