# Direction A design QA — in progress

## Visual truth

- Selected direction: Direction A — Obsidian Command Center.
- Reference: `C:\Users\james\.codex\generated_images\01a09f83-2027-77e3-a87d-18dd99de82ed\exec-2dd3f5cc-e935-4dd9-904e-2815b3ba4d3a.png`
- The 1.1.3 captures below are historical, not proof of the current implementation. The prior parity declaration was rejected by the user and is withdrawn.
- The 1.1.4 development build introduces page-specific native layouts, but is not an installed all-view proof set.
- Final comparison: `.superdesign/qa/direction-a-comparison-final.png`.
- All-view contact sheet: `.superdesign/qa/all-views-contact-sheet.png`.
- Viewport: 1680 × 945 logical pixels at 150% Windows display scaling; native proof captures are 2520 × 1417 physical pixels.
- Capture method: each installed-app view was captured with `PrintWindow`, preserving the complete window without a taskbar overlay.
- Runtime state: Production profile with live, authenticated Manager telemetry and real local provider data.

## All-view proof set

The previous 1.1.3 installed package was captured in every navigation view; these captures must not be presented as 1.1.4 proofs:

1. `.superdesign/qa/views/01-rig.png`
2. `.superdesign/qa/views/02-lm-studio-mcp.png`
3. `.superdesign/qa/views/03-agents.png`
4. `.superdesign/qa/views/04-tools.png`
5. `.superdesign/qa/views/05-feed.png`
6. `.superdesign/qa/views/06-projects.png`
7. `.superdesign/qa/views/07-autonomy.png`
8. `.superdesign/qa/views/08-continuity.png`
9. `.superdesign/qa/views/09-runtimes.png`
10. `.superdesign/qa/views/10-provider.png`
11. `.superdesign/qa/views/11-events-evidence.png`
12. `.superdesign/qa/views/12-diagnostics.png`
13. `.superdesign/qa/views/13-manager.png`
14. `.superdesign/qa/views/14-settings.png`

## Comparison findings

The packet requires rendered comparison to the pinned Mac reference views R30–R43. Rendered captures for all 14 reference views are not present in the packet or workspace; the selected Direction A image depicts Rig only. Source-only inspection cannot establish visual parity.

The 1.1.4 development Rig, Agents, Tools and LM Studio MCP were inspected via native screenshots on 2026-09-15. The unpackaged build cannot authenticate to the installed Manager, so disconnected and empty states do not prove production-data parity. A wide-window defect left secondary pages in a narrow left column; shared content stretch sizing was corrected and rechecked. A compact-width check exposed clipping with an always-open navigation pane; adaptive pane behavior was enabled and the Tools view rechecked at 1138 × 912 without horizontal clipping. Purposeful inventory/catalog empty states and aligned MCP role cards were also rechecked. Rig's broad panel hierarchy follows the selected direction, but its disconnected hero, empty telemetry, and differing action/event detail are materially unlike the active-state mock. Agents' live specialist-card content and the pinned Mac rendered comparison remain unverified.

R30–R43 remain incomplete. Capture each current installed-package view at a controlled viewport and compare it to its rendered reference before declaring a pass.

## Build, package, and interaction verification

- Full Release x64 Product All build passed after the visual refinements and Feed timestamp projection. Four focused Manager protocol/dispatcher and app presentation/scheduler tests passed after that build.
- A fresh development-signed 1.1.4 MSIX was produced from commit `5f380b9001489488f7d3ac2ed8f0a860974b9108` at `out/dist/release-1.1.4.0-20260915-103446/ForgeConductor-1.1.4.0-x64.msix`; SHA-256 `830dc00c5a4d2d34358a85f0c359f5d30c72be924020c2752a79da078195500d`, signature Valid.
- Installation again failed with `0x80073D02`: AppX deployment identified the 1.1.3 package app as still running. `tasklist` confirmed installed Manager PID 44464 remains alive. Its own Stop service command previously left the process alive, and the service was restored. No forced termination was performed.
- Full Release x64 CTest: 150/151 passed. `ForgeConductor.Manager.CompositionLifecycleTests` refused to run while the installed Manager owned its hardcoded port 7788; the service was not displaced for a test.
- The current signed MSIX contains the width, empty-state, role-card, adaptive-navigation, and Feed changes, but 1.1.4 is not installed.
- Historical 1.1.3 focused verification passed 7/7:
  - `ForgeConductor.Telemetry.WindowsTests`
  - `ForgeConductor.App.TelemetryPresentationTests`
  - `ForgeConductor.App.ActionSchedulerTests`
  - `ForgeConductor.Mcp.ProtocolServerTests`
  - `ForgeConductor.Mcp.ServeProcessSnapshotTests`
  - `ForgeConductor.Infrastructure.UnitTests`
  - `ForgeConductor.SessionHost.PluginSmokeTests`

## Final result

blocked
