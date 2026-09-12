# macOS-to-Windows parity matrix
Source audit, September 10, 2026. `Present` means source observed, not a fresh runtime pass.
The Mac reference itself is not fully live-qualified. See the [pinned source index](reference/SOURCES.md).
This matrix is the Alpha contract plus an explicit later-parity backlog, not a claim of full parity today.

| Capability | Mac evidence | Windows evidence | Scope | Required action | Phase |
|---|---|---|---|---|---|
| Native desktop GUI | Mac app/operator-console implementations present | No WinUI host target found | Required Alpha | Build all required native pages and actions | P1/P4 |
| Installation | Mac app/bundling workflow documented; not Windows evidence | MSIX template plus throwing package script | Required Alpha | Signed installable native MSIX distribution | P1/P5 |
| Per-user manager/lifecycle | Manager-owned runtime with GUI clients documented | Manager/CLI composition and native lifecycle targets present | Required Alpha | Attach/close/reopen/stop without duplicate owner | P1/P3 |
| Project registration and identity | Native register/select/reset/relink actions inspected | Project services present; GUI actions not established | Required Alpha | Wire stable identity, selection, relink and scoped reset | P2/P4 |
| LM Studio MCP deployment | Primary/fallback deploy/verification workflow documented | Native CLI/MCP exists; installed GUI deploy not established | Required Alpha | Deploy actual installed server; preserve foreign entries | P2/P5 |
| File read/write/search/glob | Native tool families documented | Windows native handlers wired into MCP composition | Required Alpha | Exercise handlers from deployed configuration | P2 |
| Git operations | Mac tool family documented | WindowsGitService present | Required Alpha | Find real Git, expose action failures; verify disposable repo | P2 |
| Shell execution | Enabled by default; explicit opt-out preserved | Shell service present, configuration defaults off | Required Alpha | Enable clean profile, retain explicit opt-out, PowerShell contract | P2 |
| Project memory / legacy memory | Project-scoped plus compatibility tools documented | Services and repository cache present | Required Alpha | Verify persistence and project separation | P2/P4 |
| Agents / tools / feed | Native surfaces and bundled agents present | Resources and core services present, no required GUI | Required Alpha | Expose actual catalog/session/activity data | P4 |
| Provider settings / discovery | Native offline-save/model-refresh/probe view model inspected | No equivalent native UI established | Required Alpha | Persist endpoint/model/auth preference and test actual provider | P3/P4 |
| Managed real inference | Mac real Responses implementation inspected | Production local logical binding; custom alternative HTTP route | Required Alpha | Native Responses transport + manager-owned tool loop | P3 |
| Context-triggered handoff | Mac adapter/handoff implementation; live qualification open | Context code also rolls over independently on count/time | Required Alpha | Context-only trigger, genuine successor, useful resumed work | P3 |
| CLU continuity behavior | README says correction and separate live qualification in progress | No live CLU/provider qualification established by audit | Required Alpha semantics | Implement real managed continuity; reuse native entry if present; no duplicate controller | P3 |
| Native completion/governance policy | Mac documents installed native policy; CLU work remains open | Forsetti module and old gates exist; runtime parity not fully traced | Retain existing / simplify Alpha | Keep native hooks; align completion with actual Alpha/task result, not old gate program | P0/P3 |
| Context settings | Mac context/continuity model and UI documented | Core ContextBudget models present; native settings UI missing | Required Alpha | Effective capacity control and honest usage/reserve display | P3/P4 |
| Count-based run quotas | Older policies are not target behavior | Progress/time rollover explicitly present; other quotas need semantic sweep | Remove now | Delete active quota configuration/enforcement and old emitted guidance | P0/P3 |
| Settings: memory and continuity flush | Project reset UI inspected; broader feature coverage not all executed | Reset interfaces exist; full UI transaction unqualified | Required Alpha | Scoped and explicit all-stores clear with writer quiescence | P4 |
| Runtimes / diagnostics / evidence | Mac operator-console files and diagnostics documented | Native process/telemetry/diagnostic services present | Required Alpha subset | Useful native status/actions/log references | P4 |
| Optional dashboard and rich GPU views | Mac telemetry/dashboard and Metal assets present | Dashboard/native telemetry code present; full parity unverified | After Alpha expansion | Keep existing basics, defer high-fidelity optional visuals | B1 |
| Full per-tool schema/semantic inventory | Historical Windows baseline includes Mac tool inventory | Not every current tool handler independently audited | Required primary tools; remaining B1 | Reconcile existing baseline with actual tools/list; report per-tool gaps explicitly | P2/B1 |
| Advanced connectors / portable import | Coverage varies; not individually executed here | Not individually verified | After Alpha expansion | Record real gaps; preserve already working optional features | B1 |
| Mac-only filesystem hardening | Mac production move/recursive protected delete are limited | Not a Windows Alpha qualification goal | Deferred / platform-specific | Do not port privileged Mac security architecture to finish Alpha | B2 |
| ARM64 / public distribution / exhaustive testing | Mac-specific release qualifications remain separate | ARM64 presets exist; runtime unverified | Deferred | x64 first; no broad release-gate dependency | B2 |

## Complete tool inventory without another archaeological project
Reuse `.forge-codex/state/baseline/mcp-tool-baseline.json` and `p02-mcp-semantic-inventory.json`
as historical starting points, not current truth. Compare with the current production `tools/list` result
and the pinned Mac catalog. Record for each actual tool: name, schema compatibility, native handler,
primary/fallback exposure, project scope, and a real invocation result or explicit unverified status.
Do not infer success from a catalog name or a source-file count.
A small generated CSV/JSON inventory is sufficient. Fix the primary workflow tools now, retain working optional tools,
and assign advanced non-Alpha gaps to B1. Do not write a new exhaustive validator for every tool.

The removal of count/time-triggered run controls is intentional owner-directed divergence, not a parity regression.
CLU is Command Logic Unit. Match genuine manager-owned continuity semantics; do not introduce another independent
controller or promise unowned desktop-tab automation. Preserve an existing compiled native deployment entry point
where required by the actual contract, but do not block Alpha on a separate governance-engine redesign.
