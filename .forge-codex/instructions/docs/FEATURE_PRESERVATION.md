# Feature preservation workflow

## Baseline

The macOS archive is the behavioral baseline. Before implementation, Codex must regenerate:

- source-file inventory;
- UI/navigation inventory;
- CLI command inventory;
- MCP tool and schema inventory;
- persistence/table inventory;
- configuration inventory;
- telemetry field inventory;
- manager/deployment inventory;
- tests and fixtures inventory.

Compare the regenerated results with `plans/feature-parity-matrix.tsv` and `plans/mcp-tool-parity.json`.

## Parity states

Each row tracks:

- source evidence;
- Windows design;
- implementation files;
- tests;
- runtime evidence;
- status;
- deviations/ADR.

No row may be silently deleted. Add newly discovered rows.

## Equivalent, not identical

Platform-specific implementation may differ, but observable behavior, error semantics, bounds, persistence, security, and automation must remain equivalent or improve. Apple APIs are replaced with Windows-native APIs. Known macOS defects are explicitly prohibited from being carried forward.
