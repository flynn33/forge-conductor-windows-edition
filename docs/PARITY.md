# Windows capability map

Forge Conductor is a native Windows 11 x64 source candidate. This map records the implemented surface; package construction, installed interaction, and operator acceptance remain separate evidence gates. Historical 0.9.x acceptance records remain available under `docs/implementation/alpha-recovery/`.

| Capability | Windows implementation |
|---|---|
| Native desktop shell | C++20/WinUI 3 navigation shell with typed Manager attachment, all operator destinations, keyboard operation, High Contrast support, and scaled-text layouts. |
| Manager lifecycle | Per-user native Manager owns runs, telemetry, continuity, and long-running services independently of GUI attachment. |
| Provider integration | Configurable LM Studio endpoint/model discovery and Responses transport with persistent settings, validation, visible failure states, and reconnect behavior. |
| Managed inference and continuity | Manager-owned turns, tool dispatch, response correlation, context-triggered rollover, handoff, pause/resume/stop, and retained run state. |
| Projects and memory | Stable project identities, project isolation, relinking, legacy and project memory, guarded full-list paging, record-level deletion, generation fencing, and scoped reset behavior. |
| MCP and native tools | Native stdio MCP host, authenticated Manager routing, 53-tool catalog, filesystem/search/Git/shell operations, and LM Studio deployment/verification with an exact 180-second outer deadline for all three roles. |
| Operational telemetry | Native CPU, RAM, GPU-engine/memory, disk, volume, process, provider, store, workflow, and continuity observations with bounded histories and freshness. |
| Operational surfaces | Four normal destinations—Workspace, Rig, Activity, and Settings—compose the typed project, provider, continuity, memory, tools, feed, runtime, diagnostics, Manager, and maintenance capabilities. |
| Settings and maintenance | Effective readback, save/revert, provider controls, startup/logging/session controls, context configuration, scrollable per-record/multi-selection deletion, and separate confirmed scoped/global reset. |
| Persistence compatibility | Production `%LOCALAPPDATA%\Forge Conductor` profile; central schema versions 3, 5, 6, 7, 8, 9, 10, and 11 are explicitly handled. Released version 9 migrates through guarded C010/C011 without fabricated provenance or project scope, current version 11 reopens byte-stably, and future versions are refused without mutation. |
| Installer | Stable `ForgeConductor.Windows` MSIX identity and a signed x64 Release packaging workflow with verified hashes/provenance, an install helper, a public certificate for development builds, and optional App Installer updates. The 1.3.5 payload is not yet built. |
| Release automation | Windows Release CI plus tag/manual secret-backed production signing and artifact publication workflows. |

The macOS implementation is behavioral reference material only. Optional browser dashboards, additional CPU architectures, public update hosting, and advanced analytics/connectors are post-1.0 enhancements rather than missing core product behavior.
