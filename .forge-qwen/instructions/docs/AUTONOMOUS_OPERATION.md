# Autonomous operation

The execution system uses `.forge-qwen/state` as a durable control plane. One fresh Qwen context executes one microtask. The optional LM Studio API controller exposes only required plugins, starts a fresh chat per microtask, permits at most two continuations, verifies state/hash progress, and stops on corruption or repeated no-progress.

Role contexts are separate: Architect, Builder, Validator, Documentation Manager, Release Manager. Builder never passes its own gate. A Validator starts from current repository/evidence, performs a clean build/test, adversarial failure path, and emits pass/request-changes/block.

Qwen selects the earliest dependency-ready microtask, not the largest phase. A blocked task does not prevent independent ready tasks. Completion requires hard gates and independent decisions.
