# Telemetry, gauges, and rendering

## Collectors

Use documented Windows APIs behind independent collectors:

- CPU: `GetSystemTimes` and/or PDH.
- RAM: `GlobalMemoryStatusEx`.
- disks/volumes: volume APIs and bounded PDH/IOCTL measurements.
- GPU memory: DXGI adapter memory queries.
- GPU engine utilization: PDH where available, with a typed unavailable state.
- processes: Tool Help/PSAPI/documented process APIs.
- power: Windows power APIs when available.
- LM Studio and Forge process classification through verified image identity.

Never map an unavailable metric to zero without an availability flag.

## Sampling

- Manager owns collection.
- Default visible cadence: 2 Hz.
- Constrained profile: 1 Hz.
- Hidden/occluded UI: no render loop; manager may remain at maintenance cadence.
- Collectors have deadlines and cannot overlap themselves.
- Slow collectors retain the previous value and report staleness.

## Latest-value mailbox

Telemetry publication has capacity one:

- producer replaces the pending snapshot;
- exactly one UI dispatch may be outstanding;
- UI consumes the newest snapshot;
- intermediate snapshots are intentionally dropped;
- shutdown cancels delivery and releases captures.

Do not enqueue one UI callback per sample.

## Histories

All histories are ring buffers. Defaults and hard maximums are in `plans/resource-budgets.json`. Downsample older data. Persist only explicitly required history.

## Graphics

Create one process-owned `GraphicsDeviceService`:

- one D3D11 device/context;
- shared D2D factory/device;
- shared DirectWrite factory;
- shared shader, brush, geometry, and text-format caches;
- render surfaces per view only where required.

Do not create a D3D device, command queue, pipeline, or immutable resources per gauge.

Render on value change or bounded animation ticks. Stop when hidden, minimized, occluded, disconnected, or in energy-saver mode. Handle device loss by recreating shared resources once.

## Verification

Use ETW/WPR/WPA, GPUView or PIX where available, frame timing, queue depth, private bytes, handle counts, and one-hour stress. A gauge must not allocate a replacement buffer or device every frame.
