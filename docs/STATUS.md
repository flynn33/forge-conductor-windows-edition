# Product status

Updated September 12, 2026 for plan `windows-alpha-recovery-2026-09-12`. R0–R3 are merged. R4 Settings merged in PR #16 at `cb5a074285be0f4bd17b210c7fe0cc0505b7efab`; the scoped-maintenance completion is on its successor branch. Implementation, pull-request delivery, merge, gate blockers, and final acceptance remain separate in [the execution ledger](../.forge-alpha/status.json).

| Area | Current source/evidence | Remaining requirement |
|---|---|---|
| Native backend | CLI, Manager and SessionHost build in x64 Debug/Release; prior focused Domain, Manager, MCP, continuity, session and infrastructure checks passed | Re-run only checks affected by each R slice; complete real installed acceptance in R6 |
| Native GUI | C++20/WinUI 3 host builds and attaches to the Manager; required destinations now use native project, LM Studio, tool, operational, run, telemetry, provider, Settings, and maintenance surfaces with persisted page/project selection | Run the blocked native page, visual, keyboard, scaling, and high-contrast walkthroughs when a native UI control surface is available |
| Native telemetry | The Manager owns real Windows CPU/RAM/process collection and DXGI adapter/memory capability reporting; one typed pipe snapshot includes runtime, provider, authoritative context/continuity, projects, tools, events, and store health | GPU utilization remains unavailable on unsupported adapters by design; compare rendered visuals and accessibility with a native interactive surface |
| Managed provider path | The Manager owns durable ordinary runs, `/v1/responses` turns, authorized native tool routing, retained-context observations, canonical handoff activation, successor response identity, and safe pause/resume/cancel | Prove live provider-originated handoff acknowledgment and useful successor work in R6 |
| Context policy | Count/time rollover fields and behavior are removed; prior focused regression kept 500 ordinary observations on one chain | Prove actual context-triggered rollover with a live tool-capable model in R6 |
| Projects, MCP and native tools | Projects registers authorized folders, persists exact selection, reads/writes memory, and binds the ID to runs/tools. LM Studio registration, the 48-tool catalog/invocation, agent/session controls, feed, runtimes, diagnostics, and Manager status use typed Manager operations. | Verify all pages and one native UI-driven workflow through the installed candidate in R6 |
| Settings and maintenance | Settings persists and reads back dashboard, Manager, provider, shell, logging, session, and context values. Exact-scope Manager maintenance coordinates transactional memory/continuity reset and closes old repository generations. Focused protocol/controller/dispatcher and Windows repository checks pass. | Complete the native keyboard/scaling/high-contrast walkthrough; exercise the full confirmed reset workflow on an installed disposable profile in R6 |
| Installer | Signed x64 engineering MSIX/ZIP creation succeeded with public certificate material and native products | Finish payload/provenance/onboarding/data behavior in R5, then install/update/uninstall and Start-launch acceptance in R6 |
| Data compatibility | Source currently contains central migrations C001–C007; the prior isolated evidence reported the owner's live central store at schema 9 and preserved it | Obtain authentic C008/C009 history or keep the newer store rejected without mutation; use `--alpha-root` for disposable work |

Current external observation: TCP connection to LM Studio at `127.0.0.1:1234` was refused on September 12, 2026, so no live model or live rollover result is claimed. A fresh isolated R2 Debug profile kept the real Manager and GUI alive across three telemetry intervals; the Manager survived GUI close while its observed working set and Windows RAM samples changed. A disconnected GUI also survived repeated failed refreshes. Native UI automation was unavailable in this session, so chart pixels, keyboard navigation, enlarged layout, and high contrast remain open.

The signed engineering distribution at `out/dist/engineering-0.9.0.0-20260912-140140/` and its recorded MSIX SHA-256 `8909b08760f5a04d4dfaee087da622060754cc19c253cda3c393ebcb4e19cad2` are historical local build evidence. Installation previously failed because the development publisher was not trusted and the session could not add machine trust. R5/R6 must revalidate the actual candidate and permissions; R0 does not relabel that attempt as current acceptance.

GitHub authentication is currently verified as owner `flynn33` with push access. R0–R7 milestones 1–8 and phase issues [#3](https://github.com/flynn33/forge-conductor-windows-edition/issues/3) through [#10](https://github.com/flynn33/forge-conductor-windows-edition/issues/10) exist. [ROADMAP.md](../ROADMAP.md) records their mapping.

<!-- alpha-phase-review:start -->
Phase review: R4 — 2026-09-12. Implementation and verification status: [Product status](STATUS.md).
Delivery/merge status is recorded by the linked phase pull request.
<!-- alpha-phase-review:end -->
