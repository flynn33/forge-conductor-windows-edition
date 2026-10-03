# Product status

Version 1.3.5 removes Managed Run and wires Auto Continuity for native LM Studio chats. See [release notes](releases/1.3.5.md) for the observed native predecessor/successor and verification scope.

| Area | Implemented behavior |
|---|---|
| Navigation | Workspace, Rig, Continuity, Activity, and Settings; no replacement run manager. |
| Workspace | Authoritative project/provider selection, ordered package queue, immediate CLU policy-folder binding, Auto Continuity preference, memory browsing, and readiness. |
| Packages | Universal inventory and streamed hashes; callable `instruction_package.read` returns the selected project queue row and paged content. |
| Status | `get_forge_status` returns project ID/folder and binding source, ordered package paths, policy source/revision, tool names/count, and agent count. |
| Governance | Bound repository reading, structured evidence evaluation, findings, model tool-result notifications, visible findings/correction history, and redacted export. |
| Auto Continuity | Primary MCP worker observes native selected-chat usage, pauses at a completed tool boundary, requests a detailed model packet, creates a native successor chat, hands it over, and verifies packet retrieval and a following Forge call. |
| Packet view | Saved packet list and detail, refresh, delete selected packet, and clear all packets. |
| Settings | Load/save/readback/revert/test/restart controls remain; record selection plus delete buttons replaces the old scope-maintenance scheme. |
| MCP | The same Primary, Fallback, and CLU integrations; exact 180-second deadline; no fourth plugin or new Forge credential. |
| Agents | Ten specialist playbooks and baseline agent-session tools remain callable. |
| Persistence/package | Existing per-user project/memory/policy stores and stable ForgeConductor.Windows MSIX identity. |

Auto Continuity was verified with a reserve-triggered pause, not physical context exhaustion. Rollover was verified while the primary MCP worker stayed alive. Interrupted handoff after idle-process eviction is not durable and is not claimed.

The native verification used an isolated Debug home, loaded `openai/gpt-oss-20b`, a 32,768-token capacity and 10,240-token total reserve. Native on-disk conversations independently contain the predecessor handoff call, successor handed message, all three integrations, packet retrieval, `agent_get`, and `get_forge_status`. Physical exhaustion, reattachment of an already-running agent, and interrupted-worker recovery were not exercised.

The complete x64 Release backend and App builds exited 0. The final configured CTest matrix reported `100% tests passed out of 153` and `Total Test time (real) = 31.54 sec`, with exit code 0. All three static gates passed. These Release results are separate from the nine affected Debug suites used during native-chat development.

The installed `ForgeConductor.Windows_1.3.4.0` app was not replaced for release construction. Its existing profile was not used as a test server home. Historical reports describe their own artifacts and do not establish qualification for newer binaries.
