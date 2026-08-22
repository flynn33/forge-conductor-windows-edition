# Forge Conductor (Windows)

Native **C++20** operator dashboard and MCP server for local models in [LM Studio](https://lmstudio.ai).

| | |
|---|---|
| **Version** | **0.8.0** |
| **Platform** | Windows 11 (x64) |
| **IDE** | Visual Studio 2022 / 2026 (`ForgeConductor.sln`) |
| **License** | Apache 2.0 |

This is a **GUI application**. There is no operator CLI. Open **Forge Conductor** from the Start Menu.

## What you get

- Native dashboard (Direct3D 11 gauges) — not a browser
- 34 MCP tools for LM Studio over stdio
- Deploy to LM Studio from the **LM Studio MCP** page
- Doctor, agents, tools, live feed, manager — all in the app
- Per-user MSI install / uninstall

## Build

Open `ForgeConductor.sln` in Visual Studio, or:

```powershell
.\scripts\build.ps1 -Configuration Debug
.\scripts\test.ps1
.\scripts\build.ps1 -Configuration Release
.\scripts\package.ps1
```

## Architecture

See [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md). Forsetti Framework is a **design guide only** and is not a dependency.

## User guide

See [USER-GUIDE.md](USER-GUIDE.md).
