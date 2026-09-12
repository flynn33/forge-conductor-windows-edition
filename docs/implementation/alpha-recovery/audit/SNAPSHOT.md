# Current snapshot and remaining-work assessment

**Observed September 12, 2026:** `flynn33/forge-conductor-windows-edition` main at `68c835729b94e574a0ecb9aa5a1bf1ebf3ff9f0b`, Git tree `bc0f281bb82b5acde57795e36371d98552191832`. PR #2, “Add Windows Alpha native desktop foundation”, was merged on September 12 at 14:15:52 UTC. Its previous main was `14660648599378c85c3cdade5bd44ffe1cded079`. The replacement therefore builds on that merged foundation, not the older package audit. [S01–S03, S23]

## Evidence limits
This assessment reviewed selected pinned GitHub files, current metadata, supplied Windows instructions and relevant official documentation. It did not access the owner's D:/A: drives, compile Windows code, run the GUI/model, test an installer or open the live database. A complete repository clone was not obtained; large/partial file reads are identified in [Sources](SOURCES.md). “Reported passed” below means a committed receipt/PR statement, not execution performed while assembling this package.

| Area | What the inspected snapshot supports | Remaining required work |
|---|---|---|
| Merged baseline | Native-desktop PR #2 is already in main | Reconcile local checkout and stale old-branch/uncommitted handoff claims; don't restart |
| C++/WinUI foundation | Real App project, build/staging helpers and native Manager connection/form exist | Keep the stack; finish application features, not another port |
| Native GUI | All required destination labels; real Provider/Rig panels; most other destinations generic | Real feature views, actions, charts, Settings and actual interactive proof |
| Telemetry | Manager composition uses an unavailable adapter; sample returns a capability error | Real native resource collectors, typed operational snapshots and meaningful visuals |
| Provider/continuity | Native non-streaming Responses bootstrap contract and reported fixture/automation wiring | Ordinary Manager-owned inference/tool loop, actual usage observation, productive successor |
| Local workflow | Project/MCP/tools/memory smoke reported passed | GUI integration and installed-path workflow, preserving another project/foreign MCP entries |
| Isolation | `--alpha-root` scoping and GUI/Manager attach/detach reported implemented | Reuse consistently; do not silently substitute it for normal startup |
| Installer | Existing signed engineering MSIX/ZIP mechanism and reported output | Real clean installed workflow, identity/version/data preservation, exact candidate provenance |
| Existing data | Records report schema 9 vs available C001–C007; source excerpt confirms C007 entry | Recover authentic compatibility contract or retain explicit open requirement; no destructive workaround |
| Documentation | CHANGELOG still “planned”; NEXT/STATUS refer to old uncommitted branch/auth/PIDs | One current tracked plan/ledger and all-doc sweep at every phase |

These findings cite S03–S22. The source's historical provider refusal, PID and elevation/authentication results are not fresh checks. Codex must refresh each when relevant, without repeatedly running a startup campaign.

## Specific anchors for implementation
`src/Hosts/App/MainWindow.xaml` and `.xaml.cpp` are the visible feature gap. `src/Hosts/Manager/ManagerCompositionRoot.cpp` is the composition starting point. `src/Composition/Windows/UnavailableTelemetryService.cpp` is the missing collector behavior. `include/ForgeConductor/Infrastructure/Windows/LMStudioResponsesTransport.h` explains the existing provider seam. `scripts/build.ps1`, `scripts/alpha/Build-App.ps1` and `scripts/package.ps1` are the existing build/distribution entry points.

The `.forge-alpha/NEXT.md` recorded next edit is a typed ordinary Manager run service feeding `ContinuityAutomation::observe`, then exposing state/action to Autonomy/Continuity. Treat that as a concrete source-directed next task, not an excuse for another plan-only session. The current source already includes `--alpha-root`; adding a new isolation subsystem would duplicate work. [S04, S13, S19]

## Scope corrections made by this replacement
Full functional operational telemetry in native visuals and accessible in-app settings are required now, not an undefined later “richer telemetry” enhancement. The eight R phases specify remaining work and a practical live acceptance pass. The new owner request adds a separate primary PR per phase, actual owner identity, synchronized source, and README/changelog/roadmap/all-document updates at every phase boundary. These are **new governing delivery instructions**, not claims that the old repository already enforced them. [U03–U04]

## GitHub administration boundaries
The inspected repository metadata identifies owner `flynn33`; merge metadata identifies Jim Daley with the owner's public GitHub noreply address. Local Codex authentication was not tested and must be verified before a push. No remote changes were made during this review. The connector could not read the milestone collection route used, so this package supplies stable desired milestone/issue definitions without fake remote IDs. A queried `.github/workflows` path returned404; this is not proof that required checks/review rules cannot exist. Discover actual repository conditions at adoption.

## Source-to-phase priority
R0 clears stale state without replaying completed foundation. R1 connects ordinary autonomous work. R2 implements real telemetry. R3/R4 finish the native operator workflows/settings. R5 completes package/data behavior. R6 proves the installed/live product. R7 delivers truthful documentation and exact tracked-source synchronization. Existing source and useful tests remain; historical broad release gates and unrelated platform plans do not become this assignment.
