# Windows Alpha execution cursor

**Package identity:** `windows-alpha-recovery-2026-09-12`
**Repository:** `D:\GitHub\Forge-Conductor-Windows-Edition`
**Current phase/slice:** R2.1
**Current branch:** `alpha/r1-managed-runs`

## Verified state

- R1 implementation is verified at product commit `759a86329fa67eca34d14cfba1131584d9761103`; R1-G1 and R1-G2 passed focused native builds and controlled service/transport tests.
- The staged Debug GUI attached, detached, and reattached to the same Manager in `out/alpha-profiles/r1-gui-smoke`; the receipt is `out/alpha-evidence/r1/gui-manager-attach-detach.json`.
- R1-G3 remains blocked only for the native visual control walkthrough because the available computer-control surface exposed browser tabs and no native application controls.
- LM Studio was offline at `127.0.0.1:1234`; live provider acknowledgment and productive successor proof remains R6-G2.
- R0 [PR #11](https://github.com/flynn33/forge-conductor-windows-edition/pull/11) is still awaiting normal merge.
- Preserve and exclude `.forge-qwen/state/**` changes and evidence. Never use `C:\Program Files\ForgeConductor` as source.

## Exact next actions

1. Continue on dependent branch `alpha/r2-native-telemetry` from the reviewed R1 head after reading the active R2 phase and telemetry/UI contract. R1 [PR #12](https://github.com/flynn33/forge-conductor-windows-edition/pull/12) targets `main` and issue #4 is synchronized.
2. Implement R2.1 by composing one shared typed telemetry snapshot from existing Manager/runtime, provider, context/continuity, project/run/tool, event, and store-health sources. Preserve authoritative R1 token values rather than recomputing them in the GUI.
3. Keep the open R1 native visual walkthrough explicit while progressing independent R2 work.

## Open dependencies

- `B-R1-NATIVE-UI-CONTROL`: run the actual Autonomy/Continuity button walkthrough when a native Windows UI control surface is available.
- `B-R1-LMSTUDIO-OFFLINE`: start a tool-capable LM Studio server for R6 live continuity acceptance.
- `B-R5-CENTRAL-SCHEMA-HISTORY`: authentic C008/C009 history remains unavailable; continue using disposable `--alpha-root` profiles and never mutate the owner's newer live store.

R1 delivery is not merged merely because its implementation and focused checks pass. Follow the normal PR/merge workflow and refresh real branch/main state after an actual merge.

<!-- alpha-phase-review:start -->
Phase review: R1 — 2026-09-12. Implementation and verification status: [Product status](../docs/STATUS.md).
Delivery/merge status is recorded by the linked phase pull request.
<!-- alpha-phase-review:end -->
