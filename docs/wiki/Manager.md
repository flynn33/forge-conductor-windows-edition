# Manager

The Manager owns the existing per-user project, tool, memory, policy, settings, deployment, telemetry, and persistence services. WinUI sends typed commands through ManagerConnection and ManagerRequestDispatcher rather than editing databases or LM Studio configuration itself.

Sessions are LM Studio chats. The historical Managed Run/readback UI remains removed. The primary stdio MCP composition owns native chat Auto Continuity, whose in-flight UI handoff phase is not recoverable after idle-worker eviction. Version 1.3.19 retains independent Manager-owned model workers and persistent schedules through dedicated MCP routes; these use fresh histories and separate receipts and do not replace the user's native chat.

## Product actions

Workspace selects/registers projects, manages ordered packages, and binds a selected policy repository directly from the folder picker. Settings retains Load effective settings, Save and read back, Revert pending edits, Test LM Studio, and Restart Manager. Saved record actions remain selection plus buttons; the old scope-maintenance scheme is removed.

Rig's Runtime configuration card exposes Ensure manager, Restart service and Stop service through the existing handlers. Wait for owned work to finish or confirm terminal cancellation before restarting/stopping Manager. Scheduled triggers need Manager to remain running. The controls are present in the 1.3.19 App source; installed UI acceptance requires its separate executed record.

Independent worker scopes exclude legacy `session_checkpoint` and `session_handoff` continuity mutations while retaining their authorized Primary/Fallback tools. Managed worker/reviewer Responses outputs add bounded PNG image content with text metadata and original call identity; text-only outputs keep their existing form. Fresh native/loopback-provider exclusion and image cases passed. The complete disposable-profile schedule lifecycle and reconnect probe also passed; current installed Qwen vision support and final package acceptance remain separately pending.

Install or repair remains one action for Primary, Fallback, and CLU, preserving foreign registrations and unknown fields. Existing provider configuration and telemetry remain. No credential or login step is added.

Auto Continuity was verified with a reserve-triggered pause, not physical context exhaustion. Rollover was verified while the primary MCP worker stayed alive. Interrupted handoff after idle-process eviction is not durable and is not claimed.

Retained backend run/session records are compatibility history, not a replacement product run manager or a claim of native-chat recovery. See [Process Model](Process-Model) and [Continuity](Continuity).

The current `agent_spawn/poll/cancel` and `schedule_create/list/cancel/run_now` services preserve frozen/current-policy scope, actual state/output/cancellation and explicit uncertain-effect records. Manager must remain running for schedule triggers. Shutdown stops the schedule loop and cancels its actual workers before shutting down worker transport/storage. Local Windows notifications persist submission acceptance/error with display unconfirmed; they do not send outbound messages or change Windows notification settings. See [Windows Workflow Capabilities](Windows-workflow-capabilities).
