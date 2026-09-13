# Native Windows GUI implementation map
Use the WinUI 3 C++/WinRT MSBuild app project under `src/Hosts/App`.
CMake remains the backend build. Prefer one NavigationView shell, reusable list/detail/status controls,
ordinary Windows typography/icons, and native settings dialogs. Do not spend Alpha effort reproducing Metal artwork.

Each view model receives the same typed manager client. Marshal UI updates through the UI dispatcher.
Async actions expose busy/cancel/result/error state and must not freeze the window during provider inference,
MCP deployment or process execution. A disconnected page offers reconnect, not fabricated empty success.

| Native surface | Essential Alpha actions/data | Reuse / reference |
|---|---|---|
| Rig | Overall manager/model/project/run status; start point for common work | Existing manager snapshot; Mac AppModel |
| LM Studio MCP | Preview/deploy/verify primary and fallback; show exact active executable/config | Existing deployment services, CLI serve |
| Agents | List packaged agents, inspect instructions, start appropriate managed work | Existing agent catalog/session services and Resources/Agents |
| Tools | Actual catalog, availability and useful descriptions; inspect a result | Existing McpToolCatalog/router/native handlers |
| Feed | Live recent activity and errors, selected project/run filters | Manager events, existing telemetry |
| Projects | Add/select/reopen/relink; stable ID and active folder | Existing project services; Mac ProjectsViewModel |
| Autonomy | Enter task, select project/model, start/pause/resume/stop; current next action | Manager-owned provider loop, not a UI task loop |
| Continuity | Context usage/threshold/source, handoffs, current chain; retry/resume/checkpoint | Existing coordinator plus new real-provider binding |
| Runtimes | Actual service/process state and start/stop/restart controls | Existing WindowsProcessSupervisor and manager |
| Provider | Save endpoint/model offline, refresh models, test real connection; token keep/replace/clear | Mac ProviderViewModel behavioral reference |
| Events & Evidence | Inspect result/log/artifact references and copy/open location | Existing evidence/event store |
| Diagnostics | Paths, versions, dependency availability, recent error and actionable checks | Existing WindowsRuntimeDiagnostics |
| Manager | Attach/start/stop/reconnect; persistence/startup preference | Existing manager lifecycle and IPC |
| Settings | Context capacity/reserve explanation, shell preference, logging, project/global reset | Native configuration/reset services |

R3 replaces required generic destinations with typed project, LM Studio, tool, agent/feed/runtime/diagnostic/Manager surfaces. R4 provides a dedicated Settings panel with effective readback, pending-edit revert, model discovery, explicit shell/startup/logging/session controls, paired context sliders/numeric inputs, and exact-scope maintenance confirmations. Standard WinUI controls carry keyboard focus and accessible names; a real scaled/high-contrast walkthrough remains required before acceptance.

Group related destinations using tabs if useful, but do not omit actions. All pages must use real data through R2–R4.
When an optional feature is unavailable, say which dependency is missing and give a real recovery action;
required Alpha features cannot be deferred by showing an unavailable label.

R1 adds a concrete Autonomy/Continuity control panel backed by typed Manager pipe commands. It accepts project,
client, authority generation, run, and task identity; displays run state, lifetime and retained token values,
pending calls, output, and errors; and exposes start, refresh, pause, resume, and stop. The GUI remains an attachable
client: closing it does not cancel or duplicate Manager-owned work.

R2 adds the live Rig presentation backed by `ManagerTelemetrySnapshot`: four operational health cards, direct
CPU/RAM/GPU/context values and gauges, bounded resource and activity-latency histories, accessible text equivalents,
and a recent activity timeline. Provider, Runtimes, Projects, Tools, Feed, Events, Diagnostics, Manager, and Settings
destinations project the same snapshot into readable detail text. Page selection persists for the Windows user,
refresh is coalesced at two seconds, chart points are recomputed after resize, and disconnected reads keep prior history explicitly stale.

## Project and reset behavior
A memory reset is not deletion of the source folder. Present the selected project and targeted stores before confirmation.
Support project memory only, continuity only, and combined project reset; provide separately confirmed all-project
memory/continuity/cache clearing. Keep actual source files, unrelated MCP entries, credentials and preferences unless
those are specifically selected. Stop/quiesce relevant writers, advance generation/fence bindings, and reopen repositories.
A stale run must not recreate cleared records after the UI reports success. Keep another project's data untouched.
Use existing reset interfaces where present; inspect their semantics before joining them into a Settings action.

## First-install workflow
Install -> Start app -> manager reachable -> select/register project -> provider settings -> test connection ->
LM Studio MCP deploy (for desktop integration) or managed task start -> actual tool result -> retained project state.
Make failures discoverable within the GUI; source checkout paths and a developer PowerShell are not an onboarding step.

## UI verification
Use a real Windows interactive session. Check navigation, keyboard focus, window resize and one useful action per page.
One focused native smoke is enough; do not add a large screenshot automation framework. Keep a short result record.

<!-- alpha-phase-review:start -->
Phase review: R7 — 2026-09-12. Implementation and verification status: [Product status](../STATUS.md).
Delivery/merge status is recorded by the linked phase pull request.
<!-- alpha-phase-review:end -->
