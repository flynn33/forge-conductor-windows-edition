# Official guidance used for this profile

- Qwen-Agent describes Qwen tool/function calling, MCP, RAG, multi-step calls, a recommended `nous` function-call template for Qwen3 where applicable, `top_p: 0.8`, and an explicit maximum input-token setting.
- LM Studio documents MCP hosting, warns that excessive MCP context can cause overflows, supports RAG over attached documents, and provides a native v1 REST API with stateful chats and configured MCP plugin integrations.
- LM Studio recommends a context above roughly 25k tokens for tool-heavy coding agents when hardware/model support permits.

References:

- https://github.com/QwenLM/Qwen-Agent
- https://github.com/QwenLM/Qwen3
- https://lmstudio.ai/docs/app/mcp
- https://lmstudio.ai/docs/app/basics/rag
- https://lmstudio.ai/docs/developer/rest
- https://lmstudio.ai/docs/developer/rest/chat
- https://lmstudio.ai/docs/developer/rest/stateful-chats
- https://lmstudio.ai/docs/integrations/codex
