# Step-by-step execution contract

Qwen does not choose a repository, phase, microtask, document set, or write scope. The controller supplies one `ACTIVE_ASSIGNMENT.json`.

For every session, follow these numbered stages in order.

## Stage 0 — lock

1. Print current directory.
2. Read the workspace lock.
3. Run the workspace assertion script.
4. Emit `WORKSPACE_LOCK_VERIFIED` in working notes.

No memory retrieval or source search is allowed before Stage 0 passes.

## Stage 1 — assignment

1. Read `ACTIVE_ASSIGNMENT.json`.
2. Confirm its run ID and fingerprint match the lock.
3. Read the generated step card.
4. Read only the listed documents.
5. Do not scan the entire package.

## Stage 2 — facts

Create a short file under `.forge-qwen/state/evidence/<microtask>/FACTS.md` with four headings:

- VERIFIED
- INFERRED
- UNKNOWN
- BLOCKED

Resolve material UNKNOWN items with tools before editing.

## Stage 3 — execute one change

1. Set the microtask `in_progress`.
2. Perform the next unchecked step card item.
3. Reread every changed file.
4. Run the listed focused command/test.
5. Check exit code and record evidence.
6. Continue only if the prior step passed or produced a documented blocker.

## Stage 4 — close

1. Compare changed paths against allowed write roots.
2. Update evidence and microtask status.
3. Write a checksummed handoff tied to run ID and fingerprint.
4. Generate `.forge-qwen/state/MEMORY_CURSOR.json` and replace only its exact `key` when persistent memory is required.
5. Write `session-result.json`.
6. Emit `FORGE_QWEN_RESULT`.

A narrative summary without these file changes does not count as progress.
