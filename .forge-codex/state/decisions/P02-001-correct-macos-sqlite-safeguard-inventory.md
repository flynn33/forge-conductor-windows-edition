# P02-001: Correct macOS SQLite Safeguard Inventory

Status: Accepted

Date: 2026-08-25

## Context

The accepted P02 persistence inventory attributed a pre-migration backup and
`quick_check` to both macOS SQLite implementations. That was not faithful to
the immutable 0.9.0 source.

`ProjectMemoryRepository` enables WAL before it calls `migrateUnlocked`, copies
only `databaseURL` to a pre-migration artifact, and runs `PRAGMA quick_check`
after migration. The copy is a raw main-database-file copy: it neither uses the
SQLite backup API nor checkpoints or copies the WAL. It therefore is not a
WAL-safe backup.

`SQLiteStore` enables WAL and then calls its transactional migration. A
source-wide audit finds no pre-migration copy, SQLite backup API, checkpoint,
`quick_check`, or `integrity_check` path in that implementation.

## Decision

- Record the central store's `backup` and `integrity` behavior as `none`.
- Retain the project store's `backup` as `pre-migration` and its `integrity` as
  `quick_check`, while recording `raw-file-copy`, `main-database-file-only`, and
  `backup_wal_safe: false` explicitly.
- Keep the inventory schema at version 1. The existing string-valued fields
  retain their types; the project backup qualifiers are additive source facts.
- Preserve the previously accepted P02/G02 reports, results, ledger, and command
  records as history. The corrected generator and regenerated current inventory,
  together with this decision, supersede only the two inaccurate safeguard
  claims.

## Consequences

The macOS project repository remains evidence for backup timing and post-migrate
integrity checking, but not for a correct WAL-aware backup mechanism. The macOS
central store is not evidence for either safeguard. Windows persistence work
must satisfy the target backup and integrity requirements independently and
must not reproduce the raw-copy weakness.

## Source evidence

- `.forge-inputs/macos/Forge-Conductor-MacOS-main/Sources/ForgeConductorCore/Infrastructure/ProjectMemoryRepository.swift`
  (SHA-256
  `0B6C85105B374A6275AFCCD735FC72B2938DDB38358753206A912ACA606E9042`):
  WAL is enabled before migration at lines 643-654; the pre-migration artifact
  copies only `databaseURL` at lines 674-681; the post-migration check is at
  lines 757-758 and its `PRAGMA quick_check` implementation is at lines 946-949.
- `.forge-inputs/macos/Forge-Conductor-MacOS-main/Sources/ForgeConductorCore/Infrastructure/SQLiteStore.swift`
  (SHA-256
  `A5C7EC5750BE9C5342DBC9FE5C1ADDE8E6C5A1F57D3009681B5AC1FB751F5CA0`):
  WAL is enabled before `migrate()` at lines 50-65, and the complete migration
  implementation at lines 132-216 has neither backup nor integrity checking.
  A source-wide case-insensitive search returned zero occurrences for
  `quick_check`, `integrity_check`, `copyItem`, `sqlite3_backup`,
  `wal_checkpoint`, `pre-migration`, and `backup`.

## Validation evidence

- Corrected baseline regeneration:
  `.forge-codex/state/commands/20260826T020830417Z-6a9c87dc.json`
  (exit 0; record SHA-256
  `DD2B60714FC7C914DFA25D3A6D2E622270C24A08E52B2722B238833C05AE1150`;
  stdout SHA-256
  `68090AF75ED17A9F7674415D993300E0E5FDF2038609FBA90594308FDE52B0D1`).
- Source-specific safeguard validation:
  `.forge-codex/state/commands/20260826T021034155Z-28d8fad7.json`
  (exit 0; record SHA-256
  `CF6E21956253E461EF17FEEFEA366C7FB086C8311A18066AC8CD177FC84F8685`;
  stdout SHA-256
  `9A5041E1DD9B8954117D6AF0A502A7D1119E2C8ADEF3858D4DF5F204F537496C`).
  Its measured output records no central backup or `quick_check`, and records
  the project pre-migration raw main-file copy as not WAL-safe plus a passing
  project `quick_check` fact.
- Canonical P02/G02 baseline validation:
  `.forge-codex/state/commands/20260826T021042683Z-393be6de.json`
  (exit 0; record SHA-256
  `D8EE3322819EAA1CAABBB3040904E4930B677662C6BF6D1309F3903A531A7AB2`;
  stdout SHA-256
  `6CFF25EC084BED4A26D92A89693D34C364716A76829E3723144F4936E82C9EFE`).
  It re-hashed the immutable sources and passed all P02 inventory checks.
- The first focused validation attempt is retained at
  `.forge-codex/state/commands/20260826T020955368Z-ae6e3a9e.json`
  (exit 1; record SHA-256
  `7DE94DF17721E6EA6A27EF35E52D6084F3565AD3795462D7D5442AB5393EFAB2`).
  The Windows PowerShell child lacked `Get-FileHash`; the successful rerun used
  the repository's `Get-FileSha256` helper. No source assertion failed.
- Final corrected artifacts:
  `.forge-codex/scripts/Generate-P02Baseline.ps1` (SHA-256
  `08B2124C6B6B2B688542587CF3025EE6D19120A7C34E4AB091A9D911D4825FD6`)
  and `.forge-codex/state/baseline/p02-persistence-data-inventory.json`
  (SHA-256
  `9C107884865BB66C6CF643B78C81FE3326937A1F934604E11687D72A56088639`).
