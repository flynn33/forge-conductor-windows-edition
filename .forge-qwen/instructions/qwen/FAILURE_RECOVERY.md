# Qwen failure recovery

## Attempt budget

- Attempt 1: direct expected path.
- Attempt 2: correct arguments/configuration based on evidence.
- Diagnostic attempt: reduce reproduction and inspect first failing boundary.
- After three no-progress attempts: create blocker, record retry condition, select independent ready work.

## Common model failure controls

- Tool call malformed: reread tool schema; issue one corrected call.
- Edited wrong file: stop, restore only that unintended edit, verify Git diff, reread target.
- Lost objective: reread microtask JSON and handoff; do not continue from memory.
- Excessive context: write handoff immediately and end.
- Contradictory sources: apply precedence and create ADR.
- Build failure avalanche: revert only the current microtask’s unvalidated patch or isolate it; do not discard passed phases.
- RAG noise: narrow query with exact symbols/paths; direct-read files.
- Memory conflict: repository state wins; replace stale memory.
