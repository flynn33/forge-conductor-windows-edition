# Windows capability map

Forge Conductor 1.0 is a complete native Windows 11 x64 product. This map records the implemented release surface; historical 0.9.x acceptance records remain available under `docs/implementation/alpha-recovery/`.

| Capability | Windows 1.0 implementation |
|---|---|
| Native desktop shell | C++20/WinUI 3 navigation shell with typed Manager attachment, all operator destinations, keyboard operation, High Contrast support, and scaled-text layouts. |
| Manager lifecycle | Per-user native Manager owns runs, telemetry, continuity, and long-running services independently of GUI attachment. |
| Provider integration | Configurable LM Studio endpoint/model discovery and Responses transport with persistent settings, validation, visible failure states, and reconnect behavior. |
| Managed inference and continuity | Manager-owned turns, tool dispatch, response correlation, context-triggered rollover, handoff, pause/resume/stop, and retained run state. |
| Projects and memory | Stable project identities, project isolation, relinking, legacy and project memory, generation fencing, and scoped reset behavior. |
| MCP and native tools | Native stdio MCP host, authenticated Manager routing, 53-tool catalog, filesystem/search/Git/shell operations, and LM Studio deployment/verification. |
| Operational telemetry | Native CPU, RAM, GPU-engine/memory, disk, volume, process, provider, store, workflow, and continuity observations with bounded histories and freshness. |
| Operational pages | Typed Rig, Autonomy, Continuity, Projects, Memory, Tools, Feed, Runtimes, Provider, Events, Diagnostics, Manager, and Settings surfaces. |
| Settings and maintenance | Effective readback, save/revert, provider controls, startup/logging/session controls, context configuration, and confirmed scoped/global reset. |
| Persistence compatibility | Production `%LOCALAPPDATA%\Forge Conductor` profile; central schema versions 3, 5, 6, 7, 8, and 9 are explicitly supported, with byte-stable version-9 open and non-mutating future-version refusal. |
| Installer | Stable `ForgeConductor.Windows` MSIX identity, signed x64 Release payload, verified hashes/provenance, install helper, public certificate for development builds, and optional App Installer updates. |
| Release automation | Windows Release CI plus tag/manual secret-backed production signing and artifact publication workflows. |

The macOS implementation is behavioral reference material only. Optional browser dashboards, additional CPU architectures, public update hosting, and advanced analytics/connectors are post-1.0 enhancements rather than missing core product behavior.
