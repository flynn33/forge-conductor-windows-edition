# Full operational telemetry in native visuals

## Completion target, not optional polish
The audited application has a real Provider panel and Manager controls, but other destinations map to generic panels. The production composition references `UnavailableTelemetryService`; its implementation returns an unavailable error for samples. Fix the missing native collection and data path as well as rendering. Merely changing the unavailable text or drawing sample graphs does not satisfy the requirement. [S08–S12]

Use existing telemetry/domain/event contracts and the typed Manager boundary. Evolve the narrow interfaces needed to supply native view models; do not create a second telemetry service stack for the GUI or redesign the optional browser dashboard. Use native Microsoft XAML controls/shapes for these modest histories; no browser, JavaScript chart dependency or new cross-platform rendering toolkit. A native Direct2D/Composition path is appropriate only when already present or demonstrably needed.

## Metric and visual contract

| Area and operator question | Actual measurements/state | Native presentation |
|---|---|---|
| Host: is the machine under pressure? | System CPU utilization, used/available physical RAM, sampling freshness. | CPU and RAM time-series with current value, units and source/scope labels. |
| Forge: which process/job is busy? | Manager and known GUI/CLI/session-host process presence, available CPU and memory; active jobs and exit/failure state. | Process rows with scoped numbers/sparklines and a selected job detail. |
| GPU: is supported graphics capacity busy? | Native-detected adapter identity; utilization and memory only where supported measurement exists. Distinguish process/accounting budgets from device totals. | Adapter selector, utilization trend and memory bar; explicit unsupported/reason state when necessary. |
| Runtime/Manager: is control connected and current? | Reachability, heartbeat/sample age, owned profile, version/PID, service/job state and last error. | Status strip and actionable health panel; stale/disconnected is not green/zero. |
| Provider: is actual inference progressing? | Endpoint/model identity, request/inference state, response duration, actual input/output usage; throughput only with its measurement basis. | Current request/status plus latency trend and token summary. |
| Context: how near is a handoff? | Effective capacity, retained usage or explicit estimate, output/handoff/safety reserves, threshold, remaining headroom and confidence/source. | Labeled stacked capacity/headroom bar or gauge, exact numbers and warning state. |
| Continuity: did a real successor take over? | Operation/handoff IDs, predecessor/successor, transition states, acknowledgment and subsequent useful work. | State timeline with events and expandable correlated evidence. |
| Projects/runs/agents/tools: what is executing? | Selected exact project/run, queued/running/paused/blocked/completed state, active role/session, tool calls/outcomes/durations. | Run list, progress/activity timeline and compact outcome summaries. Counts describe work; they never enforce quotas. |
| Memory/diagnostics: what is durable or failed? | Available store health/count/size/generation, reset outcome, recent errors and correlated evidence. | Health cards and navigable event/details view with timestamps and scope. |

Surface other existing useful metrics in the appropriate detail view; do not delete them for scope convenience. It is acceptable for a genuinely unsupported hardware reading to have no value. It is not acceptable for a missing required Windows collector, run event path or page implementation to masquerade as unsupported hardware.

## Collection and lifetime
Inspect `ITelemetryService`, domain snapshots and ManagerDashboardOperationalDataSource before adding interfaces. The native service should own collector lifetimes through explicit injection; feature view models subscribe through the normal Manager transport. Collection cannot be a blocking operation on the XAML dispatcher or an out-of-band PowerShell polling script.

Use native supported CPU/RAM/process interfaces. For example, paired `GetSystemTimes` samples and `GlobalMemoryStatusEx`/`GetProcessMemoryInfo` support native host/process readings; inspect processor-group semantics and distinguish system totals from per-process values. E03/E04/E12 document the relevant interfaces. For optional GPU counters, inspect supported Windows APIs available on the target machine and record the exact scope and source. Do not introduce a vendor SDK/driver requirement solely for Alpha telemetry.

A small sampling cadence (for example about once per second while visible) and bounded recent history are suitable design defaults, not a performance certification. Reuse current cadence preferences where present. Bound chart/event memory, release subscriptions on close, and prevent duplicate consumers after reconnect. The Manager may retain recent history for reattachment; unlimited durable telemetry storage is not required. Native process handles and callbacks use RAII and clean shutdown ordering.

## Data semantics
Each reading needs an identity/scope, optional numeric value, unit, capture time, quality/availability and reason when not available. Reuse existing types where possible rather than installing a generic observability schema platform. Distinguish at least warming-up, measured, estimated, idle, stale, unsupported, disconnected and failed. An actual measured zero is valid and must not be confused with no value.

Use monotonic elapsed time for deltas/durations and a display timestamp for correlation. Reject non-finite values; handle an initial sample, a process restart and counter discontinuity as gaps/reset histories instead of enormous negative or positive spikes. Show the latest successful value with explicit stale age or omit the point; do not fabricate a live continuation line through disconnected intervals. Scope every resource reading accurately: machine CPU is not model CPU, working set is not GPU memory, and a lifetime token total is not retained prompt context.

The current Responses transport is explicitly non-streaming (S13). A total response duration is measurable; time-to-first-token or decode-only token throughput is not automatically available. Do not label `output_tokens / total_response_duration` as pure decoding speed; label it end-to-end output rate. Optional finer measurements need a real observed timing source. Do not introduce a streaming overhaul just to draw an unsupported metric.

## Context must agree with execution
The GUI displays the actual policy values, not its own calculation with unrelated totals. The intended decision is:

`retained_context + next_response_reserve + handoff_reserve + safety_margin >= effective_capacity`

Validate positive capacity and sensible non-overlapping reserves. Show exact configured values plus the effective operational values. The model's actual loaded context limit is a separate fact; show unknown/provider-unverified when it is not observable. Do not claim that a GUI slider resized a loaded model unless the supported provider operation really did so.

When Responses usage includes previously retained input, do not repeatedly sum those totals as if every token were new. Identify the meaning of the returned usage from the actual provider contract. Deduplicate by response/turn identity and maintain separate cumulative totals for telemetry. Include relevant queued tool output in the retained-context estimate only once. Keep estimates explicitly labeled, with reserve for uncertainty and recovery on a genuine context overflow. A new root resets retained context state, not historical usage counters. Do not change usage numbers or trigger on a tool counter merely to force acceptance.

## Layout and interaction
Make Rig answer “is the system healthy, what is working, and how close is the active run to rollover?” Keep an always-visible scope/status strip with selected project/run and Manager/provider state. Place resource trends and context headroom prominently; put controls beside the state they affect. Provide a simple time-window/detail selection where useful, without elaborate animation or faux instrument art.

Use direct labels and readable units. Keep essential values visible without hover. Trends need time axes and an honest missing-data treatment. Utilization/capacity charts should not exaggerate differences with unexplained truncated axes. Memory uses readable byte units; duration units are explicit. Do not combine unrelated measures on an unlabeled dual axis.

All charts have text/value equivalents; status uses words/icons as well as color. Respect Windows theme resources/high contrast, text/display scaling, keyboard focus and reduced-animation preferences. Native controls provide a useful accessibility foundation; custom visuals still need accessible names/value summaries. E02 is the platform reference. A single practical keyboard/scaling check suffices for each changed critical surface; do not turn this into a broad certification project.

## Proof
Use the actual native app and Manager for the collector/render check. Observe changing samples and an explicit disconnected/reconnected state. Verify readings against appropriately scoped Windows observations without claiming numerical identity between instruments with different intervals/normalization. Keep a few actual screenshots and a brief interaction record when available. Fixture screenshots may assist component development but are never actual machine/installed acceptance evidence. If UI automation or the display session is unavailable, keep that check open and continue independent implementation.

<!-- alpha-phase-review:start -->
Phase review: R5 — 2026-09-12. Implementation and verification status: [Product status](../../../STATUS.md).
Delivery/merge status is recorded by the linked phase pull request.
<!-- alpha-phase-review:end -->
