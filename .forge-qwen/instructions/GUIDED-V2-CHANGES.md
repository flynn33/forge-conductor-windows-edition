# Guided v2 changes

This revision addresses Qwen selecting a stale macOS/remediation workspace from continuity or persistent memory.

Added:

- exact workspace lock and fingerprint;
- memory retrieval only after lock verification;
- run-specific memory namespace;
- foreign-context classification;
- read-only source/instruction roots;
- prohibition on package-local/foreign remediation `work/`, macOS `.forge-codex`, and Forsetti remediation state as active work;
- a pre-tool workspace microtask;
- machine-generated active assignment and step card;
- exact first-session and recovery prompts;
- lock-bound handoffs and session results;
- strict current-directory assertion;
- expected Windows-port Git branch;
- attached autonomy-plan scope note.

The model is no longer asked to “review the package and determine where work stopped.” The controller tells it exactly where it is, what it may read, what it may write, and what numbered action comes next.

## Additional supervisor controls

- exact per-run `MEMORY_CURSOR.json` and cursor key;
- previous session result reset when each assignment is prepared;
- session result rejected when it belongs to another microtask;
- routed-document existence check before a chat starts;
- persistent memory used as a cursor, with long-term memory exposed only to selected tasks;
- curated RAG denylist and no automatic external-root indexing;
- assignment-generated numbered step cards with exact commands and stop conditions.
