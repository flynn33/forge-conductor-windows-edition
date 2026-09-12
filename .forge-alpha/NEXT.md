# Windows Alpha execution cursor

**Package identity:** `windows-alpha-recovery-2026-09-12`
**Repository:** `D:\GitHub\Forge-Conductor-Windows-Edition`
**Current phase/slice:** R2.2
**Current branch:** `alpha/r2-native-telemetry`

## Verified state

- R1 implementation is verified at product commit `759a86329fa67eca34d14cfba1131584d9761103`; R1-G1 and R1-G2 passed focused native builds and controlled service/transport tests.
- The staged Debug GUI attached, detached, and reattached to the same Manager in `out/alpha-profiles/r1-gui-smoke`; the receipt is `out/alpha-evidence/r1/gui-manager-attach-detach.json`.
- R1-G3 remains blocked only for the native visual control walkthrough because the available computer-control surface exposed browser tabs and no native application controls.
- LM Studio was offline at `127.0.0.1:1234`; live provider acknowledgment and productive successor proof remains R6-G2.
- R0 PR #11 and R1 PR #12 merged; local `main` and `origin/main` were synchronized at `325d0470b8924e5fd6c202abec2b948b8312eb3a` before the R2 branch was created.
- R2.1 is verified at `53f5835`: 20 Windows CPU/RAM collector checks passed, including real-machine samples; the production Manager stayed alive with the native telemetry graph. DXGI publishes adapter/local-memory capability and explicitly leaves unavailable utilization unset.
- Preserve and exclude `.forge-qwen/state/**` changes and evidence. Never use `C:\Program Files\ForgeConductor` as source.

## Exact next actions

1. Implement R2.2 by exposing the existing `ITelemetryService` snapshot through the typed Manager protocol and native client.
2. Join the native CPU/RAM/process/GPU data with R1's authoritative run/context fields and existing operational events; do not recompute retained context in the GUI.
3. Build reusable native status, context, and bounded-history controls for R2.3 after the shared snapshot contract is covered.

## Open dependencies

- `B-R1-NATIVE-UI-CONTROL`: run the actual Autonomy/Continuity button walkthrough when a native Windows UI control surface is available.
- `B-R1-LMSTUDIO-OFFLINE`: start a tool-capable LM Studio server for R6 live continuity acceptance.
- `B-R5-CENTRAL-SCHEMA-HISTORY`: authentic C008/C009 history remains unavailable; continue using disposable `--alpha-root` profiles and never mutate the owner's newer live store.

R1 delivery is not merged merely because its implementation and focused checks pass. Follow the normal PR/merge workflow and refresh real branch/main state after an actual merge.

<!-- alpha-phase-review:start -->
Phase review: R1 — 2026-09-12. Implementation and verification status: [Product status](../docs/STATUS.md).
Delivery/merge status is recorded by the linked phase pull request.
<!-- alpha-phase-review:end -->
