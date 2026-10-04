# Windows capability map

This map describes version 1.3.6. Historical delivery records retain their original scope.

| Capability | Implementation and bounds |
|---|---|
| Native shell | WinUI 3 Workspace, Rig, Continuity, Activity, and Settings; existing visual design retained. |
| Telemetry layout | Collapsed diagnostic status and refresh-progress exclusion prevent the measured periodic Rig movement; updates continue. |
| Project context | Authoritative project ID/root, ordered instruction packages, development-policy identity, tool names/count, and agent count in `get_forge_status`. |
| Package reading | `instruction_package.read` selects a project queue row and returns paged entries/content with revision identity. |
| CLU | Immediate policy-repository folder binding, policy readback, evidence evaluation, findings, model notifications, correction receipts, and visible history. |
| Native chat continuity | Primary worker reads actual selected-chat usage, pauses at a completed tool boundary, obtains a model packet, opens/sends a native successor, and verifies recovery plus a following call. |
| Packet controls | Continuity list/detail/refresh/delete-selection/clear. |
| Settings records | Load/save/readback/revert/test/restart retained; selectable saved records and delete buttons. |
| MCP/agents | Existing Primary and Fallback general catalogs, five CLU governance tools, and ten baseline specialist playbooks/session tools. |
| Persistence/package | Existing per-user stores, retained migration history, stable ForgeConductor.Windows identity, native x64 build/package workflow. |

Auto Continuity was verified with a reserve-triggered pause, not physical context exhaustion. Rollover was verified while the primary MCP worker stayed alive. Interrupted handoff after idle-process eviction is not durable and is not claimed.
