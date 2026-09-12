# Windows Alpha execution cursor

**Package identity:** `windows-alpha-recovery-2026-09-12`
**Execution revision:** `continuous-delivery-correction-2026-09-12`
**Repository host:** `D:\GitHub\Forge-Conductor-Windows-Edition`
**Current phase/slice:** R6.1 — real installed workflow
**Current branch:** `alpha/r5-installer-data`

## Verified state

- R3 completion PR #15 merged at `1f6991d5fc60928c0a12ca348d08737fae703b1f`.
- R4 Settings PR #16 merged at `cb5a074285be0f4bd17b210c7fe0cc0505b7efab`; local main was fast-forwarded to that exact commit.
- R4 completion [PR #17](https://github.com/flynn33/forge-conductor-windows-edition/pull/17) merged at `b54525ac4aadb4847ad7685f4b9dd92c8546abc1`; local `main` and `origin/main` were synchronized to that exact ref before it was merged into the active R5 branch.
- R5 candidate source is committed at `3bcaeb3481022b38d6d6c9783510ace910957cf8` / tree `ab4587c78fe8f03328f0a2a68a983ed856634833`; draft [PR #18](https://github.com/flynn33/forge-conductor-windows-edition/pull/18) targets main.
- Full x64 Debug and Release products build. Focused MCP, persistence, infrastructure and PowerShell syntax checks pass. The signed 0.9.1.0 MSIX was unpacked and rehashed; its SHA-256 is `ddb3e8c6c43aedc21be0747f46431061f29c2ed3dfaad332e79f1026073f5087`. A real disposable schema-9 Manager start exited with the dedicated newer-store code 20 and left the database hash unchanged.
- The Release GUI used disposable profile `out/alpha-profiles/r6-native-ui`, started Manager PID 31172, visited all 14 required destinations, registered exact project `51e1f9a4-5943-46f1-9797-35a275768cc3`, closed and reattached to the same surviving Manager, and retained the selection. Rig showed changing CPU and 14.8% RAM against Windows 14.9%; GPU utilization gave an explicit unsupported reason. The test GUI and owned Manager were stopped afterward.
- Remote `alpha/r1-managed-runs` was verified at expected head `e304edeef72d9dc251a742cbefcd31693d6db423`, had no active PR, and had the same stable patch as main commit `b5ad0f4f280ac5e11b558c73ae60f8f90fd8b584`. Its exact ref is preserved in the verified ignored bundle under `out/alpha-evidence/branch-retirement/`, and only that remote branch was deleted with an expected-head lease.
- The native page/reattachment walkthrough now closes R1-G3, R2-G2, R3-G2 and R3-G3. Enlarged/high-contrast layout and complete keyboard operation remain open under R2-G3/R4-G3/R6-G3.
- Preserve and exclude `.forge-qwen/state/**` changes and evidence. Never use `C:\Program Files\ForgeConductor` as source.

## Next exact action

Unpack the exact signed MSIX into an ignored isolated directory and launch its GUI/Manager/CLI from those package bytes without repository build paths. Then recheck LM Studio at `127.0.0.1:1234`. Keep installed, live-provider, and scaled/high-contrast gates open wherever their prerequisites remain unavailable.

## Open dependencies

- `B-R1-LMSTUDIO-OFFLINE`: start a tool-capable LM Studio server for R6 live continuity acceptance.
- `B-R5-CENTRAL-SCHEMA-HISTORY`: authentic C008/C009 history is unavailable; preserve the schema-9 owner store and keep newer-store rejection non-destructive.
- `B-R6-INSTALL-TRUST`: `Add-AppxPackage` returned `0x800B0109`; approve the bundled public development certificate in Local Machine Trusted People through the normal elevated helper. No package is currently registered.
- `B-R6-NATIVE-SCALED-HIGH-CONTRAST`: native control and the page matrix now work, but enlarged/high-contrast layout and complete keyboard traversal remain unperformed.

## Phase closeout obligations

Push the R5 documentation/evidence closeout to draft PR #18, then continue R6 from its exact head while review is pending. Machine trust, provider availability, scaled/high-contrast inspection and authentic migration history retain their gate IDs; none is treated as passed. Keep implementation, checks, readiness, merge and acceptance distinct through R7.

<!-- alpha-phase-review:start -->
Phase review: R5 — 2026-09-12. Implementation and verification status: [Product status](../docs/STATUS.md).
Delivery/merge status is recorded by the linked phase pull request.
<!-- alpha-phase-review:end -->
