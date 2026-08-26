# Definition of done

The port is complete only when all items below are evidenced.

## Product

- All seven GUI surfaces work.
- CLI command parity works.
- All 53 MCP tools are listed and pass contract tests.
- Primary and fallback MCP roles pass independent handshakes.
- Multi-project memory is isolated, searchable, exportable/importable, resettable, and bounded.
- Legacy memory remains compatible.
- Autonomous continuity creates, bootstraps, acknowledges, seals, and resumes successor logical sessions.
- Manager is single-owner, restartable, and auto-start capable.
- Telemetry and gauges are correctly mapped, bounded, efficient, and accessible.
- Diagnostics, audit, feed, agents, tools, deployment, and settings are functional.
- macOS data import works transactionally.
- Security controls pass negative tests.

## Engineering

- Forsetti guardrails pass without framework patching.
- Debug/Release/ASan builds pass as applicable.
- Native unit/integration/protocol/UI/stress/fault tests pass.
- Resource budgets pass.
- No unbounded ownership issue remains.
- No Python or forbidden runtime is present.
- No automated-authorship attribution is present.

## Installer

- Native setup bootstrapper installs a signed MSIX bundle.
- Fresh install, launch, startup, repair, update, uninstall, reinstall, and purge pass.
- Data is preserved by default.
- CLI alias and MCP executable work after install.
- Artifacts have hashes, symbols, dependency manifest, and install documentation.

## Validation

- Every parity row is passed.
- Every hard gate is passed.
- Independent Validator and Release Manager sessions approve with evidence.
- Known issues are either none or explicitly accepted by an owner-level requirement; Codex may not self-accept a missing required feature.
