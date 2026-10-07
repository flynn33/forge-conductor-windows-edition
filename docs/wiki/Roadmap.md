# Roadmap

## Current candidate: 1.3.18

Version 1.3.18 supplies 104 Primary/Fallback tools while preserving all original tools, ten specialist playbooks and five CLU tools. Dedicated web, native Office documents, desktop/browser, native image previews, independent managed workers and persistent schedules join owner-selected host/workspace filesystem access. `image_analyze` adds a fresh independent read-only image analysis with output available through `reviewer_status`. Existing shell/process jobs, reviews, memory, policy, deployment and continuity remain. Final build, installation and current-chat qualification are pending. See the [capability guide](Windows-workflow-capabilities) and [release notes](Release-1.3.18) for implementation bounds and actual verification.

Auto Continuity was verified with a reserve-triggered pause, not physical context exhaustion. Rollover was verified while the primary MCP worker stayed alive. Interrupted handoff after idle-process eviction is not durable and is not claimed.

## Follow-up

- Make interrupted native handoff recovery durable across primary MCP worker eviction/restart.
- Exercise physical context exhaustion and reattachment of an already-running agent separately from reserve pressure.
- Validate additional LM Studio versions/models and Windows accessibility/scaling configurations.
- Complete an installed-window visual walkthrough when the native-control driver initializes, and validate another disposable Windows account.

Historical Alpha milestones describe their own artifacts and do not establish qualification for current binaries.
