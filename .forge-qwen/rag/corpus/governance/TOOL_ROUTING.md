# Lock-aware tool routing

## Before workspace verification

Only filesystem and shell capabilities may be used, and only for:

1. printing the current directory;
2. checking for `WORKSPACE_LOCK.json` in that exact directory;
3. reading the lock and `READ-THIS-FIRST-QWEN.txt`;
4. running `Assert-QwenWorkspace.ps1`.

Before the lock passes, do not call:

- RAG;
- GitHub;
- long-term memory;
- persistent memory;
- Forge memory tools;
- Forge continuity/context tools;
- Git status/log/diff;
- broad filesystem search.

## After workspace verification

Use the active assignment’s required capabilities only.

1. Filesystem reads the assignment and routed files.
2. RAG locates relevant material inside the prepared corpus.
3. Filesystem verifies exact files and symbols.
4. Shell runs focused commands and tests.
5. Filesystem writes the smallest patch and rereads it.
6. GitHub is used only after local branch/commit identity is verified against the lock.
7. Long-term/persistent memory stores only the exact run-scoped cursor.
8. JS sandbox validates pure data transformations only.

## Product-under-test isolation

Do not use Forge Conductor’s own `memory_*`, `project_memory.*`, `context_*`, `session_*`, or `continuity.*` tools to control Qwen’s build session. Those tools are the product under test. Test them only inside their assigned isolated protocol microtasks.

An automatically loaded Forge continuity packet is foreign execution context unless it carries the exact package run ID and workspace fingerprint. Even then, the repository lock remains authoritative.

## Failure

One malformed call: reread the exact tool schema and retry once. Two failures: use an equivalent bound capability if present. Three no-progress failures: write a blocker and close the microtask. Never loop or switch repositories.
