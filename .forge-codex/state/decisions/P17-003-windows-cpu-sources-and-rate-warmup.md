# P17-003: Windows CPU sources and rate warm-up

- Status: Accepted
- Date: 2026-08-30
- Phase: P17
- Gate: G17 remains open

## Context

The macOS CPU collector publishes aggregate utilization, one utilization value per logical processor, logical and physical processor counts, aggregate and per-logical frequency, a 1/5/15-minute load average, processor identity, and aggregate user/system/idle percentages. Its utilization fields are deltas between successive cumulative samples. The first macOS sample currently projects zeroes, but the Windows governing contract forbids using a placeholder zero for a value that has not yet been measured.

Windows exposes no single documented API with equivalent semantics for every field. The current-machine inventory also shows that nominal-frequency sources report about 3.4 GHz while the documented PDH `Actual Frequency` counter reports the processor's current boost behavior above 4 GHz. Windows' instantaneous processor queue length is not a Unix 1/5/15-minute load average.

P17-001 requires successful zeroes, unavailable values, and retained stale values to remain distinguishable. It also requires collection-valued fields to carry the same observation state as scalar fields.

## Decision

`CpuMetrics` has one `TelemetryMetric<T>` observation envelope for each established top-level CPU JSON field:

- `percent`, `user`, `system`, and `idle` use `TelemetryMetric<double>`;
- `per_cpu` uses `TelemetryMetric<std::vector<double>>`;
- `count_logical` and `count_physical` use `TelemetryMetric<std::uint32_t>`;
- `freq_mhz` uses `TelemetryMetric<std::uint32_t>`;
- `freq_per_core_mhz` uses `TelemetryMetric<std::vector<std::uint32_t>>`;
- `load_avg` uses one `TelemetryMetric<LoadAverage>` because its `m1`, `m5`, and `m15` leaves necessarily share one Windows availability/source state;
- `brand` uses `TelemetryMetric<std::string>` with a bounded UTF-8 value.

The dashboard preserves those eleven legacy keys and adds an `availability` object keyed by the same eleven names. An unavailable value is JSON `null`; an available empty collection remains `[]`; a stale value remains in its legacy key while metadata identifies it as stale.

The synchronous Windows CPU collector uses these exact sources:

1. Successive `GetSystemTimes` samples provide aggregate CPU deltas. Because Windows kernel time includes idle time, the collector calculates:
   - `total = deltaKernel + deltaUser`;
   - `idle = deltaIdle / total`;
   - `system = (deltaKernel - deltaIdle) / total`;
   - `user = deltaUser / total`;
   - `percent = (total - deltaIdle) / total`.
2. `PdhAddEnglishCounterW` with `\Processor Information(*)\% Processor Time` provides per-logical-processor utilization without depending on localized counter names.
3. `GetLogicalProcessorInformationEx(RelationProcessorCore)` provides both counts: physical cores are processor-core relationship records and logical processors are the bounded population count of their group masks.
   The platform observation also retains the exact bounded `(group, processor)` identities represented by those masks so PDH samples are accepted only when their identity set exactly matches topology; cardinality alone is insufficient.
4. `PdhAddEnglishCounterW` with `\Processor Information(*)\Actual Frequency` provides current frequency. The global `_Total` instance maps `freq_mhz`; numeric `group,index` instances map `freq_per_core_mhz`. Group-total instances are not logical processors. Finite positive PDH values are converted to the nearest whole MHz with checked range; missing or malformed aggregate and per-logical observations fail independently so one field cannot erase a valid sibling field.
5. `RegGetValueW` at `HKLM\HARDWARE\DESCRIPTION\System\CentralProcessor\0\ProcessorNameString` provides the bounded processor brand on the owner-qualified machine.
6. `load_avg` is `Unsupported` with source `Windows scheduling model: no 1/5/15 load-average equivalent`. `\System\Processor Queue Length` is not substituted because it has different units and no 1/5/15-minute decay semantics.

The first aggregate and per-logical rate samples are `WarmingUp` with an observation time and no value. The collector never sleeps to manufacture a delta; P18's manager-owned cadence supplies the next sample. Static topology, identity, and instantaneous frequency may be available on the first call.

PDH processor instances are accepted only when they are the global `_Total`, a group total, or a bounded numeric `group,index` pair. Numeric instances are sorted by group then processor number, duplicates are rejected, and every available per-logical vector must exactly match both the available logical count and the topology identity set. Counter failures retain prior values as stale at the field level. Aggregate-frequency and per-logical-frequency failures are distinct typed observations; an absent value without an explicit success, warm-up, or failure state is invalid. Counter buffers and instance counts are hard-bounded.

The collector owns its cumulative samples and a typed RAII PDH query through its injected platform boundary. It admits at most one synchronous operation, checks cancellation/deadline before and after every native block, and linearizes admission, publication, active release, and shutdown under one lifecycle lock. P18, not this collector, owns scheduling, the capacity-one latest-value mailbox, history, and production Manager service lifetime.

## Consequences

- A measured zero remains an available zero; first-sample warm-up and probe failures cannot masquerade as idle CPU.
- Current boost frequency is not weakened to nominal clock speed on this qualified machine.
- Windows does not fabricate Unix load-average values.
- The legacy dashboard field names remain compatible while every CPU field gains explicit availability, staleness, provenance, and timestamps.
- G17 remains open until RAM, CPU, disk, GPU, process, and power collectors and the complete field inventory are all qualified.
