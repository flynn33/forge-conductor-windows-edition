# Tool discovery microtask

Tool discovery occurs only after `BOOT-WORKSPACE-000` has passed.

1. Verify workspace lock again.
2. Enumerate tools visible in the current LM Studio project.
3. Match them by description to `TOOL_CAPABILITY_MAP.json`.
4. Record exact plugin ID, function name, argument schema, read/write risk, and harmless probe.
5. Run harmless probes inside the locked target only.
6. Persist verified bindings.
7. Record missing capabilities as exact blockers.

Harmless probes:

- filesystem: list locked target root and read `VERSION` or `AGENTS.md`;
- shell: print locked target and `git --version`;
- RAG: retrieve “workspace lock active assignment repository canonical state”;
- GitHub: read account/repository metadata only, no mutation;
- JS sandbox: pure JSON round trip;
- long-term memory: write/read/delete `<memory_namespace>::probe::<nonce>`;
- persistent memory: write/read/delete the same namespaced ephemeral probe.

Do not invoke Forge’s product-under-test memory or continuity tools as orchestration probes. Never leave probe data outside the run namespace.
