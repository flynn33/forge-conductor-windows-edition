# Roadmap

## Current release: 1.3.14

Version 1.3.14 expands Primary/Fallback to 103 tools while preserving all original tools, ten specialist playbooks and five CLU tools. Dedicated web, native Office documents, desktop/browser, PNG/image previews, independent managed workers and persistent schedules join owner-selected host/workspace filesystem access. Existing shell/process jobs, reviews, memory, policy, deployment and continuity remain. See the [capability guide](docs/HOST-CAPABILITIES.md) and [release notes](docs/releases/1.3.14.md) for implementation bounds and executed verification.

Auto Continuity was verified with a reserve-triggered pause, not physical context exhaustion. Rollover was verified while the primary MCP worker stayed alive. Interrupted handoff after idle-process eviction is not durable and is not claimed.

## Follow-up

- Make interrupted native handoff recovery durable across primary MCP worker eviction/restart.
- Exercise physical context exhaustion and reattachment of an already-running agent separately from reserve pressure.
- Validate additional LM Studio versions/models and Windows accessibility/scaling configurations.
- Complete an installed-window visual walkthrough when the native-control driver initializes, and validate another disposable Windows account.

Historical Alpha milestones describe their own artifacts and do not establish qualification for current binaries.
