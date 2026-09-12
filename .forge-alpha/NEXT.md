# Windows Alpha execution cursor

**Package identity:** `windows-alpha-recovery-2026-09-12`
**Execution revision:** `continuous-delivery-correction-2026-09-12`
**Repository host:** `D:\GitHub\Forge-Conductor-Windows-Edition`
**Current phase/slice:** R5.2 — complete package payload and provenance
**Current branch:** `alpha/r5-installer-data`

## Verified state

- R3 completion PR #15 merged at `1f6991d5fc60928c0a12ca348d08737fae703b1f`.
- R4 Settings PR #16 merged at `cb5a074285be0f4bd17b210c7fe0cc0505b7efab`; local main was fast-forwarded to that exact commit.
- R4 completion [PR #17](https://github.com/flynn33/forge-conductor-windows-edition/pull/17) merged at `b54525ac4aadb4847ad7685f4b9dd92c8546abc1`; local `main` and `origin/main` were synchronized to that exact ref before it was merged into the active R5 branch.
- R5 product source is committed at `428dc3b32aea920f726af3b9093572b4fde27ea5`; the current R5 branch reconciles merged main at `82312a471028f2b49d4a83845835bb3b2f447c71` before the instruction/cursor update.
- Full x64 Debug and Release products build. Focused MCP, persistence, infrastructure and PowerShell syntax checks pass. A real disposable schema-9 Manager start exited with the dedicated newer-store code 20 and left the database hash unchanged.
- Remote `alpha/r1-managed-runs` was verified at expected head `e304edeef72d9dc251a742cbefcd31693d6db423`, had no active PR, and had the same stable patch as main commit `b5ad0f4f280ac5e11b558c73ae60f8f90fd8b584`. Its exact ref is preserved in the verified ignored bundle under `out/alpha-evidence/branch-retirement/`, and only that remote branch was deleted with an expected-head lease.
- R3/R4 native walkthrough gates remain open under their original IDs because the available control surface cannot operate native windows.
- Preserve and exclude `.forge-qwen/state/**` changes and evidence. Never use `C:\Program Files\ForgeConductor` as source.

## Next exact action

Commit the existing-guidance correction and refreshed R5 cursor, regenerate the x64 Release staging manifest at the resulting committed head, then run `scripts/package.ps1 -DevelopmentSigning` and inspect the signed MSIX, unpacked payload, signer, hashes and provenance. Preserve the owner's schema-9 store and use disposable profiles only.

## Open dependencies

- `B-R1-NATIVE-UI-CONTROL`: run the consolidated R1–R4 native page/visual/keyboard/scaling/high-contrast walkthrough when native UI control is available.
- `B-R1-LMSTUDIO-OFFLINE`: start a tool-capable LM Studio server for R6 live continuity acceptance.
- `B-R5-CENTRAL-SCHEMA-HISTORY`: authentic C008/C009 history is unavailable; preserve the schema-9 owner store and keep newer-store rejection non-destructive.

## Phase closeout obligations

Finish R5 installer/data behavior, focused gates and the full active-document review. Push under verified `flynn33`, open the R5 PR explicitly as a draft against main, and keep implementation, checks, readiness, merge and acceptance distinct. A build, package, PR, compaction or phase boundary is a checkpoint; continue through R6–R7 afterward.

<!-- alpha-phase-review:start -->
Phase review: R4 — 2026-09-12. Implementation and verification status: [Product status](../docs/STATUS.md).
Delivery/merge status is recorded by the linked phase pull request.
<!-- alpha-phase-review:end -->
