# Guided microtask protocol

A microtask is selected by `ACTIVE_ASSIGNMENT.json`. Qwen never selects one from memory or by exploring the repository.

## Start

1. Verify workspace lock in the exact current directory.
2. Read active assignment and step card.
3. Retrieve the lock-scoped memory cursor only when `persistent_memory` is listed in the active assignment; otherwise do not call memory.
4. Verify the exact handoff path in run-state.
5. Read only routed documents.
6. Write the fact table.

## Execute

- One objective.
- Follow the step card in order.
- Default maximum 5 changed files, 400 lines, 18 tool calls.
- Compile after structural changes.
- Run the narrowest test after behavior changes.
- No broad cleanup, package review, task selection, repository migration, or unrelated refactor.
- No write outside assignment roots.

## End

Update state, evidence, ledger, run-bound handoff, namespaced memory cursor, and session result. Then emit the marker.
