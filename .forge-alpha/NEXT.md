# Windows Alpha execution cursor

**Package identity:** `windows-alpha-recovery-2026-09-12`
**Execution revision:** `continuous-delivery-correction-2026-09-12`
**Repository host:** `D:\GitHub\Forge-Conductor-Windows-Edition`
**Current phase/slice:** R6.4 — continuation delivery and remaining installed lifecycle
**Current branch:** `alpha/r6-live-continuity-fix`, created from synchronized main `a161443c974bce594ef7a655a88d02b20dec6040`

## Verified state

- PRs #19, #20, and #21 merged in dependency order. Local main, origin/main, and authenticated GitHub main synchronized at `a161443c974bce594ef7a655a88d02b20dec6040` before this continuation.
- The managed-run start transaction now creates an admissible open session and active binding before running. Continuity observes persisted tool effects, includes bounded completed-work summaries, and tells the successor to continue without repeating them.
- Native Manager and CLI session ledgers live under the memory root, with pre-SQLite migration from the legacy root location. LM Studio bootstrap uses its supported required-tool selection.
- Real LM Studio run `786d0672-6231-4202-a46c-401732ade283` activated fresh successor `7dd7aece-532a-459a-8489-ab9d62567466` after authoritative context usage crossed the configured threshold. Exact saved handoff retrieval, structured provider acknowledgment, predecessor fencing, and useful post-successor filesystem effects passed.
- A separate operation recovered after GUI detach and Manager restart. Exact unpacked 0.9.3 GUI/Manager/CLI bytes completed live run `b74477f9-25e8-4e6b-ba3c-d73daeb370f8` with `PACKAGE_OK`.
- Full x64 Debug and Release products built. The five affected focused suites passed. Signed candidate `ForgeConductor-0.9.3.0-x64.msix` SHA-256 is `9d9b899e7133cb9b46ac3f6221df5673e0bb7f55f1f92ef979d08ab77324607f`; ZIP SHA-256 is `dbef5788b6bd2bc91d03ee9228db02e7a9605bfdadbc66ed9e55e58329aa2721`.
- Remote `alpha/r1-managed-runs` remains retired and preserved in the verified ignored bundle under `out/alpha-evidence/branch-retirement/`.
- Preserve and exclude `.forge-qwen/state/**` changes and evidence. Never use `C:\Program Files\ForgeConductor` as source.

## Next exact action

Commit the reviewed R6 continuation records, push `alpha/r6-live-continuity-fix`, open the permitted continuation PR against main, follow normal review/merge rules, then fetch and fast-forward local main. Keep R6-G1 and schema-9 compatibility open.

## Open dependencies

- `B-R5-CENTRAL-SCHEMA-HISTORY`: authentic C008/C009 history is unavailable; preserve the schema-9 owner store and keep newer-store rejection non-destructive.
- `B-R6-INSTALL-TRUST`: Windows returned `0x800B0109` because the development publisher is absent from Local Machine Trusted People. Approve the bundled public certificate through the normal elevated helper, then run registered install/Start/update/uninstall acceptance.

## Phase closeout obligations

R6-G2 and R6-G3 pass. R6-G1 remains blocked and is not waived. R7 is merged and synchronized. Preserve implementation, checks, readiness, merge, and acceptance as distinct states; report ref equality separately from the intentionally dirty unrelated `.forge-qwen` working tree.

<!-- alpha-phase-review:start -->
Phase review: R6 continuation — 2026-09-12. Implementation and verification status: [Product status](../docs/STATUS.md).
Delivery/merge status is recorded by the linked phase pull request.
<!-- alpha-phase-review:end -->
