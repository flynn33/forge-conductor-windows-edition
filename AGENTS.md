# Forge Conductor (Windows)

Native WinUI 3 / C++20 operator app and LM Studio MCP server.

## Rules

- Visual Studio solution is the source of truth (`ForgeConductor.sln`).
- Forsetti Framework is a **design guide only**. Do not copy or link that repo.
- One user-facing GUI. No operator CLI. Hidden `serve` is for LM Studio only.
- Dashboard is DirectX + WinUI. No HTML, WebView2, or port 7788.
- Interface-first, constructor DI, `final` classes, one-way dependencies.
- Tests use temp homes. Do not write to the live `%USERPROFILE%\.forge-conductor` unless the user asks.

## Build

```
msbuild ForgeConductor.sln /p:Configuration=Debug /p:Platform=x64
msbuild ForgeConductor.sln /p:Configuration=Release /p:Platform=x64
```

See `docs/ARCHITECTURE.md`.
