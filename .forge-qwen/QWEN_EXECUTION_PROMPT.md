# Lock-first bounded execution prompt

Perform these steps in order. Do not skip or reorder them.

1. Establish the current directory with a tool.
2. Read `.forge-qwen/state/WORKSPACE_LOCK.json` from that exact directory.
3. Run `Assert-QwenWorkspace.ps1 -RequireCurrentDirectory`.
4. Begin working notes with `WORKSPACE_LOCK_VERIFIED <run_id> <workspace_fingerprint>`.
5. Read `.forge-qwen/state/ACTIVE_ASSIGNMENT.json`.
6. Confirm assignment run ID/fingerprint equal the workspace lock.
7. When—and only when—the active assignment requires `persistent_memory`, retrieve the exact `memory_cursor_key`; otherwise do not call memory. Ignore every generic, latest, global, or mismatched key.
8. Read only the assignment’s step card and routed documents.
9. Execute the step card one numbered step at a time.
10. Record facts, edits, commands, exit codes, tests, evidence, and blockers.
11. Stay inside the assignment’s allowed write roots. Never write to read-only or forbidden roots.
12. Write a run-bound checksummed handoff and `session-result.json`.
13. Emit `FORGE_QWEN_RESULT`.

Do not broadly inspect another repository, review a prior remediation run, search for a work folder, or infer where work left off. The controller already selected the exact task.
