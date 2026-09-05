# Resource budgets

Machine-readable budgets are in `plans/resource-budgets.json` for constrained (≤8 GiB), standard (8–16 GiB), and expanded (>16 GiB) systems. Memory-pressure notification reduces optional retention/cadence, not durability/security/handoff correctness.

Every queue/cache/history/log/body/process/stream exposes capacity, current use, high-water mark, overflow/coalesced/dropped/evicted counters, and diagnostics. A budget miss triggers same-workload profiling and root-cause repair, not a larger limit without owner decision/ADR.
