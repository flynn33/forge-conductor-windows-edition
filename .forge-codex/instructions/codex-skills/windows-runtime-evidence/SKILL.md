---
name: forge-windows-runtime-evidence
description: Prove Windows lifecycle, memory, telemetry, GPU, IPC, and installer behavior.
---

- Reproduce an exact flow in Release configuration.
- Capture process private bytes, working set, handles, threads, CPU, GPU, queue depth, and relevant ETW events.
- Use WPR/WPA, Application Verifier, PageHeap, ASan, WinDbg, GPUView, or PIX where appropriate.
- For leaks, identify the owning object/resource and retaining path or unbounded mechanism.
- Repeat the same flow after repair.
- For installer claims, use a clean user profile and preserve deployment logs.
- Store all evidence under `.forge-codex/state/` with hashes and environment metadata.
