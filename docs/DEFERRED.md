# Explicitly deferred work
Deferred does not mean implemented, removed without notice, or an Alpha failure hidden in another milestone.

**B1 — Remaining behavioral parity:** richer optional web dashboard, high-fidelity GPU/Metal visual equivalents,
advanced connector/runtime breadth not required for the primary local workflow, portability/import between platforms,
and any unverified per-tool schema/behavior deltas recorded during R3 workflow reconciliation.
The optional tools already working should remain available. A required filesystem/shell/memory/provider capability
cannot be moved here just because it needs integration. Mac-only unavailable/protected filesystem operations must
be distinguished from working features rather than counted as port regressions.

**B2 — Broader distribution/hardening:** ARM64, Store/public release operations, a new security-hardening program,
extensive filesystem attack/race qualification, independent security validation, long stress/leak/performance campaigns,
extraordinary power-loss/exactly-once proofs, automatic update service and a broad signed hardware matrix.

Useful existing cancellation, project scoping, atomic writes, redaction, consent for destructive resets and installer trust
remain in place. Deferring hardening does not mean making the application destroy data or disabling Windows protection.
No recurring quota-based run controls are deferred: their removal is required now by the owner.

<!-- alpha-phase-review:start -->
Phase review: R6 continuation — 2026-09-12. Implementation and verification status: [Product status](STATUS.md).
Delivery/merge status is recorded by the linked phase pull request.
<!-- alpha-phase-review:end -->
