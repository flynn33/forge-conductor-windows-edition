# Forge Conductor Windows 11 Qwen guided v2 package

This package is optimized for Qwen3.8-27B 4-bit in LM Studio and adds a lock-first supervisor to prevent cross-project continuity and memory contamination. Qwen is no longer asked to determine the repository or prior work state. Bootstrap creates one isolated Windows target, one run fingerprint, one memory namespace, one active assignment, and one numbered step card.

See `GUIDED-V2-CHANGES.md`, `READ-THIS-FIRST-QWEN.txt`, and `START-HERE.md`.

This package directs a local Qwen3.8-27B 4-bit model running in LM Studio to build, test, debug, package, install, and independently validate a native Windows 11 edition of Forge Conductor.

It is optimized for a quantized local model by using:

- one bounded microtask per fresh model context;
- capability-based tool discovery instead of assumed function names;
- RAG-first retrieval followed by direct file verification;
- repository files as canonical state;
- long-term and persistent memory only as compact indexes/caches;
- small edit budgets and immediate compile/test feedback;
- fixed status/result schemas;
- anti-hallucination labels: `VERIFIED`, `INFERRED`, `UNKNOWN`, `BLOCKED`;
- automatic handoffs and optional LM Studio API-driven fresh-session execution;
- separate Builder and Validator contexts.

The package contains the exact macOS, Forsetti Framework Windows, and Forsetti Agentic Edition archives, plus the embedded audit evidence from the macOS source package.

The target product remains native object-oriented C++20 with WinUI 3/C++/WinRT, Microsoft Windows development tools, PowerShell automation, Winsqlite3, DirectX, ETW, MSIX, and no Python.