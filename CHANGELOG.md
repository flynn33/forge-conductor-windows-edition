# Changelog

## Unreleased — Windows Alpha recovery

### Implemented foundation

- Added the native C++20/WinUI 3 desktop host, per-user Manager attachment, typed provider settings, and isolated `--alpha-root` development profile support.
- Added real loopback LM Studio `/v1/models` and `/v1/responses` bootstrap transport, actual response-ID chaining, tool-call correlation, usage accounting, and Manager-owned continuity automation lifetime.
- Removed count- and time-triggered rollover behavior; context consumption is the only automatic continuity trigger.
- Verified disposable project registration, MCP deployment, filesystem/Git/shell/memory operations, project isolation, and preservation of foreign MCP entries through existing native services.
- Added signed x64 engineering MSIX/ZIP packaging with public certificate material and install guidance.

### R0 — Current baseline and accountable delivery

- Adopted replacement plan `windows-alpha-recovery-2026-09-12` without resetting merged foundation work or local Forge Qwen runtime evidence.
- Reconciled merged PR #2 and the real GitHub main, created R0–R7 milestones and phase issues, and migrated the execution ledger to R phase semantics.
- Reconciled all active first-party documentation and classified prior P0–P6 guidance and audit records as historical reference.

### Remaining Alpha work

- Complete the typed Manager-owned ordinary run path and productive continuity wiring in R1.
- Complete native telemetry, operational pages, persistent accessible Settings and scoped reset, installer/data behavior, and real installed/live acceptance in R2–R7.

<!-- alpha-phase-review:start -->
Phase review: R0 — 2026-09-12. Implementation and verification status: [Product status](docs/STATUS.md).
Delivery/merge status is recorded by the linked phase pull request.
<!-- alpha-phase-review:end -->
