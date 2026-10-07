# Windows capability map

This map describes version 1.3.14 and implemented native workflow bounds. The [capability guide](HOST-CAPABILITIES.md) explains the new request/receipt contracts; [verification](validation/HOST-CAPABILITIES-1.3.14.md) records executed checks separately from final release qualification. Historical delivery records retain their original scope. Generative image providers and cloud accounts require separate configured connections or authorized APIs.

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
| MCP/agents | Primary and Fallback 103-tool catalogs, retaining all original 80 tools, five CLU governance tools, and ten specialist playbooks with matching embedded fallbacks. |
| Web | Dedicated native WinHTTP `web_search`, `web_fetch` and authorized bounded `http_request`; actual source URLs, HTTP status and truncation. Search challenges are explicit and fetched text is untrusted. |
| Office | Native DOCX/XLSX/PPTX ZIP packages from structured text/typed cells, validated before authorized atomic publication; no Office or Python dependency. Sample packages passed Microsoft Open XML SDK schema validation. Formula evaluation and visual rendering are separate capabilities. |
| Desktop/browser | Visible-window/PID listing, bounded accessibility text, browser launch, observed authorized input and visible-region PNG capture. Launch/input acceptance requires subsequent observation; OS desktop/integrity permissions still apply. |
| Images | Native structured PNG drawing and Windows-codec local image previews. Additive managed worker/reviewer Responses image content preserves text metadata and call identity, with a 512 KiB encoded PNG bound; fresh loopback-provider image-content checks passed, while current Qwen vision interpretation remains pending. Generative artwork requires a separately configured image provider. |
| Independent workers | Fresh Manager-owned provider histories, frozen roots/grants/tool allowlists, distinct receipt store, bounded total lifetime, actual state/output/cancellation and interruption evidence. Worker scope excludes recursive/control mutations, including legacy session_checkpoint/session_handoff; existing authorized Primary/Fallback routes, specialists and reviewers remain. |
| Schedules | Separately authorized atomic persistence; UTC/interval model tasks, current-policy intersection, no overlap, actual histories and meaningful notifications. Manager must remain running; future triggers restore after restart, uncertain effects require explicit authorized retry. |
| Process work | Synchronous shell capped at 120 seconds; synchronous/tracked PowerShell accepts 65,536 UTF-8 bytes through supervised stdin. Tracked shell/executable launch has a 3,600-second limit, two active jobs, sixteen results in service memory and thirty-two durable jobs per project, live named logs, bounded waits, verified adoption, and Manager ownership across MCP reconnects. All launch cwd checks use the locally active project roots before Manager dispatch. |
| Inspection | Native Windows process/service snapshots, loaded LM Studio model/context facts with unknown provenance, and fixed read-only GitHub repository routes. |
| Evidence authority | Owner-selected host access to ordinary available local volumes or retained workspace/configured-root mode; relative artifact paths keep the selected project default. Deliberate narrowing, native ACL/path/reparse protections and exact configured bindings remain. UTF-8 writes retain a 2 MiB file-layer cap and smaller 1 MiB encoded MCP request limit; streamed SHA-256 and the capture chain retain their bounds. |
| Verification/review | Tracked external pinned Python venv creation and observed version manifest; separate Manager reviewer session with authorized file or 64 KiB inline opening, read-only tools by default, optional text-only mode, per-turn receive timeout 1–3,600 seconds (default 600), sealed timeout persistence and infrastructure failure status. |
| Persistence/package | Existing per-user stores, retained migration history, stable ForgeConductor.Windows identity, native x64 build/package workflow. |

Auto Continuity was verified with a reserve-triggered pause, not physical context exhaustion. Rollover was verified while the primary MCP worker stayed alive. Interrupted handoff after idle-process eviction is not durable and is not claimed.
