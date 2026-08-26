# Target architecture

## Composition

Forge Conductor for Windows is a Forsetti consumer application composed from public contracts:

```text
ForgeConductor.App.exe (WinUI 3 host)
  └─ ForsettiHostTemplate
      └─ ForgeConductorAppModule
          └─ ForgeCompositionRoot
              ├─ Domain
              ├─ Application
              ├─ Infrastructure.Windows
              ├─ Protocol.MCP
              ├─ Telemetry.Windows
              ├─ Rendering.Windows
              └─ Presentation.WinUI

forge-conductor.exe
  └─ same Domain/Application/Infrastructure composition without WinUI

ForgeConductor.Manager.exe
  └─ single-owner per-user manager, IPC, dashboard, telemetry, recovery

ForgeConductor.SessionHost.exe / ForgeNativeSessionHostPlugin.dll
  └─ autonomous logical-session host and provider adapters

ForgeConductor.Setup.exe
  └─ native installer bootstrapper for MSIX, dependencies, startup, repair, and uninstall
```

## Required projects/libraries

- `ForgeConductor.Domain` — value types, IDs, state machines, errors, policies.
- `ForgeConductor.Contracts` — abstract interfaces only.
- `ForgeConductor.Application` — use cases and orchestration.
- `ForgeConductor.Infrastructure.Windows` — filesystem, process, registry, DPAPI, paths, Winsqlite3, networking, ETW.
- `ForgeConductor.Mcp` — newline-delimited JSON-RPC stdio server and tool adapters.
- `ForgeConductor.Telemetry.Windows` — collectors and latest-value delivery.
- `ForgeConductor.Rendering.Windows` — shared D3D11/D2D/DirectWrite/Composition services.
- `ForgeConductor.Presentation.WinUI` — view models, navigation, views, accessibility.
- `ForgeConductor.ForsettiModule` — thin public Forsetti module boundary.
- `ForgeConductor.Cli`, `ForgeConductor.Manager`, `ForgeConductor.SessionHost`, `ForgeConductor.App`, `ForgeConductor.Setup`.
- Native unit, integration, protocol, UI automation, stress, and installer test projects.

## Dependency direction

```text
Domain
  ↑
Contracts
  ↑
Application
  ↑
Infrastructure / MCP / Telemetry / Rendering / Presentation / Process hosts
```

Domain and Contracts must not include WinUI, Windows App SDK, Winsqlite3, WinHTTP, DirectX, registry, process, filesystem implementation, or Forsetti implementation headers.

## Forsetti boundary

`ForgeConductorAppModule` is the only app/UI Forsetti module. It declares the capabilities and runtime requirements actually used. It exposes UI contributions through public Forsetti interfaces and delegates behavior to the application composition root. CLI, manager, and MCP hosts reuse application libraries but do not patch or subclass sealed Forsetti classes.

## Process ownership

- GUI owns windows and presentation state only.
- Manager owns the loopback dashboard, periodic telemetry collection, durable recovery, deployment reconciliation, and startup lifecycle.
- MCP server processes own one stdio connection and close all resources on EOF/cancellation.
- Session host owns autonomous model sessions and rollover operations.
- Setup owns install/repair/update/uninstall transactions.
- Only one process may own each exclusive resource; cross-process coordination uses authenticated named pipes, mutexes, file locks, and database transactions.

## No hidden global state

Composition roots create services explicitly. Process-wide facilities such as ETW providers or shared graphics devices must be owned by one root object and injected through interfaces. Global accessor functions and mutable static service state are prohibited.
