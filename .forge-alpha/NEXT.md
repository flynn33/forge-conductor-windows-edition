# Windows Alpha execution cursor

**Package identity:** `windows-alpha-recovery-2026-09-12`
**Repository:** `D:\GitHub\Forge-Conductor-Windows-Edition`
**Current phase/slice:** R2.3
**Current branch:** `alpha/r2-native-telemetry`

## Verified state

- R1 implementation is verified at product commit `759a86329fa67eca34d14cfba1131584d9761103`; R1-G1 and R1-G2 passed focused native builds and controlled service/transport tests.
- The staged Debug GUI attached, detached, and reattached to the same Manager in `out/alpha-profiles/r1-gui-smoke`; the receipt is `out/alpha-evidence/r1/gui-manager-attach-detach.json`.
- R1-G3 remains blocked only for the native visual control walkthrough because the available computer-control surface exposed browser tabs and no native application controls.
- LM Studio was offline at `127.0.0.1:1234`; live provider acknowledgment and productive successor proof remains R6-G2.
- R0 PR #11 and R1 PR #12 merged; local `main` and `origin/main` were synchronized at `325d0470b8924e5fd6c202abec2b948b8312eb3a` before the R2 branch was created.
- R2.1 is verified at `53f5835`: 20 Windows CPU/RAM collector checks passed, including real-machine samples; the production Manager stayed alive with the native telemetry graph. DXGI publishes adapter/local-memory capability and explicitly leaves unavailable utilization unset.
- Draft R2 PR #13 targets `main` from `alpha/r2-native-telemetry`: https://github.com/flynn33/forge-conductor-windows-edition/pull/13. Issue #5 records R2.1 as verified while R2.2-R2.5 and all incomplete gates remain open.
- R2.2 is verified at `320e006`: the typed Manager protocol and native named-pipe client transport one timestamped operational snapshot, with native resources joined to Manager status/settings, the selected run's authoritative tokens and response ID, context headroom, continuity identity, runtime diagnostics, projects, tools, events and store-read health. The WinUI Rig refresh consumes this typed projection.
- Preserve and exclude `.forge-qwen/state/**` changes and evidence. Never use `C:\Program Files\ForgeConductor` as source.

## Exact next actions

1. Implement R2.3 reusable native CPU/RAM history charts, context capacity gauge, operational status cards and continuity/activity timeline from `ManagerTelemetrySnapshot`.
2. Preserve direct values, units and accessible text for unavailable, stale and error states; leave DXGI utilization absent when unsupported.
3. Continue directly into the R2.4 Rig dashboard and detail presentation after the reusable controls build and pass focused presentation checks.

## Open dependencies

- `B-R1-NATIVE-UI-CONTROL`: run the actual Autonomy/Continuity button walkthrough when a native Windows UI control surface is available.
- `B-R1-LMSTUDIO-OFFLINE`: start a tool-capable LM Studio server for R6 live continuity acceptance.
- `B-R5-CENTRAL-SCHEMA-HISTORY`: authentic C008/C009 history remains unavailable; continue using disposable `--alpha-root` profiles and never mutate the owner's newer live store.

R1 delivery is not merged merely because its implementation and focused checks pass. Follow the normal PR/merge workflow and refresh real branch/main state after an actual merge.

<!-- alpha-phase-review:start -->
Phase review: R1 — 2026-09-12. Implementation and verification status: [Product status](../docs/STATUS.md).
Delivery/merge status is recorded by the linked phase pull request.
<!-- alpha-phase-review:end -->
