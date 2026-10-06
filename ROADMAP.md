# Roadmap

## Current release: 1.3.13

Version 1.3.13 repairs local-model reviewer deadlines and opening inputs, recovered evidence-root bindings, consistent process cwd authorization, nested filesystem writes, and 65,536-byte PowerShell script delivery. Primary/Fallback retain all 80 tools and ten specialist playbooks; CLU retains five tools. Existing deployment, native tool delivery, package/policy browsing, telemetry, persistence, and continuity features remain. See [release notes](docs/releases/1.3.13.md).

Auto Continuity was verified with a reserve-triggered pause, not physical context exhaustion. Rollover was verified while the primary MCP worker stayed alive. Interrupted handoff after idle-process eviction is not durable and is not claimed.

## Follow-up

- Make interrupted native handoff recovery durable across primary MCP worker eviction/restart.
- Exercise physical context exhaustion and reattachment of an already-running agent separately from reserve pressure.
- Validate additional LM Studio versions/models and Windows accessibility/scaling configurations.
- Complete an installed-window visual walkthrough when the native-control driver initializes, and validate another disposable Windows account.

Historical Alpha milestones describe their own artifacts and do not establish qualification for current binaries.
