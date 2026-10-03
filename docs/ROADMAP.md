# Roadmap

## Current release: 1.3.5

Native LM Studio chat Auto Continuity, callable selected-package reading, authoritative workspace disclosure, direct policy-folder binding, packet browsing/deletion, and requested UI/callable-route removals are implemented. The native app architecture and three existing integrations remain in place. See [release notes](releases/1.3.5.md).

Auto Continuity was verified with a reserve-triggered pause, not physical context exhaustion. Rollover was verified while the primary MCP worker stayed alive. Interrupted handoff after idle-process eviction is not durable and is not claimed.

## Follow-up

- Make interrupted native handoff recovery durable across primary MCP worker eviction/restart.
- Exercise physical context exhaustion and reattachment of an already-running agent separately from reserve pressure.
- Validate additional LM Studio versions/models and Windows accessibility/scaling configurations.
- Validate the package on a separate disposable Windows account without replacing the preserved host baseline.

Historical Alpha milestones describe their own artifacts and do not establish qualification for current binaries.
