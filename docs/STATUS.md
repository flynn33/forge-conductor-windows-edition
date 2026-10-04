# Product status

Version 1.3.6 repairs telemetry-refresh layout movement with two behavioral lines: collapse the diagnostic ManagerState row and keep Action::Refresh out of generic connecting-progress text. It retains the 1.3.5 native-chat/product behavior below. See [1.3.6 release notes](releases/1.3.6.md) for the measured layout repair and [1.3.5 release notes](releases/1.3.5.md) for native predecessor/successor evidence.

| Area | Implemented behavior |
|---|---|
| Navigation | Workspace, Rig, Continuity, Activity, and Settings; no replacement run manager. |
| Workspace | Authoritative project/provider selection, ordered package queue, immediate CLU policy-folder binding, Auto Continuity preference, memory browsing, and readiness. |
| Packages | Universal inventory and streamed hashes; callable `instruction_package.read` returns the selected project queue row and paged content. |
| Status | `get_forge_status` returns project ID/folder and binding source, ordered package paths, policy source/revision, tool names/count, and agent count. |
| Governance | Bound repository reading, structured evidence evaluation, findings, model tool-result notifications, visible findings/correction history, and redacted export. |
| Auto Continuity | Primary MCP worker observes native selected-chat usage, pauses at a completed tool boundary, requests a detailed model packet, creates a native successor chat, hands it over, and verifies packet retrieval and a following Forge call. |
| Telemetry layout | Real WinUI observer with injected Manager measured zero Rig position transitions across 16 polls; injected CPU values kept updating. |
| Packet view | Saved packet list and detail, refresh, delete selected packet, and clear all packets. |
| Settings | Load/save/readback/revert/test/restart controls remain; record selection plus delete buttons replaces the old scope-maintenance scheme. |
| MCP | The same Primary, Fallback, and CLU integrations; exact 180-second deadline; no fourth plugin or new Forge credential. |
| Agents | Ten specialist playbooks and baseline agent-session tools remain callable. |
| Persistence/package | Existing per-user project/memory/policy stores and stable ForgeConductor.Windows MSIX identity. |

Auto Continuity was verified with a reserve-triggered pause, not physical context exhaustion. Rollover was verified while the primary MCP worker stayed alive. Interrupted handoff after idle-process eviction is not durable and is not claimed.

The native verification used an isolated Debug home, loaded `openai/gpt-oss-20b`, a 32,768-token capacity and 10,240-token total reserve. Native on-disk conversations independently contain the predecessor handoff call, successor handed message, all three integrations, packet retrieval, `agent_get`, and `get_forge_status`. Physical exhaustion, reattachment of an already-running agent, and interrupted-worker recovery were not exercised.

Version 1.3.6 Release backend, App, and package builds exited 0. Its final configured CTest matrix reported `100% tests passed out of 153` and `Total Test time (real) = 32.36 sec`, exit code 0; all three static gates passed. The earlier 1.3.5 native-chat evidence and nine affected Debug suites are separate historical checks.

The installed 1.3.4 app was preserved during 1.3.5 construction. The later authorized 1.3.6 repair installed the original signed `ForgeConductor.Windows_1.3.6.0_x64__wj2yg5ac9gadp` package, with App and Manager running and App/Manager/CLI hashes matching its payload. Publication preserves that exact immutable package from source commit `9912541debc92c1117cceab0f6322c4e62728ee4`; documentation updates on main do not change product source or the published package. No installed-window visual inspection is claimed: native-control initialization failed with MODULE_NOT_FOUND for kernel.js. Historical reports describe their own artifacts and do not establish qualification for newer binaries.
