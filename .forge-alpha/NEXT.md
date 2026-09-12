# Windows Alpha execution cursor

**Package identity:** `windows-alpha-recovery-2026-09-12`
**Execution revision:** `continuous-delivery-repair-2026-09-12`
**Repository host:** `D:\GitHub\Forge-Conductor-Windows-Edition`
**Current phase/slice:** R5.1 — preserve and diagnose existing data
**Current branch:** `alpha/r4-settings-reset-completion`

## Verified state

- R3 completion PR #15 merged at `1f6991d5fc60928c0a12ca348d08737fae703b1f`.
- R4 Settings PR #16 merged at `cb5a074285be0f4bd17b210c7fe0cc0505b7efab`; local main was fast-forwarded to that exact commit.
- R4 completion [PR #17](https://github.com/flynn33/forge-conductor-windows-edition/pull/17) carries typed scoped maintenance, persisted shell policy, accessibility refinements, focused tests and R4 documentation from reviewed functional head `91dd6bbfd6911773027188eb8b4f9357808ee30c`.
- Full x64 Debug products build. Manager protocol reports 646 assertions; dispatcher 11 groups; controller 7 groups; project-memory application/cache 4/7 groups; Windows memory/continuity repositories 17/6 groups.
- R3/R4 native walkthrough gates remain open under their original IDs because the available control surface cannot operate native windows.
- Preserve and exclude `.forge-qwen/state/**` changes and evidence. Never use `C:\Program Files\ForgeConductor` as source.

## Next exact action

Create `alpha/r5-installer-data` from the exact reviewed R4 completion head while PR #17 is pending. Inspect package identity, manifest, payload, signing path, install helper and current central-store compatibility. Build and verify the signed x64 Release MSIX and provenance against disposable profiles only; never open or mutate the owner's schema-9 store.

## Open dependencies

- `B-R1-NATIVE-UI-CONTROL`: run the consolidated R1–R4 native page/visual/keyboard/scaling/high-contrast walkthrough when native UI control is available.
- `B-R1-LMSTUDIO-OFFLINE`: start a tool-capable LM Studio server for R6 live continuity acceptance.
- `B-R5-CENTRAL-SCHEMA-HISTORY`: authentic C008/C009 history is unavailable; preserve the schema-9 owner store and keep newer-store rejection non-destructive.

## Phase closeout obligations

Finish R5 installer/data behavior, focused gates and the full active-document review. Push under verified `flynn33`, target main, and retain accurate dependency ordering. A build, package, PR, or phase boundary is a checkpoint; continue through R6–R7 afterward.

<!-- alpha-phase-review:start -->
Phase review: R4 — 2026-09-12. Implementation and verification status: [Product status](../docs/STATUS.md).
Delivery/merge status is recorded by the linked phase pull request.
<!-- alpha-phase-review:end -->
