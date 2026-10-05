# Roadmap

## Current release: 1.3.11

Version 1.3.11 repairs selected-project deployment, legacy instruction-package bootstrap, filesystem tool contracts, project-scoped continuity lookup, complete native tool delivery, and Windows request-path authorization. It also includes the direct package-queue and policy-source browsing improvements developed since 1.3.6. The existing telemetry layout repair and three LM Studio integrations remain. See [release notes](docs/releases/1.3.11.md).

Auto Continuity was verified with a reserve-triggered pause, not physical context exhaustion. Rollover was verified while the primary MCP worker stayed alive. Interrupted handoff after idle-process eviction is not durable and is not claimed.

## Follow-up

- Make interrupted native handoff recovery durable across primary MCP worker eviction/restart.
- Exercise physical context exhaustion and reattachment of an already-running agent separately from reserve pressure.
- Validate additional LM Studio versions/models and Windows accessibility/scaling configurations.
- Complete an installed-window visual walkthrough when the native-control driver initializes, and validate another disposable Windows account.

Historical Alpha milestones describe their own artifacts and do not establish qualification for current binaries.
