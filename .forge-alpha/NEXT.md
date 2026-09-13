# Windows Alpha execution cursor

**Package identity:** `windows-alpha-recovery-2026-09-12`
**Execution revision:** `continuous-delivery-correction-2026-09-12`
**Repository host:** `D:\GitHub\Forge-Conductor-Windows-Edition`
**Current phase/slice:** R7.3 — draft delivery and dependency-order synchronization
**Current branch:** `alpha/r7-delivery`, created from exact R6 PR #20 head `ad5b3fc6a9215f3fa08dccaefa14703e6183eedc`

## Verified state

- R1–R5 implementation is merged through PR #18 at `f83fe4c81c261545d70bdf8c786997ded4043dd2`; local main and origin/main match it. PR #19 preserves the narrow R5 closeout and later native evidence. Primary R6 [PR #20](https://github.com/flynn33/forge-conductor-windows-edition/pull/20) and primary R7 [PR #21](https://github.com/flynn33/forge-conductor-windows-edition/pull/21) are open explicitly as drafts.
- R6 fixed cross-profile page/project selection leakage and stale-ID use. Profile A restores exact project `51e1f9a4-5943-46f1-9797-35a275768cc3`; profile B retains zero projects and no selection.
- Candidate source commit `d8a2d68c80f2fd090aa36466a517725a0eb59445` / tree `151ccf51a9aa3d1a0b6fdcfe22af2cbf599a7c6e` produced signed `ForgeConductor-0.9.2.0-x64.msix`, SHA-256 `4f43569b45438202d10cbfb67da4e456a04d65a80bb4b33177a3c94cfb74a695`; bundle SHA-256 `18e43f5508499b56ec802447cfb98dfe8bfc048ba1f649eb1da657f0ae28f8dd`.
- Full x64 Release products built. Focused App telemetry, MCP protocol/process, and Infrastructure tests passed. Exact unpacked MSIX bytes supplied the CLI, GUI and Manager; no repository build path was used for package acceptance.
- Exact-package Manager detach/reattach, measured CPU/RAM, explicit unsupported GPU state, two-profile selection, keyboard context controls, actual High Contrast, and actual 150% Windows text size passed. Temporary OS settings, trust entries and Forge processes were restored.
- Remote `alpha/r1-managed-runs` remains retired. Its exact former ref is preserved in the verified ignored bundle under `out/alpha-evidence/branch-retirement/`; no other remote branch was deleted.
- Preserve and exclude `.forge-qwen/state/**` changes and evidence. Never use `C:\Program Files\ForgeConductor` as source.

## Next exact action

Push the R7 PR #21 reference closeout, then follow normal dependency-order review/merge for PR #19, PR #20, and PR #21. After actual merges, fetch and fast-forward local main and prove GitHub/origin/local equality. Keep R6-G1/G2 open.

## Open dependencies

- `B-R1-LMSTUDIO-OFFLINE`: TCP `127.0.0.1:1234` remained unavailable at `2026-09-13T00:20:40Z`; start a tool-capable LM Studio server for provider-originated productive successor acceptance.
- `B-R5-CENTRAL-SCHEMA-HISTORY`: authentic C008/C009 history is unavailable; preserve the schema-9 owner store and keep newer-store rejection non-destructive.
- `B-R6-INSTALL-TRUST`: `Add-AppxPackage` returned `0x800B0109`. Current-user trust is insufficient and the machine-level publisher import was rejected by automatic approval policy; approve the bundled public certificate in Local Machine Trusted People, then run install/Start/update/uninstall acceptance.

## Phase closeout obligations

R7-G1 and R7-G2 are passed. R7-G3 remains blocked until dependent PRs actually merge and main is synchronized. R6-G1 and R6-G2 remain blocked and are not waived. Preserve implementation, checks, readiness, merge and acceptance as distinct states; report ref equality separately from the intentionally dirty unrelated `.forge-qwen` working tree.

<!-- alpha-phase-review:start -->
Phase review: R7 — 2026-09-12. Implementation and verification status: [Product status](../docs/STATUS.md).
Delivery/merge status is recorded by the linked phase pull request.
<!-- alpha-phase-review:end -->
