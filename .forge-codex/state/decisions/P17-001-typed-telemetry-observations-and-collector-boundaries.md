# P17-001: Typed Telemetry Observations and Collector Boundaries

Status: Accepted

Date: 2026-08-30

## Context

The inherited telemetry domain represents many measurements as mandatory scalar
values initialized to zero. That shape cannot distinguish a successful zero
measurement from a first sample, an unsupported Windows metric, an access
denial, or a failed refresh. It also cannot retain the last successful value
while reporting that the value is stale. G17 requires every projected field to
identify its verified Windows source and expose explicit availability and
staleness semantics.

P17 owns synchronous Windows collectors and source-to-domain mapping. P18 owns
the periodic producer, capacity-one latest-value mailbox, histories, consumer
delivery, and Manager composition. Replacing the Manager's deliberately
unavailable telemetry service during P17 would cross that phase boundary and
would create a producer without the required P18 lifecycle and backpressure
controls.

P16-030 used the broader phrase that P17 would replace the unavailable Manager
adapter. The canonical phase plan assigns latest-value delivery and bounded
history to P18. The conflict is resolved by interpreting P17's replacement as
completion of the production collector composition and deferring the actual
Manager service wiring to P18, where its producer and consumer lifetimes can be
implemented as one owned unit.

## Decision

- Add a generic `TelemetryMetric<T>` domain value containing an optional value,
  a closed availability state, a stale flag, an optional value-capture time, a
  optional current observation time, a bounded source identifier, and a
  bounded optional reason. A default warming-up metric may be never observed;
  measured values and concrete failure states require canonical timestamps.
- Use exactly these availability states: `available`, `warming_up`,
  `unsupported`, `temporarily_unavailable`, and `access_denied`.
- A successful zero is an available value. A metric with no successful value is
  represented without a value and is never synthesized as zero.
- `available` requires a value, `stale == false`, and no unavailable reason.
  An unavailable state without a prior value has no value and is not stale. An
  unavailable state may retain a prior value only with `stale == true`; it keeps
  the prior value, capture time, and source while adding the current bounded
  failure reason and observation time. A never-successful unavailable metric
  has no capture time; its observation time records when the state was seen.
  Capture time may not follow observation time.
- A metric becomes stale on the first failed scheduled refresh. P17 will not
  invent a time-to-live that is absent from the governing resource budgets.
- Retrofit telemetry categories incrementally, beginning with every RAM field.
  Scalar and collection-valued fields use the same observation envelope; an
  available empty collection remains distinct from an unavailable collection.
  Preserve the established macOS-compatible JSON field names. Emit `null` when
  a field has no value and add a bounded `availability` object keyed by those
  same field names.
- Implement P17 collectors as synchronous, threadless, non-overlapping `final`
  classes. Each collector owns bounded prior-sample state, checks cancellation
  and deadline before and after bounded native API blocks, and has an
  idempotent shutdown path.
- Add `ForgeConductor.Telemetry.Windows` as an application-owned native target
  that depends inward on Contracts and Domain only. It must not depend laterally
  on `Infrastructure.Windows` or modify sealed Forsetti modules.
- Map RAM physical capacity and utilization from `GlobalMemoryStatusEx`. Map
  commit pressure, committed bytes, and paged-pool bytes from
  `GetPerformanceInfo`. Report active, wired, swap, and compressed measurements
  as explicit unsupported values when no verified current-machine source with
  equivalent semantics is available; do not report zero.
- Leave the Manager on `UnavailableTelemetryService` in P17. P18 will compose
  the completed system collector into the bounded producer and consumer
  lifecycle.
- Keep G17 open after the RAM checkpoint. It may pass only after CPU, RAM,
  volume, disk-I/O, GPU, process, and power mappings and their availability and
  staleness fixtures are complete.

## Consequences

- Domain consumers cannot confuse unavailable measurements with successful
  zeroes for retrofitted fields.
- Existing JSON clients retain their established keys and gain additive state
  metadata.
- Partial Windows API failures can be represented per field without discarding
  successful measurements from the same collection.
- P17 remains independently testable without prematurely introducing the P18
  background thread, callback lifetime, history, or backpressure design.
- Call sites constructing RAM fixtures must now state whether each value was
  actually measured.

## References

- `governance/source/forsetti-agentic/`
- `.forge-codex/instructions/templates/cpp/include/ForgeConductor/Contracts/ITelemetryService.hpp`
- `.forge-codex/instructions/plans/phases.json`
- `.forge-codex/instructions/plans/gates.json`
- `.forge-codex/instructions/plans/feature-parity-matrix.tsv`
- `.forge-codex/instructions/plans/resource-budgets.json`
- `.forge-codex/state/decisions/OWNER-002-alpha-release-qualification-scope.md`
- `.forge-codex/state/decisions/P16-030-truthful-unavailable-telemetry-and-manager-reachability.md`
