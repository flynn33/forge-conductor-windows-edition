# Changelog

## Unreleased

### Fixed

- Product version is 0.8.0 in the binary, MCP serverInfo, plugin manifests, WiX package, and MSI filename
- MCP smoke uses a temp `FORGE_CONDUCTOR_HOME` and also checks the `comfy` role
- Operator single-instance lock is held for the life of the GUI (second launch no longer opens another window)
- Operator GUI prunes MCP presence older than 30 seconds on startup so leftover smoke heartbeats do not look live

### Added

- Native `ForgeComfy` library and `comfy-control` LM Studio plugin (`FORGE_MCP_ROLE=comfy`)
- Prepare-only video setup: start/discover ComfyUI, import/validate API graphs, `comfy_prepare_video` next steps
- Deploy writes primary, fallback, and `comfy-control` mcpBridge plugins
- WinHTTP loopback client and `comfy.sqlite` under the Forge home

## [0.8.0] — 2026-08-15

First Windows native release. Behavioral parity with Forge Conductor macOS 0.8.0.

### Added

- Visual Studio C++20 solution (`ForgeConductor.sln`)
- Native GUI operator dashboard (Direct3D 11 gauges)
- Hidden `serve` mode for LM Studio MCP (34 tools)
- SQLite schema 5 home under `%USERPROFILE%\.forge-conductor`
- Deploy to LM Studio from the GUI
- Manager start/stop and Start with Windows
- Per-machine MSI installer with Start Menu shortcut
