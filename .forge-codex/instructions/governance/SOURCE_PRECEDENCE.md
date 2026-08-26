# Source precedence and conflict resolution

## Precedence

1. Owner requirements.
2. Port task contract.
3. Forsetti Agentic Edition governance.
4. Forsetti Windows sealed-runtime/public-API rules.
5. macOS source behavior and tests.
6. Package architecture and plans.
7. implementation choices.

## Interpretation

The macOS source is authoritative for user-visible behavior, MCP names and schemas, persistence semantics, continuity semantics, error behavior, settings, CLI commands, telemetry meaning, and feature presence. It is not authoritative for Apple-specific APIs or known defects.

The Forsetti Windows repository is authoritative for module boundaries, capabilities, manifests, runtime requirements, host composition, public APIs, and framework immutability.

Forsetti Agentic Edition is governance only. It must not become a runtime dependency.

## Conflict handling

When two lower-precedence sources conflict:

1. Collect exact evidence.
2. Determine whether the difference is platform-specific or a behavioral inconsistency.
3. Prefer feature preservation and the safer/bounded behavior.
4. Create an ADR.
5. Update tests and parity rows.
6. Continue without asking the operator unless the higher-precedence contract truly cannot resolve the issue.

Never copy a macOS implementation defect merely to preserve implementation-level similarity.
