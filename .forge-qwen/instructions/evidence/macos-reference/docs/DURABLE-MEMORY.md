# Durable memory MCP tools

Forge Conductor stores durable notes in SQLite under `FORGE_CONDUCTOR_HOME`
(default `~/.forge-conductor/store.sqlite`, table `memory_notes`). Notes survive
LM Studio chat sessions, model reloads, and MCP process restarts.

## Tools

| Tool | Purpose |
|------|---------|
| `memory_set` | Upsert `key` + `body` (+ optional `tags`) |
| `memory_get` | Fetch one note by `key` |
| `memory_list` | List notes (`prefix`, `tag`, `include_system`, `include_body`, `limit`) |
| `memory_delete` | Delete by `key` |
| `memory_search` | Substring search over key/body/tags |

## Behavior

- Available **without** an agent session (session lifecycle allow-list).
- Still available **during** agent sessions even if the agent’s `tools_primary`
  list omits memory tools.
- `agent_run/*`, `agent_active/*`, and `continuity/*` keys used by agent sessions
  and handoff pointers are **hidden** from `memory_list` / `memory_search` and
  `memory_note_count` unless `include_system: true`.
- Bodies are redacted in audit logs (`body` / `content` / `value`).

## Suggested key layout

```
task/current
project/<slug>
prefs/<name>
session/<date>
```

## Host bootstrap example

1. `forge_status` — includes `memory_note_count`
2. `memory_list` or `memory_get` for `task/current` / project keys
3. Work with tools / agents
4. `memory_set` before ending the chat

## Local development only

Build and test from this repo:

```bash
cd /path/to/Forge-Conductor-MacOS
swift test --filter MemoryToolTests
```

Do **not** point LM Studio at a debug binary until you intentionally deploy.
Production deploy remains: build → install / **Deploy to LM Studio**.
