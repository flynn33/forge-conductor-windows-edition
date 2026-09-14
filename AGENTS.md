# Forge Conductor Windows delivery guidance

## Product definition

Deliver a finished Windows 11 x64 native C++20 and WinUI 3 application with Manager-owned execution, complete operational telemetry, accessible persistent Settings, project/MCP/native-tool workflows, context continuity, and a working signed MSIX release. Continue the existing implementation and Forsetti public interfaces. Do not replace it with a web, interpreted, or managed application runtime.

Current product truth is documented in `README.md`, `ROADMAP.md`, and `docs/STATUS.md`. The `.forge-alpha/` directory and `docs/implementation/alpha-recovery/` are historical delivery evidence, not active scope selectors.

## Workspace and preservation

Work only in `D:\GitHub\Forge-Conductor-Windows-Edition` against `https://github.com/flynn33/forge-conductor-windows-edition`. Preserve unrelated dirty work, configured inputs, and user data. Never hard-reset, force-push, clean away work, or modify a live database during validation. Use generated fixtures or disposable copies.

Ordinary use selects `%LOCALAPPDATA%\Forge Conductor`. Released schema-9 stores must open compatibly and older supported schemas must migrate through the immutable C001–C009 ledger. The historical `--alpha-root` option remains available only for backward-compatible isolated testing.

Production code remains modular object-oriented C++20 with WinUI 3/C++/WinRT, MSVC v143, the Windows SDK, native Win32/COM, WinHTTP, Windows SQLite, and the approved JSON dependency. Use constructor injection, explicit composition roots, and RAII. Keep GUI concerns separate from services; the Manager continues to own runs and services after the GUI closes. PowerShell is for build, test, packaging, and administrative automation.

## Execution and delivery

Ordinary inference enters through the typed Manager-owned run path that owns provider turns, authorized tool dispatch, telemetry, and continuity. Context consumption alone triggers automatic rollover. Preserve provider acknowledgment, predecessor fencing, productive successor work, project isolation, and explicit error states.

Run focused checks while editing, then the complete Release build, all CTest entries, static gates, package validation, and simulated lifecycle checks before release. Keep implementation, review, merge, and release publication as separate states.

Use normal pull requests targeting `main`; do not bypass review or merge permissions. Keep local and GitHub refs synchronized after an actual merge. Add no assistant/model attribution, generated-by notices, assistant coauthor trailers, or bot authorship. Never publish credentials, databases, build output, or unrelated state.
