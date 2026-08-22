# Architecture

Forge Conductor for Windows is a Forsetti-**compliant** application. It does not depend on the Forsetti Framework repository.

## Layers (one-way)

```
ForgeConductor.exe (WinUI 3 + DirectX)
        → ForgeHost
            → ForgePlatform → ForgeRuntime
        → feature libraries (Domain, Persistence, Orchestration, Mcp,
           Telemetry, LmStudio, Comfy, Manager, GaugeKit, Ui)
```

- `ForgeRuntime` is portable C++20. No Windows headers.
- Feature modules never include each other. They share contracts through `ForgeDomain` and the host service container.
- One active app surface: `com.forge.module.app.operator`.
- Service modules may run together.

## Process modes

| Argv | Who | UI |
|---|---|---|
| (none) | User | WinUI dashboard |
| `serve` | LM Studio | none |
| `--headless-manager` | Task Scheduler | none |

## Persistence

`%USERPROFILE%\.forge-conductor` (override `FORGE_CONDUCTOR_HOME`), schema version 5, compatible with the macOS 0.8.0 store.

## Dashboard

Native Direct3D 11 + HLSL gauges. No browser surface.

## ComfyUI control

`ForgeComfy` is a native C++ service module (`com.forge.module.comfy`). Deploy writes a third LM Studio mcpBridge plugin, `comfy-control`, that runs `ForgeConductor.exe serve` with `FORGE_MCP_ROLE=comfy`.

That role exposes prepare-only Comfy tools (`comfy_prepare_video`, `comfy_system`, `comfy_nodes`, `comfy_workflow`). It talks to loopback ComfyUI over WinHTTP, records state in `comfy.sqlite`, and never `POST`s `/prompt` while `execution_policy` is `prepare_only`.

Python `lmstudio-comfy-control` remains a behavioral spec, not a runtime. Remote ComfyUI transport is configured as `comfy.transport` but `remote` is rejected until implemented.
