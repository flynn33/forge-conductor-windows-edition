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
- Empty/disconnected, compact viewport, scaling, keyboard, contrast, long-identifier and long-error checks remain to be recorded. Feature gaps, particularly tool typed controls, actual run history/continuity restoration, durable evidence, and diagnostic support export, are not waived by visual improvement.

## Build and package verification

- Full Release x64 Product All build and `ForgeConductor.App.TelemetryPresentationTests` passed for 1.1.5. The signed 1.1.5 package was valid, installed, and connected to live Manager telemetry; its visual audit is explicitly blocked above.
- The 1.1.6 Release x64 Product All build, focused native tests, signed package validation and installed production Manager inspection passed; design QA remained blocked.
- The 1.1.7 Release x64 Product All build and six focused test groups passed. Signed packaging and installed all-view visual QA are pending.
- Previous full CTest result was 150/151. `ForgeConductor.Manager.CompositionLifecycleTests` cannot run while the live installed Manager owns its hardcoded port 7788; displacement requires a bounded service pause and restoration.

## Final result

blocked
