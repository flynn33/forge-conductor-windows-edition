# Direction A design QA — in progress

## Visual standard and provenance

- User-selected standard: Direction A, Obsidian Command Center. Original Rig concept: `C:\Users\james\.codex\generated_images\01a09f83-2027-77e3-a87d-18dd99de82ed\exec-2dd3f5cc-e935-4dd9-904e-2815b3ba4d3a.png`.
- On 2026-09-15, the owner explicitly approved newly composed Windows views in that same design language instead of rendered Mac references. This is the alternate visual standard for R30–R43, not a claim of pixel parity with the unavailable Mac captures.
- The 1.1.3 screenshots in `.superdesign/qa/views/` and its contact sheet are historical. The owner rejected their parity claim; they must not be used as current proofs.
- Evidence must come from an installed, release-signed Windows package with authenticated production Manager data. Unpackaged development screenshots are useful for iteration but are not final proof.

## Current visual findings

- Installed 1.1.4 was launched on this host after the owner removed 1.1.3. Its Rig, Agents, Tools, Feed and LM Studio MCP were inspected at a 1666 × 659 physical-pixel window with live Manager telemetry.
- Rig had the intended broad grid but fell short of the selected concept in hero identity, thin meters, noisy unscaled chart, plain event dumps and lifecycle-centric command-center actions. Agents and Feed were still dense native data panes rather than polished operational compositions. Tools was functional but its invocation path remained an advanced JSON form.
- Signed 1.1.5 was installed and authenticated to the production Manager. At 1666 × 827, Rig aligned its broad grid and meter treatment more closely with Direction A, but hierarchy, type size, history labeling and recent-event treatment still missed the selected concept. The fourteen destinations were inspected live; Agents, Feed, Projects, Provider, Autonomy/Continuity, Runtimes, Events & Evidence, Diagnostics and Manager remained too thin or generic for a finish claim.
- Signed 1.1.6 was installed and connected to the production Manager. The live sweep showed genuine improvement in Rig, Projects, Provider, Autonomy/Continuity, Feed and Agents, but Tools still required JSON, Manager/Runtimes had an oversized empty inventory, some record counters misleadingly said LIVE, and Provider exposed raw telemetry. The 1.1.7 source pass addresses these specific misses; it is not installed visual proof until packaged and inspected.
- Signed 1.1.7 installed live. Guided tool arguments successfully invoked a read-only native catalog action through the production Manager. Its latest result still exposed raw JSON, and Settings remained a long, flat form.
- Signed 1.1.8 installed live and authenticated to the production Manager. Its guided `agent_list` result displayed structured records and exact Manager-measured duration. Settings improved but its expanded form occupied only half the available width; every cold launch still opened an excessively wide, short window. The 1.1.9 source pass addresses those concrete issues. Unpackaged inspection at a 1108 × 607 layout-pixel viewport confirmed a usable compact navigation rail and two-row Rig metrics, but is not installed proof.
- Signed 1.1.10 was installed and inspected live at 1668 × 906 Windows layout pixels on this host's 150% UI scaling. The installed release opened with the full navigation pane, authenticated to the production Manager and populated all fourteen destinations. Rig now has the selected broad instrument-board structure, but its dense CPU-core/GPU-engine detail became a long plain dump when scrolled, and configured-but-unverified Provider and no-run Continuity states used false healthy colors. Agents and Diagnostics left their detail panes sparse; Runtimes had a fixed-height nearly empty inventory; navigation retained the previous page's scroll offset. The 1.1.11 source changes target these observed defects. Events & Evidence still mirrors Feed audit activity without a durable verification workflow, Autonomy lacks project-scoped run history, and Diagnostics lacks a support export.
- Empty/disconnected, compact viewport, scaling, keyboard, contrast, long-identifier and long-error checks remain to be recorded. Feature gaps, particularly actual run history/continuity restoration, durable evidence, and diagnostic support export, are not waived by visual improvement.

## Build and package verification

- Full Release x64 Product All build and `ForgeConductor.App.TelemetryPresentationTests` passed for 1.1.5. The signed 1.1.5 package was valid, installed, and connected to live Manager telemetry; its visual audit is explicitly blocked above.
- The 1.1.6 Release x64 Product All build, focused native tests, signed package validation and installed production Manager inspection passed; design QA remained blocked.
- The 1.1.7 Release x64 Product All build, six focused test groups, signed package validation and installed production Manager/tool inspection passed; design QA remained blocked.
- The 1.1.8 Release x64 Product All build and eight focused Manager/MCP/host/infrastructure tests passed. Its MSIX signature was valid and the production install authenticated to the live Manager; all-view visual and feature QA remained blocked.
- The 1.1.9 adaptive layout compiled and passed eight focused tests. Its signed 1.1.9 MSIX validated and installed. The cold launch connected to the production Manager and loaded Runtimes automatically, but viewport sizing used physical pixels as though they were Windows layout units, rendering only 1108 × 607 layout pixels at this host's DPI. Its all-view acceptance remains blocked.
- The 1.1.10 candidate compiled. Unpackaged inspection confirmed a 1668 × 906 layout-pixel viewport and stable full navigation; narrowing it to 1391 pixels gave a correctly anchored expandable compact rail. Signed 1.1.10 validated and installed; the production cold launch was 1668 × 906 layout pixels and connected to the live Manager. Its release source was commit `c7037f5`; package SHA-256 was `eabf5c7d683a00bbdd3c382a9f5c01217746a42cf29c76e5c82574f12fbd58c4`.
- The 1.1.11 source pass compiled the native GUI and Manager dispatcher; eight focused Manager/MCP/host/infrastructure checks passed. Product All rebuilt successfully before the final project-selection race guard; its App target passed again after that guard. Exact-commit Product All staging, signed package verification, installed 1.1.11 visual sweep and scaled/accessibility checks remain pending.
- Previous full CTest result was 150/151. `ForgeConductor.Manager.CompositionLifecycleTests` cannot run while the live installed Manager owns its hardcoded port 7788; displacement requires a bounded service pause and restoration.

## Final result

blocked
