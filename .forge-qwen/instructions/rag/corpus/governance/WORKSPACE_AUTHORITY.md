# Workspace authority

## One source of truth

Only `.forge-qwen/state/WORKSPACE_LOCK.json` may establish the active Windows-port repository. The lock is created by the bootstrap script and binds:

- mission ID;
- run ID;
- target repository absolute path;
- repository fingerprint;
- expected branch;
- exact memory namespace;
- source-baseline roots;
- read-only roots;
- writable roots;
- forbidden roots.

Conversation history, long-term memory, persistent memory, model continuity, an adopted `cwd`, Git branch name, recent modification time, a package `work/` folder, and a global “latest” handoff are never workspace authority.

## Required start order

1. Establish current directory through a tool.
2. Read and verify the exact workspace lock in that directory.
3. Run the workspace assertion script.
4. Read the active assignment.
5. Only then retrieve the run-scoped memory cursor.
6. Execute the assignment.

The previous start order—memory first—has been removed because stale cross-project memory can redirect a session before repository identity is proven.

## Foreign context

Treat a continuity or memory record as `FOREIGN_STALE_CONTEXT` when any of these differ from the lock:

- mission ID;
- run ID;
- repository fingerprint;
- target path;
- expected branch;
- memory namespace.

Do not merge, reinterpret, or follow foreign context. Record a concise ignored-context event without storing the foreign payload.

## Read-only baselines

These are evidence only:

- the instruction package;
- `.forge-inputs/**`;
- `.forge-qwen/instructions/**`;
- macOS source `.forge-codex/**` state;
- Forsetti remediation directories;
- the attached autonomy plan.

Never run their selectors, resume their handoffs, or treat their Git state as the Windows port state.
## Absolute preflight rule

Do not run Git first. Print the current directory, verify `WORKSPACE_LOCK.json`, and run the workspace assertion before any Git command. No global/latest memory key, continuity pointer, or handoff is authoritative; only the exact run-scoped cursor and handoff named by the lock-bound state may be used.
