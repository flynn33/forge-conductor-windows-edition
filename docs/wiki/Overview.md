# Overview

The current Forge Conductor 1.3.25 source implementation is a native Windows 11 workspace and MCP server for project work with local models in LM Studio. Its existing WinUI design, project identities, persistent memory/policy/settings, native telemetry and stable package identity remain. Integrated 1.3.25 source, package, installed Manager/LM Studio and native-model qualification remain pending. Historical [published 1.3.21 qualification](Release-1.3.21) identifies its own source and binaries. See [1.3.25 source notes](Release-1.3.25).

The 1.3.25 source catalog contains 112 Primary/Fallback tools, adding six optional ComfyUI generation/edit/job routes while retaining native CMake/CTest, web, Office, desktop/browser, PNG drawing/image analysis, independent workers, schedules and host-capability inspection. Accessibility reads page through fresh-tree indices; optional preview dimensions retain the 256 default and 512 KiB base64 bound. Schedules require a running Manager, frozen/current-policy intersection and explicit authorization after uncertain effects. The optional ComfyUI `sd1` provider is disabled by default and requires an owner-configured loopback endpoint plus an existing compatible checkpoint. `image_provider_status` reads current configuration and core-node/checkpoint inventory; inventory availability does not establish a loaded model or image quality. See [local image-provider guide](https://github.com/flynn33/forge-conductor-windows-edition/blob/main/docs/IMAGE-PROVIDER.md) and [Windows Workflow Capabilities](Windows-workflow-capabilities).

The retained repairs keep activated evidence roots through recovery, align cwd checks before Manager dispatch, support 64 KiB supervised UTF-8 PowerShell scripts, and make independent reviewer receive deadlines configurable. Reviewer openings can use authorized files or bounded inline text; read-only tools remain the default. Windows tool requests accept supported path separators before strict authority validation. The earlier stable-refresh layout fix remains.

Sessions are LM Studio chats. Managed Run and the requested action/maintenance controls are removed. Auto Continuity is a primary MCP worker that observes the selected native chat, requests a detailed model packet at reserve pressure, creates a visible successor through product Windows UI Automation, and verifies resumed Forge calls.

| Component | Responsibility |
|---|---|
| ForgeConductorApp.exe | Workspace, Rig, Continuity, Activity, Settings; typed user actions |
| ForgeConductor.Manager.exe | Projects, tools, persistence, settings, policy, deployment, telemetry |
| forge-conductor.exe serve | Native stdio MCP roles; Primary owns native chat Auto Continuity |
| ForgeConductor.SessionHost.exe | Retained session-host compatibility component |
| ForgeConductor.Windows | Stable MSIX identity |

Primary/Fallback retain the general catalog; CLU exposes five governance tools. Ten specialist playbooks and baseline agent-session tools remain. Install/repair remains one action for the existing three plugins.

Historical rollover was verified with reserve pressure and a live primary worker. Accepted 1.3.11 native checks verify observing state and project-scoped pickup; a new rollover, physical exhaustion, and interrupted-worker recovery remain outside that check. See [Continuity](Continuity).

See [Product Surfaces](Product-Surfaces), [Continuity](Continuity), and [Release 1.3.11](Release-1.3.11).
