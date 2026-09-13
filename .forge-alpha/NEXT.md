# Windows Alpha execution cursor

**Package identity:** `windows-alpha-recovery-2026-09-12`
**Execution revision:** `continuous-delivery-correction-2026-09-12` plus owner internal-Alpha completion goal
**Repository host:** `D:\GitHub\Forge-Conductor-Windows-Edition`
**Current phase/slice:** R6.1 — installed lifecycle prerequisite and acceptance
**Current branch:** `alpha/r6-internal-alpha-completion`, based on synchronized main `7ad26a931620285d9cf2b1dabfdc1ea7fae474a8`; draft [PR #25](https://github.com/flynn33/forge-conductor-windows-edition/pull/25)

## Verified state

- PR #24 merged normally at `7ad26a931620285d9cf2b1dabfdc1ea7fae474a8`. Local main, `origin/main`, and GitHub main matched that commit before this branch. The reviewed `alpha/r2-telemetry-parity` head `66b8228d358d0f951c309eaa6973c69ed7ffac5b` is preserved in a verified local Git bundle and the remote branch is absent.
- R2 telemetry implementation and gates R2-G1/G2/G3 pass. The exact 0.9.5 candidate rendered live logical CPU/frequency, RTX 4090 engines, scoped GPU memory, disk/volume/process/workflow measurements, cadence/freshness, bounded histories, disconnect/reconnect, keyboard focus, High Contrast, and a repaired top-to-bottom 150% text layout.
- The retained 0.9.4 candidate and replacement 0.9.5 candidate `out/dist/candidate-0.9.5.0-20260913-144427` provide a genuine increasing-version path. The replacement was built from commit `c77c45386b25d7b76270c3685b79c172f41526c8` and tree `2bb16d60c7616f3d6f31ec94c85192dfe30db349`; its MSIX SHA-256 is `9c772fcc9646f1e876f83c59c9e59a189f6f6881bcd603eb290dd589d5129a74` and ZIP SHA-256 is `2874a1cdf51d6861e780a32b599172f9ca0d84fe9ad9a9bb5eb92b55e3bb8dd4`.
- A fresh non-elevated prerequisite check found publisher thumbprint `0AC803FF3292A2C736B1CBB31AFF88418983B992` in Current User Trusted People but not Local Machine Trusted People. The current user has no registered `ForgeConductor.Windows.Alpha` package, the all-users package query is access denied, and local account `.\ForgeAlphaTest` does not exist.
- `scripts/alpha/Install-Engineering.ps1` now supports a read-only `-PreflightOnly` check, reports the missing machine trust before deployment, and writes timestamped `install-result-*.json` receipts so install/update/reinstall evidence cannot overwrite itself. Syntax validation passes. The exact distributed helper passed hash/signer validation and returned the expected precise trust failure before deployment.
- The owner profile and schema-9 database remain untouched. Unrelated `.forge-qwen/state/**` work remains uncommitted and excluded.

## Next exact action

An authorized administrator must verify and import `out/dist/candidate-0.9.5.0-20260913-144427/Publisher.cer` into `Cert:\LocalMachine\TrustedPeople`, then create or designate an explicitly disposable Windows test account. Sign in to that account and run the replacement candidate helper with `-PreflightOnly`; if it reports `ready_for_install: true`, install retained 0.9.4.0 and begin Pass A from [Installed acceptance handoff](../docs/INSTALLED-ACCEPTANCE-HANDOFF.md). Do not use the owner profile.

After Pass A reaches installed 0.9.5.0, execute Pass B's real LM Studio operator task and Pass C's installed GUI/settings/recoverable-failure walkthrough, then finish uninstall/reinstall and data-preservation checks. Preserve every timestamped receipt and actual installed executable path.

## Open dependencies

- `B-R6-INSTALL-TRUST`: A fresh check found that Local Machine Trusted People lacks verified development publisher `0AC803FF3292A2C736B1CBB31AFF88418983B992`, no approved disposable test account is available, and the medium-integrity session cannot perform either administrator action. The current user has no Forge package registration and the all-users query is access denied. Registered install/Start/update/uninstall/reinstall and the installed operator passes cannot start.
- `B-R5-CENTRAL-SCHEMA-HISTORY`: authentic C008/C009 history remains unavailable. Preserve the owner schema-9 store. Internal Alpha readiness requires either authentic compatibility evidence or an explicit owner decision selecting a narrower supported deployment profile.

## Phase closeout obligations

R6-G2 and R6-G3 pass from real continuity and native visual/settings evidence. R6-G1 remains blocked and is not waived. Do not label the product INTERNAL ALPHA READY until Passes A–C and the existing-data decision are complete. Keep Git ref equality separate from the intentionally dirty unrelated `.forge-qwen` working tree.

<!-- alpha-phase-review:start -->
Phase review: R6 internal Alpha completion continuation — 2026-09-13. Implementation and verification status: [Product status](../docs/STATUS.md).
Delivery/merge status is recorded by the linked completion pull request.
<!-- alpha-phase-review:end -->
