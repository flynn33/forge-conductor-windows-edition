# P12-002: Native Session Ledger Privacy and Recovery

Status: Accepted

Date: 2026-08-26

## Context

Native create and bootstrap calls are external effects. Process loss can occur
after either effect but before its result is published. The macOS ledger
preserves compact lifecycle identifiers, but does not validate duplicates,
cross-field invariants, or a content checksum. Architecture also asks for
bounded message persistence, while the higher-precedence source behavior and
product rules prohibit retaining complete transcripts or secrets in this
ledger.

## Decision

- Persist a schema-1, revisioned, checksum-bound snapshot with at most 4,096
  records. Each record contains only project, operation, predecessor, logical
  and provider session IDs, model, idempotency key, status, handoff ID and hash,
  bounded usage, timestamps, and the owning cancellation operation.
- Never persist the canonical handoff, mission, next-action text, transcript,
  provider response body, or provider secret in the native ledger. Canonical
  handoffs remain in the project continuity repository. Bounded provider
  message envelopes can be added later behind their own project-scoped store.
- Commit `Creating` before provider creation and `Bootstrapping` plus the exact
  handoff identity before bootstrap. Publication uses revision compare-and-swap,
  atomic same-directory replacement, a sibling backup, and a SHA-256 over the
  canonical checksum-free document.
- Missing storage loads an empty revision-zero ledger. Future schemas,
  oversized input, invalid UTF-8, duplicate identifiers, partial bindings,
  reversed timestamps, invalid states, checksum mismatches, and cross-project
  aliases fail closed. A valid backup may recover a corrupt primary.
- Capacity pruning is deterministic and removes terminal failed, cancelled, or
  sealed records only. Active idempotency evidence is never evicted to make a
  write succeed.
- Adapter and provider cancellation sets are capped at 256. Provider response
  handling is capped at 256 chunks, 16 KiB per chunk, and 256 KiB total, with
  no negative usage accepted.

## Consequences

Restart can reconcile a pending create without changing the logical successor
ID and can ask the project coordinator to replay a pending bootstrap from the
canonical project handoff. Compact durable state supports recovery without
creating a second transcript store.

Security campaign work remains deferred under the owner-approved alpha scope;
these identity, privacy, corruption, cancellation, and bound checks remain
functional correctness requirements and are not deferred.

## Evidence basis

- `.forge-inputs/macos/Forge-Conductor-MacOS-main/Sources/ForgeNativeSessionHostPlugin/ForgeNativeSessionHostPlugin.swift`
- `.forge-inputs/macos/Forge-Conductor-MacOS-main/Tests/ForgeConductorTests/NativeSessionHostPluginTests.swift`
- `.forge-codex/state/decisions/OWNER-002-alpha-release-qualification-scope.md`
- `.forge-codex/instructions/specifications/CONTINUITY_TOOL_CONTRACT.md`
