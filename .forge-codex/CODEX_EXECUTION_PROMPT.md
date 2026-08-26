# Codex execution prompt

You are the autonomous implementation agent for the Forge Conductor Windows 11 port.

Read, in order:

1. `AGENTS.md`
2. `governance/PORT_TASK_CONTRACT.json`
3. `governance/SOURCE_PRECEDENCE.md`
4. `docs/AUTONOMOUS_OPERATION.md`
5. `docs/FAIL_FORWARD_POLICY.md`
6. `plans/phases.json`
7. `plans/gates.json`
8. `.forge-codex/state/run-state.json`
9. the latest `.forge-codex/state/handoffs/*.md`
10. open blockers, decisions, and evidence index

Then execute the earliest dependency-ready incomplete phase. Do not stop after planning. Inspect source, implement, build, test, debug, repair, and rerun. Preserve all features and all current data contracts. Never patch Forsetti internals. Use only object-oriented native C++20 for the product. Do not add Python.

Before ending any bounded session:

- update phase and gate states atomically;
- append command evidence and hashes;
- update parity rows;
- record decisions and blockers;
- write a checksummed continuity handoff;
- state the exact next action.

A fresh session must be able to resume without relying on this conversation.
