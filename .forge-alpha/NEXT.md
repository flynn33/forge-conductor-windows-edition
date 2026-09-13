# Windows Alpha execution cursor

**Package identity:** `windows-alpha-recovery-2026-09-12`
**Execution revision:** `continuous-delivery-correction-2026-09-12`
**Repository host:** `D:\GitHub\Forge-Conductor-Windows-Edition`
**Current phase/slice:** R6.4 — installed lifecycle and authentic schema compatibility
**Current branch:** `alpha/r6-installed-acceptance-closeout`, created from synchronized main `35f61de3c5a8159e20752a3843e5f26bfa5290c9`

## Verified state

- PR #22 merged at `35f61de3c5a8159e20752a3843e5f26bfa5290c9`. Draft PR #23 carries the narrow acceptance closeout and 0.9.4 candidate.
- The four redundant remote branches named in the owner directive were preserved at their exact expected heads in verified bundle `out/alpha-evidence/branch-retirement/redundant-alpha-branches-20260913.bundle`, SHA-256 `fc1085d1979739eb1cf313ddf5556d5f5f1e05143c2f7c521c37998895b76ea9`, then deleted with individual expected-head leases and verified absent. Their local branches remain.
- The old live run's terminal `context_budget_exceeded` text came from Forge's legacy desktop-chat invocation guard, not the model or Codex runtime. Manager-owned `managed-run-v1` calls now bypass that guard while retaining authorization, routing, and audit.
- The managed-run service regression covers 80 tool turns; a separate invocation-guard regression proves 12 identical Manager-owned calls proceed without legacy handoff or block.
- Full x64 Debug and Release products and the five affected focused suites passed. Existing G04 wrapper overflow remains failed and was not rerun.
- Signed candidate `ForgeConductor-0.9.4.0-x64.msix` has SHA-256 `937e503c3198d829907aff2a349067ad8f21c7cec54df071d668a531eb65c586`; ZIP SHA-256 is `2e0598163d582999f18d26f373cb49f23334b17425ac2277f1eee50912ed0403`. It is bound to commit `3fb70143ef2db9704c8a395b49a29857bd087976` and tree `fad39f10bdb2638a74a0b28deb0c0ecc1e88dede`.
- Exact 0.9.4 GUI/Manager run `34c078f8-6256-4b03-80e1-936e2e50f2f4` completed canonical rollover to successor `4ddd93de-6cc6-413f-b5fd-90da72e074d8`, performed both requested file effects, and returned terminal `DONE`. The per-project ledger records all eight transitions through `active_session_pointer_swapped`.
- The 0.9.3 `PACKAGE_OK` run is separate package-execution evidence. It is not used as the rollover run.
- LM Studio was restored to its initial state: model unloaded and server stopped.
- Preserve and exclude `.forge-qwen/state/**` changes and evidence. Never use `C:\Program Files\ForgeConductor` as source.

## Next exact action

After PR #23 merges, synchronize local `main`, preserve the exact merged continuation ref in a verified bundle, retire only its remote branch with an expected-head lease, and verify absence. The remaining product action is the authorized installed lifecycle in [Installed acceptance handoff](../docs/INSTALLED-ACCEPTANCE-HANDOFF.md), followed independently by authentic C008/C009 history recovery or continued non-destructive rejection.

## Open dependencies

- `B-R5-CENTRAL-SCHEMA-HISTORY`: authentic C008/C009 history is unavailable; preserve the schema-9 owner store and keep newer-store rejection non-destructive.
- `B-R6-INSTALL-TRUST`: an authorized administrator must import the public development certificate into Local Machine Trusted People and run the documented 0.9.3→0.9.4 install/update/uninstall/reinstall workflow in dedicated account `.\ForgeAlphaTest`. Do not retry elevation from this session or use the owner profile.

## Phase closeout obligations

R6-G2 and R6-G3 pass. R6-G1 remains blocked and is not waived. R7 remains merged. Keep implementation, individual checks, review readiness, merge, and acceptance distinct; report Git ref equality separately from the intentionally dirty unrelated `.forge-qwen` working tree.

<!-- alpha-phase-review:start -->
Phase review: R6 acceptance closeout — 2026-09-13. Implementation and verification status: [Product status](../docs/STATUS.md).
Delivery/merge status is recorded by the linked phase pull request.
<!-- alpha-phase-review:end -->
