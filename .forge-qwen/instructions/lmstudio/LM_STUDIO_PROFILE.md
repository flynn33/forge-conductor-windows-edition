# LM Studio profile

The exact model label is owner-supplied. Verify the loaded model through LM Studio APIs/CLI and record the exact ID, architecture, quantization, maximum context, loaded context, runtime, GPU offload, and API version.

Recommended starting settings for deterministic coding/tool use:

| Setting | Start value |
|---|---:|
| Context length | 32,768 or largest stable value; avoid less than 24,576 for tool-heavy operation |
| Temperature | 0.2 |
| Top P | 0.8 |
| Top K | 20 |
| Repeat penalty | 1.05 |
| Max output | 6,144 |
| Reasoning | low for routine edits; medium for architecture/debugging; never leave high reasoning on for repetitive tool work |

Do not assume a parser/template name. Use LM Studio’s model-native tool-call configuration; when an explicit Qwen/Nous function-call option is available, smoke-test it before adopting.

MCP tools consume context. Enable only the seven required capability plugins for the API controller and avoid unrelated MCP servers.
