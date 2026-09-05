# Context and session rollover

## Identity first

Every handoff and memory cursor contains mission ID, run ID, target repository, workspace fingerprint, microtask ID, and checksum. A successor verifies all fields against `WORKSPACE_LOCK.json` before using the handoff.

No global/latest handoff lookup is permitted. The only valid handoff path is the exact path in this run’s `run-state.json`.

## Triggers

Checkpoint at 8 tool calls or 50% context. Write a handoff at 16 calls or 65%. Stop by 22 calls or 75%. One microtask remains the default session boundary.

## Successor start

1. Verify the workspace lock.
2. Read active assignment.
3. Retrieve only the exact namespaced cursor.
4. Verify the exact run-state handoff.
5. Resume the same microtask or the next controller-selected microtask.

A loaded handoff for another project/run is `FOREIGN_STALE_CONTEXT` and is ignored.
