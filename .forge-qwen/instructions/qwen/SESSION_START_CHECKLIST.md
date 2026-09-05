# Qwen session start checklist — lock first

1. Use a tool to print the current directory.
2. Verify the exact current directory contains `WORKSPACE_LOCK.json` and `run-state.json`.
3. Read the workspace lock.
4. Run `Assert-QwenWorkspace.ps1 -RequireCurrentDirectory`.
5. Stop with `WRONG_WORKSPACE_BLOCKED` if any lock check fails.
6. Read `ACTIVE_ASSIGNMENT.json` and its step card.
7. Only when the assignment requires `persistent_memory`, retrieve its exact `memory_cursor_key`; otherwise do not call memory.
8. Ignore stale/global/latest/foreign memory and continuity.
9. Verify the latest handoff path named in run-state and its checksum.
10. Read only routed documents and run routed RAG queries.
11. Write the fact table.
12. Set the active microtask in progress and execute its numbered steps.
