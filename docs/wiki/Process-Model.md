# Process model

<p align="center"><img src="images/diagram-architecture.png" alt="Four native executables. LM Studio owns the chat. The Manager owns durable work. ComfyUI stays external." width="100%"></p>

Four native executables share the stable MSIX identity `ForgeConductor.Windows` 1.3.29.0.

| Process | Responsibility |
| --- | --- |
| ForgeConductorApp.exe | Workspace, Rig, Continuity, Activity, and Settings. Typed commands only. Views do not spawn processes, read databases, or rewrite LM Studio configuration. |
| ForgeConductor.Manager.exe | Projects, memory, settings, policy, deployment, telemetry, schedules, ComfyUI jobs, the legacy image jobs, and the visible-chat observer. |
| forge-conductor.exe serve | stdio MCP for Primary, Fallback, and CLU. Primary reports authorized chat observations. `serve` is the external MCP entry. |
| ForgeConductor.SessionHost.exe | Retained execution component. It is not proof that a provider session exists. |

LM Studio owns the chat. ComfyUI, its interpreter, Edge, and FFmpeg stay outside Forge. Ordinary data lives at `%LOCALAPPDATA%\Forge Conductor`. The package leaves that directory unvirtualized, so uninstall does not remove it.

Since 1.3.28 the visible-chat observer is a Manager transition worker. Primary supplies the observations. Fallback and CLU do not start a second observer. The installed 1.3.28 reserve-pressure delivery continued after the predecessor connector processes exited, with the same Manager still running. Physical context exhaustion, Manager reconstruction during delivery, UI interruption, and already-running agent reattachment remain unverified.

The published source `2e77c5f` retains the registered-CLI cold bootstrap introduced at `e6e97d3`. The installed cold connector reached its matching Manager in 2.1128484 seconds; that Manager survived connector and supervisor-job closure. The installed probe did not place the CLI inside the supervisor job, so inherited-job containment remains unexercised in that probe. The separate complete native process regression passed 40,610 assertions under its retained 90-second bound. See [Release 1.3.29](Release-1.3.29) for the distinct installed and fixture results.

Closing the app window stops UI refresh. It does not stop LM Studio or the Manager. Schedules still need the Manager to keep running.

Earlier notes that describe a process-local handoff belong to those earlier artifacts. See [Architecture](Architecture) and [Continuity](Continuity).
