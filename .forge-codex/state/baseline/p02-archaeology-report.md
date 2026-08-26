# P02 Source Archaeology Baseline

The canonical immutable input is Forge Conductor macOS 0.9.0 (3e344d4b3bb0fff80487f99a7c69e7ceadf22aa1e64da3a6f2640ea2fa0072dd). It contains 870 files, 133 Swift files, and 33763 Swift lines.

The supplied audit and several pre-bootstrap baseline files describe an older 0.8.0/34-tool snapshot. They are retained as historical anti-regression evidence and are not used as the Windows parity contract. The current source declares 53 MCP tools, 84 planned feature rows, 269 unit/integration test methods, and 5 UI test methods.

Key reconciliations:

- The router advertises tool names in sorted order; plan order is retained separately.
- 'agent_run_status' and 'project_memory.export' are mutating in source.
- Target-only reset, Windows rendering/security, data migration, and packaging rows identify their requirement origin instead of claiming macOS source evidence.
- Telemetry must expose availability and staleness. Historical placeholder zero values are explicitly excluded.
- 'TelemetryDashboardView' is source-declared but unreachable from the current navigation composition.
- Embedded agent manifests contain unavailable or prohibited tool references; Windows normalization must preserve policy intent without those references.

The JSON inventories beside this report are the machine-readable G02 baseline. 'Test-P02Baseline.ps1' re-hashes immutable sources and checks all structural counts and anchors.