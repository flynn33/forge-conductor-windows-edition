# LM Studio tool binding

## Project separation

Create a dedicated LM Studio project/chat configuration for the Windows port. Enable only the seven external capabilities declared by the user:

- shell access;
- filesystem read/write;
- GitHub repository tools;
- RAG-v1;
- JavaScript code sandbox;
- long-term memory;
- persistent memory.

Do not enable a running Forge Conductor build’s own memory or continuity MCP as Qwen’s orchestration memory. It is the product under test and can contain legacy/global project state.

## Lock-first permissions

For `BOOT-WORKSPACE-000`, allow only filesystem and shell tools. After it passes, the controller derives a per-microtask plugin/tool allowlist from `required_capabilities` and verified bindings.

## Binding evidence

Store exact plugin IDs and tool names in `.forge-qwen/state/tool-bindings.json`. Never assume a plugin name from documentation or memory. The runner refuses placeholders.

## Quantized-model tool minimization

The controller exposes only the active microtask's `required_capabilities`. Persistent memory is normally the only memory plugin used for a cursor. Long-term memory is exposed only for selected architecture, continuity, or final-validation assignments. `BOOT-WORKSPACE-000` receives only filesystem and shell.
