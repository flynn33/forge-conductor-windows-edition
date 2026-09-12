# Product status

Updated September 12, 2026 for plan `windows-alpha-recovery-2026-09-12`. R1 implementation is in [PR #12](https://github.com/flynn33/forge-conductor-windows-edition/pull/12), based on the still-open R0 delivery branch. Implementation, pull-request delivery, merge, and final acceptance remain separate in [the execution ledger](../.forge-alpha/status.json).

| Area | Current source/evidence | Remaining requirement |
|---|---|---|
| Native backend | CLI, Manager and SessionHost build in x64 Debug/Release; prior focused Domain, Manager, MCP, continuity, session and infrastructure checks passed | Re-run only checks affected by each R slice; complete real installed acceptance in R6 |
| Native GUI | C++20/WinUI 3 host builds and attaches to the Manager; Autonomy/Continuity expose project/run/task inputs, live run/context state, and start/refresh/pause/resume/stop commands | Complete telemetry, operating workflows and accessible Settings in R2–R4; run the native visual control walkthrough when a native UI control surface is available |
| Managed provider path | The Manager owns durable ordinary runs, `/v1/responses` turns, authorized native tool routing, retained-context observations, canonical handoff activation, successor response identity, and safe pause/resume/cancel | Prove live provider-originated handoff acknowledgment and useful successor work in R6 |
| Context policy | Count/time rollover fields and behavior are removed; prior focused regression kept 500 ordinary observations on one chain | Prove actual context-triggered rollover with a live tool-capable model in R6 |
| Projects, MCP and native tools | Prior disposable service workflow covered two-project isolation, MCP deployment, file/search/Git/shell/memory effects and foreign-entry preservation | Expose and verify the real workflows through native pages in R3 and from the installed candidate in R6 |
| Installer | Signed x64 engineering MSIX/ZIP creation succeeded with public certificate material and native products | Finish payload/provenance/onboarding/data behavior in R5, then install/update/uninstall and Start-launch acceptance in R6 |
| Data compatibility | Source currently contains central migrations C001–C007; the prior isolated evidence reported the owner's live central store at schema 9 and preserved it | Obtain authentic C008/C009 history or keep the newer store rejected without mutation; use `--alpha-root` for disposable work |

Current external observation: TCP connection to LM Studio at `127.0.0.1:1234` was refused on September 12, 2026, so no live model or live rollover result is claimed. A fresh isolated Debug profile started the real Manager and GUI; closing and reopening the GUI left the same Manager owner running. Native UI automation was unavailable in this session, so the visual button walkthrough remains open.

The signed engineering distribution at `out/dist/engineering-0.9.0.0-20260912-140140/` and its recorded MSIX SHA-256 `8909b08760f5a04d4dfaee087da622060754cc19c253cda3c393ebcb4e19cad2` are historical local build evidence. Installation previously failed because the development publisher was not trusted and the session could not add machine trust. R5/R6 must revalidate the actual candidate and permissions; R0 does not relabel that attempt as current acceptance.

GitHub authentication is currently verified as owner `flynn33` with push access. R0–R7 milestones 1–8 and phase issues [#3](https://github.com/flynn33/forge-conductor-windows-edition/issues/3) through [#10](https://github.com/flynn33/forge-conductor-windows-edition/issues/10) exist. [ROADMAP.md](../ROADMAP.md) records their mapping.

<!-- alpha-phase-review:start -->
Phase review: R1 — 2026-09-12. Implementation and verification status: [Product status](STATUS.md).
Delivery/merge status is recorded by the linked phase pull request.
<!-- alpha-phase-review:end -->
