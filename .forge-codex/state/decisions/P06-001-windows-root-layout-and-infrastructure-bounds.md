# P06-001: Windows Root Layout and Infrastructure Bounds

Status: Accepted

Date: 2026-08-25

## Context

P06 introduces native Windows paths and the first durable platform services. Every collection, encoded document, process input, and retained diagnostic must have an owner-visible bound. The application also needs one deterministic per-user root without hidden environment lookup in production.

## Decision

- The default root is `%LOCALAPPDATA%\Forge Conductor`, obtained with `SHGetKnownFolderPath(FOLDERID_LocalAppData)` and released with `CoTaskMemFree`.
- `FORGE_CONDUCTOR_HOME` is a development/test override only. It is sampled once and honored only when an injected `allowEnvironmentOverride` option is true. An explicit injected root takes precedence.
- Owned directories are derived beneath the root: `config`, `projects`, `logs`, `exports`, `handoffs`, `session-host`, `manager`, `deployment-backups`, `evidence`, `cache`, and `agents`.
- App-owned resolution rejects malformed UTF-8, relative roots, UNC and device paths, alternate data streams, reparse-point escapes, and case/normalization mismatches. The complete public `IFileSystem` tool adapter remains assigned to P13.
- Configuration documents are schema 1, at most 2 MiB, and at most 32 JSON levels deep. Two MiB is required because 32 allowed roots of 32 KiB already consume one MiB before JSON overhead.
- Atomic-file payloads are at most 32 MiB.
- Secure storage admits 128-byte keys, 64 KiB secrets, and 128 entries. Existing keys may be overwritten at capacity; a distinct 129th key fails with `limit_exceeded`.
- Diagnostic rings retain at most 4,000 records and no more encoded bytes than the active profile's single-log-file byte cap.
- Process supervision admits 64 concurrent operations, with no waiting admission queue. Per-operation argv/environment caps are defined by P05.
- The final command line is capped at 32,767 UTF-16 code units including its terminator. Forge separately caps the final Unicode environment block at 32,767 code units including terminators; that environment-block value is a Forge workload limit, not a claimed modern `CreateProcessW` total-block ceiling.
- Count overflow returns `limit_exceeded`; byte or encoded-size overflow returns `payload_too_large`; malformed values return `invalid_request`.

## Consequences

All significant P06 memory, registry, disk, and process surfaces are finite and public. Later adapters must reuse these constants rather than add hidden limits.

## Rejected alternatives

- A 1 MiB configuration cap: rejected because a valid maximum-root configuration can exceed it before JSON overhead.
- Deriving process admission from manager thread budgets: rejected because 8/10/12 is not a governing resource limit and breaks source-backed 12-way and 48-rapid-run parity.
- Treating environment lookup as implicit production configuration: rejected because it introduces hidden ambient authority.
- Counting diagnostic records without encoded bytes: rejected because record count alone does not bound retained memory.

## Evidence

- `.forge-codex/instructions/plans/resource-budgets.json`
- `.forge-codex/instructions/architecture/SECURITY.md`
- `.forge-codex/instructions/architecture/OOP_AND_OWNERSHIP.md`
- `.forge-codex/state/decisions/P05-002-source-compatibility-and-windows-resource-bounds.md`
