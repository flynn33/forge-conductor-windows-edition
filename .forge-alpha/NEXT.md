# Windows Alpha execution cursor

**Package identity:** `windows-alpha-recovery-2026-09-12`
**Execution revision:** `continuous-delivery-repair-2026-09-12`
**Repository host:** `D:\GitHub\Forge-Conductor-Windows-Edition`
**Current phase/slice:** R4.1 — persistent accessible Settings
**Current branch:** `alpha/r3-native-workflows`

## Verified state

- PR #13 merged at `bdd6e05fd1843a353116628b1ef33c2e6a948fc5`; local `main`, `origin/main`, and a fresh GitHub ref observation were synchronized before the R3 branch was created.
- R2-G2/G3 remain blocked under their original IDs for native rendered-value, keyboard, scaling, and high-contrast inspection. The merge did not close those acceptance checks.
- Draft [PR #14](https://github.com/flynn33/forge-conductor-windows-edition/pull/14) is open from commit da199521176a338774a5e12bcd58fddddb9cbdce; R3.1 is implemented in the current branch: typed Manager requests expose project listing/registration and persistent memory search/read/write/status; the native Projects page shows exact identity, authorized folders, persistence health and records, persists selection, and binds that exact ID into Autonomy.
- Focused evidence: x64 Debug Manager and App builds passed; Manager protocol passed 625 assertions; dispatcher passed 10 groups including rejection of a cross-project memory response; the real native P2 workflow smoke passed on a fresh isolated profile with two distinct projects, no memory leakage and memory persistence after process restart.
- R3.2 source is implemented: the native LM Studio MCP page performs Manager-owned inspect/repair/activate operations with foreign-entry preservation, while the Tools workbench displays the exact catalog and persisted shell preference and invokes tools under the selected project authority. Manager and WinUI builds pass; the consolidated native walkthrough remains open under R3-G2.
- R3.3-R3.5 source is complete: Agents, Feed, Runtimes, Diagnostics, and Manager use typed operational Manager data; agent/session IDs, audit outcomes/errors/durations, close/prune actions, runtime counts, and doctor results are visible. Required R3 destinations no longer route through GenericPanel.
- The corrected continuous-delivery rule is incorporated into root execution/Git guidance and the handoff/closeout templates as part of this R3 change.
- Preserve and exclude `.forge-qwen/state/**` changes and evidence. Never use `C:\Program Files\ForgeConductor` as source.

## Next exact action

Extend the typed Manager surface and native `LM Studio MCP` / `Tools` pages for R3.2. Reuse the existing deployment service, tool catalog, workspace authority and MCP registration logic to expose installed-registration, connected-client and managed-continuity states; add primary/fallback deploy, inspect and repair actions while preserving foreign entries and the explicit shell preference.

If the R3 PR is pending when a later phase becomes actionable, continue independent work from refreshed main or dependent work from the exact reviewed R3 head on its separate phase branch. Keep the successor PR targeting main and do not merge it ahead of R3.

## Open dependencies

- `B-R1-NATIVE-UI-CONTROL`: the current control surface exposes browser tabs only. Run the consolidated R1/R2/R3 native visual, keyboard, scaling and high-contrast walkthrough when an interactive native control surface becomes available; continue independent source work now.
- `B-R1-LMSTUDIO-OFFLINE`: start a tool-capable LM Studio server for R6 live continuity acceptance. Offline provider state does not block R3.2 registration/tool implementation.
- `B-R5-CENTRAL-SCHEMA-HISTORY`: authentic C008/C009 history remains unavailable. Continue using disposable `--alpha-root` profiles and never mutate the owner's live schema-9 store.

## Phase closeout obligations

Complete R3.2–R3.5, the focused R3 gates, every required native destination, and the full active-document review. Push under verified `flynn33`, keep one R3 PR targeting main, and record its real URL and reviewed head. A build, ready PR, saved handoff, or phase boundary is a checkpoint; execute the next feasible R4–R7 action afterward.

<!-- alpha-phase-review:start -->
Phase review: R2 — 2026-09-12. Implementation and verification status: [Product status](../docs/STATUS.md).
Delivery/merge status is recorded by the linked phase pull request.
<!-- alpha-phase-review:end -->
