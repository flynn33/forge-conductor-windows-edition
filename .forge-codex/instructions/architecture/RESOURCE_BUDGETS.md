# Resource budget interpretation

The machine-readable budgets are in `plans/resource-budgets.json`.

Budgets are acceptance requirements, not assumptions. Measure on representative Windows 11 hardware after warm-up in Release builds. Record OS build, CPU, GPU, RAM, display/DPI, model host state, and exact scenario.

A budget miss triggers profiling and repair. Do not raise a budget merely to pass. A changed budget requires an ADR with before/after evidence and an owner requirement that justifies the change.

Prioritize:

1. bounded work and memory;
2. no monotonic leaks;
3. responsive UI;
4. low idle CPU/GPU;
5. predictable behavior on 8 GB systems;
6. graceful degradation when counters or GPU features are unavailable.
