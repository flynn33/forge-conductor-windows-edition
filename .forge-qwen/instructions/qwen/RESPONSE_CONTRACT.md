# Response and state contract

Do not begin with a narrative such as “runtime healthy,” “continuity loaded,” “workspace adopted,” or “previous work found.” Those are not lock evidence.

The first verified working-note line after tools must be:

```text
WORKSPACE_LOCK_VERIFIED <run_id> <workspace_fingerprint>
```

When lock verification fails, the only allowed terminal status is:

```text
WRONG_WORKSPACE_BLOCKED <reason>
```

No Git/source/remediation inspection follows a wrong-workspace result.

The final line after a valid assignment remains:

```text
FORGE_QWEN_RESULT {"run_id":"...","workspace_fingerprint":"...","microtask_id":"...","status":"passed|in_progress|blocked|failed","handoff":".forge-qwen/state/handoffs/...json","next_microtask":"...|null"}
```

Before emitting it, write `session-result.json`. Detailed evidence belongs in files. Do not print private chain-of-thought.
