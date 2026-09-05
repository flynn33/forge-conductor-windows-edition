# Telemetry and rendering

Native collectors use documented APIs behind independent interfaces: CPU/PDH, memory/process APIs, DXGI and PDH GPU counters when available, disk/volume APIs, IP Helper, power APIs, bounded process discovery, LM Studio/Forge identity. Missing metrics are typed unavailable/stale, never fabricated zero.

Pipeline:

```text
collectors -> normalized immutable sample -> resource policy -> fixed histories
-> capacity-one latest mailbox -> manager stream/view model -> shared renderer
```

At most one UI delivery and one replaceable latest snapshot. Coalesced/dropped/latency/high-water counters are observable. Fixed-capacity ring histories downsample old data.

One process-owned D3D11/D2D/DirectWrite/Composition graph, shared immutable resources and persistent buffers. No per-gauge device/context/swapchain/pipeline/timer. At most one frame request pending. Hidden/minimized/detached surfaces render zero recurring frames. Handle DPI/theme/high contrast/device loss/suspend/display changes explicitly.
