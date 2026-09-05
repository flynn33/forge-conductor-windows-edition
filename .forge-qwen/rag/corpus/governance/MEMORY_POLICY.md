# Run-scoped memory policy

## Workspace selection

Memory never selects or changes the workspace. Verify `WORKSPACE_LOCK.json` first.

## Exact namespace

The lock provides one namespace:

```text
forge_windows_port::<run_id>::<workspace_fingerprint_prefix>
```

The only cross-session cursor key is:

```text
<namespace>::cursor
```

The handoff script generates the exact payload at `.forge-qwen/state/MEMORY_CURSOR.json`. Replace that exact key only when the active assignment requires `persistent_memory`. An absent cursor is normal; repository state remains sufficient.

Do not read, write, merge, or delete generic keys such as `forge_windows.*`, `continuity.latest`, `LATEST`, `current-task`, `workspace_adopted`, or any unscoped “latest handoff.” Ignore foreign memory.

## Persistent-memory cursor

The cursor contains only:

- mission ID;
- run ID;
- workspace fingerprint;
- target repository;
- expected branch;
- active microtask;
- exact latest handoff path and checksum;
- last validated commit;
- blocker IDs;
- one exact next action.

Reject it when mission, run ID, fingerprint, target, branch, or handoff checksum differs from repository state.

## Long-term memory

Use long-term memory only when the active assignment explicitly requires it. Store a compact, run-scoped index of approved ADR IDs or blocker IDs—never source text, project selection, task selection, or a “latest” pointer. Repository files remain canonical.
