# Roadmap

## Current release: 1.3.6

Version 1.3.6 fixes the periodic telemetry-refresh layout movement while retaining telemetry cadence. Actual WinUI geometry with an injected isolated Manager stayed stable across 16 polls. The existing native-chat Auto Continuity, package/policy/status improvements, app design, and three integrations remain. See [release notes](docs/releases/1.3.6.md).

Auto Continuity was verified with a reserve-triggered pause, not physical context exhaustion. Rollover was verified while the primary MCP worker stayed alive. Interrupted handoff after idle-process eviction is not durable and is not claimed.

## Follow-up

- Make interrupted native handoff recovery durable across primary MCP worker eviction/restart.
- Exercise physical context exhaustion and reattachment of an already-running agent separately from reserve pressure.
- Validate additional LM Studio versions/models and Windows accessibility/scaling configurations.
- Complete an installed-window visual walkthrough when the native-control driver initializes, and validate another disposable Windows account.

Historical Alpha milestones describe their own artifacts and do not establish qualification for current binaries.
