# Optional LM Studio API autonomous mode

LM Studio’s native `/api/v1/chat` endpoint can use configured MCP plugins and stateful chats. The supplied PowerShell controller:

- asserts the exact workspace lock before reading model/plugin configuration;
- generates one lock-bound ACTIVE_ASSIGNMENT and step card;
- starts a fresh chat for that microtask;
- permits at most two stateful continuations using `previous_response_id`;
- checks repository state/handoff progress after each response;
- starts a fresh context for the next microtask;
- stops on ledger corruption, missing required plugins, repeated no-progress, or final completion.

This mode requires LM Studio local API authentication and exact plugin IDs. It never guesses credentials or enables unrelated tools. Chat mode remains supported when API prerequisites are absent.

## Per-microtask least-context binding

Each microtask declares `required_capabilities`. The API controller includes only the matching plugin integrations. After `BOOT-TOOLS-001`, verified tool names become `allowed_tools` allowlists; until then, the exact configured plugin is available only for its declared capability. This reduces prompt/tool-schema load for the 27B 4-bit model.

## Unattended resolution

`Resolve-LMStudioRunnerConfig.ps1` queries `GET /api/v1/models`, selects exactly one Qwen/27B/approximately-4-bit candidate, records its reported quantization/context metadata, maps one `mcp.json` server candidate to each declared capability, and blocks on ambiguity rather than guessing. `BOOT-TOOLS-001` still performs harmless tool probes before bindings become verified.
