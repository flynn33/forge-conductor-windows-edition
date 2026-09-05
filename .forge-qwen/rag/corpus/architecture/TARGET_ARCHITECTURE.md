# Target Windows architecture

## Installed product

```text
ForgeConductor.App.exe          packaged WinUI 3 GUI
forge-conductor.exe             console CLI and stdio MCP host
ForgeConductor.Manager.exe      per-user manager and loopback dashboard
ForgeConductor.SessionHost.exe  Forge-owned logical model sessions and rollover
ForgeConductor.Setup.exe        native install/repair/update/uninstall bootstrapper
Forge first-party modules       isolated Forsetti app/service factories + manifests
Forsetti public runtime         sealed attached Windows framework
Windows App SDK resources       self-contained deployment
```

## Libraries

- `ForgeConductor.Domain`: IDs, value objects, policies, state machines, typed errors.
- `ForgeConductor.Contracts`: abstract interfaces only.
- `ForgeConductor.Application`: use cases and orchestration.
- `ForgeConductor.Infrastructure.Windows`: files, paths, process, DPAPI, registry, Winsqlite3, WinHTTP, ETW.
- `ForgeConductor.Mcp`: JSON-RPC stdio, schemas, tool routing, authorization.
- `ForgeConductor.Memory`: project registry, project and legacy memory.
- `ForgeConductor.Continuity`: checkpoints, handoffs, rollover/recovery.
- `ForgeConductor.AgentHost`: model provider and session-host adapters.
- `ForgeConductor.Manager`: single-owner manager, named-pipe IPC, HTTP/SSE dashboard.
- `ForgeConductor.Telemetry.Windows`: native collectors, pressure policy, bounded snapshots.
- `ForgeConductor.Rendering.Windows`: shared D3D11/D2D/DirectWrite resources.
- `ForgeConductor.Presentation.WinUI`: view models, views, accessibility.
- `ForgeConductor.ForsettiModule`: thin public Forsetti module boundary.

## Dependency direction

```text
Domain <- Contracts <- Application <- infrastructure/adapters/presentation/process hosts
```

Domain and Contracts have no WinUI, Win32 handle, database, network, graphics, or Forsetti implementation dependencies.

## Composition

Composition roots create concrete graphs through constructor injection. No mutable global service state and no hidden service locator after startup. Feature modules depend on `ForsettiCore` public headers only. Product hosts compose public Forsetti host/platform products and module factories.
