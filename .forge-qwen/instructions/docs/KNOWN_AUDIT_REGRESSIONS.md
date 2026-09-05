# Audit regressions that are release blockers

The embedded audit is a direct anti-regression input. The Windows product must prove:

- telemetry delivery has one in-flight UI application and one replaceable newest value;
- no per-gauge graphics device/queue/pipeline/timer graph;
- no replacement buffer allocation at telemetry cadence;
- hidden/dismissed/minimized views stop recurring graphics work;
- no duplicated history mutation or broad redundant UI invalidation;
- all long-lived tasks, timers, callbacks, subscriptions, delegates, processes, pipes, files, sockets, SQLite statements, native allocations, and network sends have explicit bounded owners/cleanup;
- no main-thread blocking I/O;
- all histories/caches/logs/streams/queues/process output are bounded;
- all telemetry/gauge fields have a unique verified source and unavailable state;
- leak/performance claims use same-flow runtime evidence.

Complete `plans/AUDIT_REGRESSION_MAP.tsv` before gate G26.
