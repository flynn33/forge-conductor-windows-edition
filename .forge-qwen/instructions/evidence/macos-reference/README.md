# Forge Conductor (macOS)

Native **Swift** control plane and MCP server for **local models in [LM Studio](https://lmstudio.ai)** on macOS.

This project is **not** Claude Code orchestration, CCDT, or `~/.claude/local-mcp`.

| | |
|---|---|
| **Version** | **0.9.0** |
| **User guide** | [USER-GUIDE.md](USER-GUIDE.md) |
| **Changelog** | [CHANGELOG.md](CHANGELOG.md) |
| **License** | [Apache License 2.0](LICENSE) |
| **Platform** | macOS 26+ |
| **Wiki** | [Project wiki](https://github.com/flynn33/Forge-Conductor-MacOS/wiki) |
| **Contributions** | **Closed** — see [CONTRIBUTING.md](CONTRIBUTING.md) |

### How LM Studio connects

LM Studio is the MCP **host**. It spawns a Forge **stdio** server:

| Executable | Argv | Role |
|------------|------|------|
| `…/Forge Conductor.app/…/Forge Conductor` | `serve` | **Default MCP** via `ForgeProcessEntry` (Deploy to LM Studio) |
| Selected app or CLI executable | `serve` + `FORGE_MCP_ROLE` | Independent primary and fallback MCP registrations |
| App (LaunchAgent) | `manager run --home …` | Dashboard manager (`install-login`) |
| App (double-click) | _(none)_ | SwiftUI GUI |

**Product path (v0.5+):** GUI → **LM Studio MCP** → **Deploy to LM Studio**. Forge transactionally writes primary + failover configuration, triggers LM Studio reload (relaunching it only if needed), verifies LM Studio synchronized the exact revision, and independently smokes both tool servers. Details: [`docs/LM-STUDIO-CONNECTION.md`](docs/LM-STUDIO-CONNECTION.md), operator test plan: [`docs/RELEASE-0.5.0-TEST.md`](docs/RELEASE-0.5.0-TEST.md).

```bash
# After building products — does NOT write LM Studio by itself:
forge-conductor install
# Explicit deploy (same as GUI Deploy to LM Studio):
forge-conductor install-lmstudio-plugin
# GUI: LM Studio MCP tab → Deploy to LM Studio
```

No manual LM Studio configuration-file edit or restart is required. Selecting which plugins a model may use remains a per-chat LM Studio choice.

## Requirements

- macOS 26+
- Swift 6 / Xcode toolchain
- [LM Studio](https://lmstudio.ai) for running local models

## Quick start

```bash
cd /path/to/Forge-Conductor-MacOS
# Reproducible native app build, bundle staging, and launch:
./script/build_and_run.sh --verify
# Build and stage the app without launching or touching the live Forge home:
./script/build_and_run.sh --build-only

# Full Core/CLI/connector acceptance suite:
swift test

# CLI install (from built product or SPM release)
forge-conductor install
forge-conductor doctor
forge-conductor manager start --open   # native dashboard / manager
# LM Studio starts MCP via ~/.lmstudio/mcp.json → forge-conductor serve (Swift stdio)
```

## What the UI shows

| Surface | Meaning |
|---------|---------|
| **FORGE RIG** | Host telemetry (CPU/GPU/RAM/disk) + LM Studio-oriented load |
| **LM Studio MCP** | LM Studio host, model backends, Forge MCP from `mcp.json` / live processes |
| **Agents / Tools / Feed** | Playbooks and tool audit for local-model agent runs |
| **Manager** | Start/Stop HTTP control plane, settings, doctor |

**Never listed:** CCDT, Claude Code `local-mcp` binaries, project-continuity (foreign projects).

The LaunchAgent manager is the single owner of the loopback dashboard port. Opening the SwiftUI app attaches to that manager through a native typed client; it does not start a competing listener. The app uses a persistent button-based navigation column; use its toolbar button or the **Navigation** menu to show or hide it.

## Architecture

| Layer | Responsibility |
|-------|----------------|
| **Domain** | Typed models (`ForgeSnapshot`, `AppConfig`, agent sessions) |
| **Infrastructure** | SQLite, paths, process runner, PDF, audit |
| **Application** | `ForgeApp`, catalog, sessions, tool packs |
| **MCP** | JSON-RPC stdio for LM Studio (`tools/list`, `tools/call`) |
| **Dashboard / App** | SwiftUI + Metal gauges; optional loopback HTTP |
| **CLI** | install / doctor / serve / manager |

State: `~/.forge-conductor` (`FORGE_CONDUCTOR_HOME` override).  
LM Studio MCP config: `~/.lmstudio/mcp.json`.  
Durable memory notes: SQLite `memory_notes` (see [docs/DURABLE-MEMORY.md](docs/DURABLE-MEMORY.md)).
Context and agent handoffs: SQLite `context_handoffs` with rebuildable JSON/Markdown projections (see [docs/CONTEXT-AGENT-CONTINUITY.md](docs/CONTEXT-AGENT-CONTINUITY.md)).

More detail: [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) and the [project wiki](https://github.com/flynn33/Forge-Conductor-MacOS/wiki).

## What's new in 0.9.0

This minor release adds durable, project-scoped memory and completes the runtime
reliability work around continuity, resource ownership, telemetry, and local tool
authorization.

| Area | What changed |
|------|--------------|
| **Project memory** | Twelve `project_memory.*` tools provide bounded search, optimistic updates, links, batch writes, health, and checksummed import/export. |
| **Continuity** | A serialized coordinator and native session-host adapter preserve handoff state across process and chat boundaries. |
| **Runtime** | Resource policy, lifecycle ownership, diagnostics, bounded latest-value telemetry, and shared Metal resources reduce unbounded work and retained state. |
| **Security** | Workspace authority is derived from trusted roots; `shell_exec` is disabled by default and capped at 120 seconds when explicitly enabled. |

Legacy `memory_*` and `session_*`/`context_*` tools remain compatible. Full release
notes and qualification boundaries are in **[CHANGELOG.md](CHANGELOG.md)**.

## Design principles

1. OOP modules + DI via `ForgeApp.bootstrap`.
2. Apple-native stack (Foundation, SQLite3, Network, Metal) — no Node/Python core.
3. **LM Studio is the host** for local models; Forge is the MCP tool server + rig.
4. Durable sessions, memory notes, and context/agent handoffs in SQLite for local-model agent runs.
5. SwiftPM acceptance tests and native GUI compilation gate release builds; Xcode remains the distribution/signing project.

Current 0.9.0 qualification evidence is recorded in
[`.forge-codex/evidence/P12-final-validation-report.md`](.forge-codex/evidence/P12-final-validation-report.md).
Earlier audit records remain available under `docs/`.

## CLI

```
forge-conductor install
forge-conductor install-lmstudio-plugin [--binary PATH]
forge-conductor doctor
forge-conductor status
forge-conductor agents
forge-conductor serve                 # MCP stdio (LM Studio client)
forge-conductor manager run [--open]
forge-conductor manager start|stop|restart|status
forge-conductor version               # prints 0.9.0 (and related build info)
```

## Changelog

See **[CHANGELOG.md](CHANGELOG.md)** for the full version history.

- **0.9.0** — Project memory, coordinated continuity, runtime ownership, and security hardening
- **0.8.0** — Automatic continuity budgets, workspace resume, and MCP presence
- **0.7.0** — Context and agent continuity (`session_*`, `context_*`), resume-ready handoffs
- **0.6.0** — Durable memory MCP tools (`memory_*`)
- **0.5.x** — LM Studio deploy path, agents, tool packs, rig / manager

## License

Copyright 2026 Jim Daley.

Licensed under the **Apache License, Version 2.0**. See [LICENSE](LICENSE) and [NOTICE](NOTICE).

You may use, copy, modify, and redistribute this software under that license.

## Contribution policy

**No outside contributors are invited.**

Developers may use the software under the Apache 2.0 license, but must **fork** this repository or **copy** it into a **new repository** they control. Outside developers are **not** allowed to submit pull requests or otherwise alter this repository. **No pull requests will be approved.**

Full policy: [CONTRIBUTING.md](CONTRIBUTING.md).

## Sponsors

Support development of Forge Conductor and related projects:

**[Sponsor @flynn33 on GitHub](https://github.com/sponsors/flynn33)**
