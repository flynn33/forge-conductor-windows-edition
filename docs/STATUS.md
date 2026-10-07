# Product status

Version 1.3.14 expands Primary/Fallback to 103 tools while retaining all 80 existing tools, ten specialist playbooks and CLU's five tools. Dedicated web, Office, desktop/browser, image, independent-worker and schedule workflows join owner-selected host/workspace filesystem modes. See [capabilities](HOST-CAPABILITIES.md), [release notes](releases/1.3.14.md) and [verification](validation/HOST-CAPABILITIES-1.3.14.md). Native exported Office samples passed Microsoft SDK schema validation; full release, package and live-model qualification are recorded only after execution. Earlier records below retain their original artifact provenance and limits.

The fresh x64 Release source test command passed **162/162 CTest entries in 58.10 seconds**, with all three static gates and package-persistence contract passing. The complete disposable-profile direct MCP probe passed ten groups and 63 requests with an owned loopback provider, including native host/continuity, Office/images, workers, persistent schedules and exact reconnect evidence. Current focused checks include thirteen scheduler groups, four normal desktop/image groups, nine filesystem/search groups and all 67 persistence internal tests. These source/native-fixture results do not qualify the clean shipping App, signed package installation or current Qwen acceptance, which remain pending.

## Historical 1.3.12 qualification

<!-- qualification-1.3.12 -->

Version 1.3.12 native Release qualification passed 156/156 CTest checks and all three static gates. The signed 1.3.12.1 installation, matching payload hashes, three native integration roles, Manager-owned 125-second reconnect/adoption, separate read-only reviewer, and provider/host readbacks passed. The release notes distinguish earlier source-probe evidence from the final payload; these checks do not execute the user's original project gate.

<!-- /qualification-1.3.12 -->

The installed 1.3.11 Release suite passed 153/153; signed installation, all three role bindings, 31 disposable feature calls and final loaded-Qwen native acceptance passed. Native readback verifies all 270 policy entries, exact pinned package/policy text and the complete six-fragment large shell result. Detailed evidence and unexercised rollover/GUI limits are in [installed qualification](validation/LM-STUDIO-REPAIR-1.3.11.md). The public package is rebuilt from the Jim Daley release commit; its clean source/staging provenance and publication checks are attached to the GitHub release. Publication does not reinstall the host or rerun the earlier native GUI conversation. Historical records below retain their own binaries and limitations.

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
| Shell/process jobs | Primary/Fallback lifecycle, live named logs, bounded waits and verified adoption; 65,536-byte UTF-8 PowerShell scripts, maximum job lifetime 3,600 seconds, two active jobs, sixteen results in memory and thirty-two durable jobs per project. All cwd routes validate locally before Manager dispatch. Manager-backed work survives MCP reconnect; owner shutdown terminates active work. |
| Review/evidence | Separate read-only reviewer contexts, optional supplied-text mode, file/inline opening sources and 1–3,600-second per-turn receive waits; full SHA-256-verified receipts, sixteen-record admission cap and 256 KiB report cap. Exact verification pins, activated external roots retained through recovery, and a durable capture chain preserve observed evidence. |
| Context/readback | Latest-provider-generation telemetry retains sample provenance/unknowns; status distinguishes current packet retrieval through `context_get` from packet storage and job memory attachment. |
| Agents | Ten specialist playbooks and baseline agent-session tools remain callable. |
| Persistence/package | Existing per-user project/memory/policy stores and stable ForgeConductor.Windows MSIX identity. |

Auto Continuity was verified with a reserve-triggered pause, not physical context exhaustion. Rollover was verified while the primary MCP worker stayed alive. Interrupted handoff after idle-process eviction is not durable and is not claimed.

The native verification used an isolated Debug home, loaded `openai/gpt-oss-20b`, a 32,768-token capacity and 10,240-token total reserve. Native on-disk conversations independently contain the predecessor handoff call, successor handed message, all three integrations, packet retrieval, `agent_get`, and `get_forge_status`. Physical exhaustion, reattachment of an already-running agent, and interrupted-worker recovery were not exercised.

Version 1.3.6 Release backend, App, and package builds exited 0. Its final configured CTest matrix reported `100% tests passed out of 153` and `Total Test time (real) = 32.36 sec`, exit code 0; all three static gates passed. The earlier 1.3.5 native-chat evidence and nine affected Debug suites are separate historical checks.

The installed 1.3.4 app was preserved during 1.3.5 construction. The later authorized 1.3.6 repair installed the original signed `ForgeConductor.Windows_1.3.6.0_x64__wj2yg5ac9gadp` package, with App and Manager running and App/Manager/CLI hashes matching its payload. Publication preserves that exact immutable package from source commit `9912541debc92c1117cceab0f6322c4e62728ee4`; documentation updates on main do not change product source or the published package. No installed-window visual inspection is claimed: native-control initialization failed with MODULE_NOT_FOUND for kernel.js. Historical reports describe their own artifacts and do not establish qualification for newer binaries.
