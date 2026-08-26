# MCP server and LM Studio integration

## Transport contract

Implement JSON-RPC 2.0 over stdio with exactly one compact UTF-8 JSON object followed by `\n` per frame.

- No LSP `Content-Length`.
- No stdout logging.
- Flush every response.
- Bounded input line and parsed-object size.
- Supported methods include `initialize`, `notifications/initialized`, `tools/list`, and `tools/call`.
- Unknown methods return standard JSON-RPC errors.
- Tool failures return stable Forge error payloads without crashing the server.
- EOF, parent exit, cancellation, or broken pipe triggers deterministic shutdown.

## Roles

Support independent `primary` and `fallback` registrations with distinct server names, deployment IDs, health, logs, and process identities.

## Windows LM Studio discovery

Do not guess configuration or executable locations. Implement `ILMStudioEnvironment` that:

1. checks explicit configuration;
2. inspects installed application registrations and known user-scoped locations;
3. inspects running process image paths when permitted;
4. validates candidate configuration by parsing it;
5. preserves evidence of the selected path.

Never edit a file merely because its path resembles the macOS path.

## Transactional deployment

- independently smoke the selected binary in primary and fallback roles;
- parse the current MCP configuration;
- preserve foreign servers and unknown fields;
- stage both Forge entries;
- write a new shared deployment revision;
- atomically replace configuration with backup/rollback;
- verify both roles after commit;
- activate/reload through supported host mechanisms only;
- wait for host evidence when available;
- report degraded states honestly.

Do not use GUI automation.

## Tool parity

`plans/mcp-tool-parity.json` is the canonical 53-tool list. Names and compatible schemas must not change. Legacy tools remain available alongside project-scoped memory and continuity tools.
