# RAG-v1 playbook

RAG is prohibited until the exact current directory passes workspace-lock verification. RAG never selects a repository or handoff.

RAG reduces context, but retrieved text is not automatically current or complete.

## Query construction

Include:

- exact phase/microtask ID;
- repository and subsystem;
- exact class, method, tool, route, table, metric, or manifest name;
- behavior to preserve;
- expected source path or terminology;
- “macOS source”, “Forsetti public contract”, “audit finding”, or “Windows target” as appropriate.

Example:

```text
Forge Conductor P14 MCP stdio tool list exact names input schemas ToolRouter MCPServer macOS source Forsetti Windows public boundary
```

## Retrieval sequence

1. governing rule query;
2. macOS behavior/source query;
3. audit anti-regression query;
4. Windows architecture/acceptance query;
5. direct filesystem verification of retrieved paths/symbols.

## Limits

Use at most three focused RAG queries before direct file inspection. Do not paste large retrieved chunks into state. Store query, document IDs/paths, and concise findings. When RAG conflicts with current files, current verified files win and the stale retrieval is recorded.
